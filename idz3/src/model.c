#include "center.h"

#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static unsigned int random_number(Parcel *parcel) {
    parcel->random = parcel->random * 1664525U + 1013904223U;
    return parcel->random;
}

static unsigned long long elapsed_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long long ns = (long long)(now.tv_sec - shared->start_time.tv_sec) * 1000000000LL
                 + now.tv_nsec - shared->start_time.tv_nsec;
    return (unsigned long long)(ns / 1000000LL);
}

void model_stop(int number) {
    shared->stopping = 1;
    if (number && !shared->stop_signal) {
        shared->stop_signal = number;
    }
}

void model_fail(const char *text) {
    shared->model_error = 1;
    snprintf(shared->error_text, sizeof(shared->error_text), "%s", text);
    model_stop(0);
}

static int wait_locked(int counter) {
    while (!shared->stopping) {
        model_unlock();
        int result = ipc_wait(counter, 100);
        model_lock();
        if (result < 0) {
            model_fail("Ошибка ожидания семафора");
        }
        if (result > 0) {
            return !shared->stopping;
        }
    }
    return 0;
}

static void post_locked(int counter) {
    if (ipc_post(counter) < 0) {
        model_fail("Ошибка освобождения семафора");
    }
}

static int pause_locked(int units) {
    unsigned long long remaining = (unsigned long long)units * config.delay_ms;
    // задержки выполняются без блокировки, остановку проверяем каждые 20 мс
    do {
        unsigned long long part = remaining > 20 ? 20 : remaining;
        struct timespec pause = {0, (long)part * 1000000L};
        int result = 0;
        model_unlock();
        if (part == 0) {
            sched_yield();
        } else {
            do {
                result = nanosleep(&pause, &pause);
            } while (result < 0 && errno == EINTR);
        }
        model_lock();
        if (result < 0) {
            model_fail("Ошибка задержки");
        }
        if (shared->stopping) {
            return 0;
        }
        remaining -= part;
    } while (remaining > 0);
    return 1;
}

static int mark_place(int *seen, int id, enum Stage stage) {
    if (id < 0 || id >= config.parcels || shared->parcels[id].stage != stage) {
        return 0;
    }
    seen[id]++;
    return seen[id] == 1;
}

static int check_queue(Queue *queue, enum Stage stage, int *seen) {
    if (queue->count < 0 || queue->count > queue->limit ||
        queue->head < 0 || queue->head >= MAX_PARCELS) {
        return 0;
    }
    for (int i = 0; i < queue->count; i++) {
        int id = queue->items[(queue->head + i) % MAX_PARCELS];
        if (!mark_place(seen, id, stage) || shared->parcels[id].owner != -1 ||
            (queue->direction >= 0 && shared->parcels[id].direction != queue->direction)) {
            return 0;
        }
    }
    return 1;
}

int check_model(void) {
    int seen[MAX_PARCELS] = {0};
    if (!check_queue(&shared->queues[0], INPUT, seen) ||
        !check_queue(&shared->queues[1], SORT_QUEUE, seen) ||
        !check_queue(&shared->queues[2], MANUAL, seen)) {
        return 0;
    }
    for (int i = 0; i < config.directions; i++) {
        if (!check_queue(&shared->queues[3 + 2 * (i)], BELT_QUEUE, seen) ||
            !check_queue(&shared->queues[4 + 2 * (i)], OUTPUT, seen)) {
            return 0;
        }
    }
    for (int i = 0; i < config.lines; i++) {
        int id = shared->sources[i].parcel;
        if (id != -1 && (!mark_place(seen, id, SOURCE) || shared->parcels[id].source != i)) {
            return 0;
        }
    }
    for (int i = 0; i < shared->device_count; i++) {
        Device *device = &shared->devices[i];
        int id = device->parcel;
        if (id == -1) {
            continue;
        }
        enum Stage stage = device->role == SCANNER ? SCAN :
                           device->role == SORTER ? SORT : BELT;
        if (id < 0 || id >= config.parcels || shared->parcels[id].owner != i ||
            (shared->parcels[id].stage != stage && shared->parcels[id].stage != stage + 1) ||
            !mark_place(seen, id, shared->parcels[id].stage)) {
            return 0;
        }
    }
    int count = 0, good = 0, bad = 0;
    for (int i = 0; i < config.parcels; i++) {
        Parcel *parcel = &shared->parcels[i];
        if (parcel->id != i + 1 || parcel->attempts > config.retries + 1) {
            return 0;
        }
        enum Stage stage = parcel->stage;
        int needs_place = stage != NEW && stage != DONE && stage != REJECTED;
        if (seen[i] != needs_place) {
            return 0;
        }
        if (((stage >= SORT_QUEUE && stage <= OUTPUT) || stage == DONE) &&
            !parcel->recognized) {
            return 0;
        }
        count += stage != NEW;
        good += stage == OUTPUT || stage == DONE;
        bad += stage == MANUAL || stage == REJECTED;
    }
    return count == shared->formed && good == shared->delivered && bad == shared->rejected;
}

static void event(int id, const char *name, const char *text) {
    char line[1024];
    shared->sequence++;
    snprintf(line, sizeof(line),
             "[seq=%llu time_ms=%llu] parcel=%d event=%s %s pid=%ld\n",
             shared->sequence, elapsed_ms(), shared->parcels[id].id, name, text, (long)getpid());
    print_text(line);
    if (output_error) {
        model_fail("Ошибка записи вывода или журнала");
    } else if (config.check && !check_model()) {
        model_fail("Нарушен инвариант модели");
    }
}

static int put(Queue *queue, int id, enum Stage stage, const char *name) {
    char text[256];
    if (queue->count == queue->limit) {
        shared->waits++;
        snprintf(text, sizeof(text), "queue=%s direction=%d Накопитель заполнен",
                 queue->name, queue->direction + 1);
        event(id, "WAIT", text);
    }
    if (!wait_locked(queue->free_slots)) {
        return 0;
    }
    if (queue->count >= queue->limit) {
        model_fail("Переполнение накопителя");
        return 0;
    }
    Parcel *parcel = &shared->parcels[id];
    if (parcel->stage == SOURCE) {
        shared->sources[parcel->source].parcel = -1;
    } else {
        Device *device = &shared->devices[parcel->owner];
        device->parcel = -1;
        device->repeat = 0;
        post_locked(device->released);
    }
    parcel->owner = -1;
    parcel->stage = stage;
    int tail = (queue->head + queue->count) % MAX_PARCELS;
    queue->items[tail] = id;
    queue->count++;
    shared->delivered += stage == OUTPUT;
    shared->rejected += stage == MANUAL;
    snprintf(text, sizeof(text), "queue=%s direction=%d count=%d capacity=%d Переход в накопитель",
             queue->name, parcel->direction + 1, queue->count, queue->limit);
    event(id, name, text);
    post_locked(queue->used_slots);
    return !shared->stopping;
}

static int take(Queue *queue, int owner, enum Stage stage) {
    if (!wait_locked(queue->used_slots)) {
        return -1;
    }
    if (queue->count <= 0) {
        model_fail("Извлечение из пустого накопителя");
        return -1;
    }
    int id = queue->items[queue->head];
    queue->head = (queue->head + 1) % MAX_PARCELS;
    queue->count--;
    shared->parcels[id].stage = stage;
    shared->parcels[id].owner = owner;
    if (owner >= 0) {
        shared->devices[owner].parcel = id;
    }
    post_locked(queue->free_slots);
    return id;
}

static void source_work(int index) {
    Source *source = &shared->sources[index];
    model_lock();
    for (int id = source->id; id < config.parcels && !shared->stopping; id += config.lines) {
        int interval = 1 + (int)(random_number(&shared->parcels[id]) % config.arrival_interval);
        if (id >= config.lines && !pause_locked(interval)) {
            break;
        }
        source->parcel = id;
        shared->parcels[id].stage = SOURCE;
        shared->formed++;
        char text[128];
        snprintf(text, sizeof(text), "source=%d Посылка поступила на входную линию", source->id + 1);
        event(id, "ARRIVE", text);
        if (!put(&shared->queues[0], id, INPUT, "INPUT")) {
            break;
        }
    }
    model_unlock();
    return;
}

static void device_work(int index) {
    Device *device = &shared->devices[index];
    model_lock();
    while (!shared->stopping) {
        enum Stage stage = device->role == SCANNER ? SCAN :
                           device->role == SORTER ? SORT : BELT;
        int id = take(&shared->queues[device->input], device->id, stage);
        if (id < 0) {
            break;
        }
        Parcel *parcel = &shared->parcels[id];
        do {
            device->repeat = 0;
            parcel->stage = stage;
            int duration = device->role == SCANNER ? config.scan_time : config.sort_time;
            const char *start = device->role == SCANNER ? "SCAN_START" : "SORT_START";
            const char *end = device->role == SCANNER ? "SCAN_END" : "SORT_END";
            if (device->role == CONVEYOR) {
                duration = (config.conveyor_length + config.conveyor_speed - 1) / config.conveyor_speed;
                start = "TRANSPORT_START";
                end = "TRANSPORT_END";
            }
            if (device->role == SCANNER) {
                parcel->attempts++;
                shared->attempts++;
            }
            char text[256];
            snprintf(text, sizeof(text), "device=%d direction=%d attempt=%d duration_ms=%llu Начало обработки",
                     device->id + 1, parcel->direction + 1, parcel->attempts,
                     (unsigned long long)duration * config.delay_ms);
            event(id, start, text);
            if (shared->stopping || !pause_locked(duration)) {
                break;
            }
            parcel->stage = stage + 1;
            event(id, end, "Обработка устройством завершена");
            if (device->role == SCANNER) {
                parcel->recognized = random_number(parcel) % 100 >= (unsigned int)config.error_percent;
                if (parcel->recognized) {
                    snprintf(text, sizeof(text), "direction=%d Направление распознано", parcel->direction + 1);
                    event(id, "RECOGNIZED", text);
                } else {
                    shared->errors++;
                    event(id, "SCAN_ERROR", "Ошибка чтения маркировки");
                }
            }
            post_locked(parcel->ready);
            // устройство остаётся занятым, пока посылка не перейдёт дальше
            if (!wait_locked(device->released)) {
                break;
            }
        } while (device->repeat && !shared->stopping);
    }
    model_unlock();
    return;
}

static void parcel_work(int index) {
    Parcel *parcel = &shared->parcels[index];
    int id = parcel->id - 1;
    model_lock();
    while (wait_locked(parcel->ready)) {
        if (parcel->recognized) {
            break;
        }
        if (parcel->attempts > config.retries) {
            put(&shared->queues[2], id, MANUAL, "REJECT");
            model_unlock();
            return;
        }
        // решение о повторе и переходах принимает процесс самой посылки
        shared->repeats++;
        shared->devices[parcel->owner].repeat = 1;
        event(id, "RETRY", "Запрошено повторное чтение маркировки");
        post_locked(shared->devices[parcel->owner].released);
    }
    if (!shared->stopping && put(&shared->queues[1], id, SORT_QUEUE, "SORT_QUEUE") &&
        wait_locked(parcel->ready) &&
        put(&shared->queues[3 + 2 * (parcel->direction)], id, BELT_QUEUE, "CONVEYOR_QUEUE") &&
        wait_locked(parcel->ready)) {
        put(&shared->queues[4 + 2 * (parcel->direction)], id, OUTPUT, "DELIVER");
    }
    model_unlock();
    return;
}

static void unload_work(int index) {
    Queue *queue = index == config.directions ? &shared->queues[2] : &shared->queues[4 + 2 * index];
    model_lock();
    while (!shared->stopping && pause_locked(config.unload_interval)) {
        while (queue->count > 0 && !shared->stopping) {
            int is_manual = queue == &shared->queues[2];
            int id = take(queue, -1, is_manual ? REJECTED : DONE);
            if (id < 0) {
                break;
            }
            event(id, is_manual ? "MANUAL_UNLOAD" : "UNLOAD", "Накопитель освобождён");
        }
    }
    model_unlock();
    return;
}

static int init_queue(int index, int limit, const char *name, int direction) {
    Queue *queue = &shared->queues[index];
    queue->limit = limit;
    queue->direction = direction;
    snprintf(queue->name, sizeof(queue->name), "%s", name);
    queue->free_slots = ipc_counter(limit);
    queue->used_slots = ipc_counter(0);
    return queue->free_slots >= 0 && queue->used_slots >= 0;
}

static void add_job(enum Job role, int index) {
    Worker *worker = &shared->workers[shared->worker_count++];
    worker->role = role;
    worker->index = index;
}

const char *job_name(enum Job role) {
    if (role == PARCEL_JOB) return "parcel";
    if (role == SOURCE_JOB) return "source";
    if (role == DEVICE_JOB) return "device";
    return "unload";
}

int prepare_model(void) {
    shared->start_gate = ipc_counter(0);
    shared->queue_count = 3 + 2 * config.directions;
    if (shared->start_gate < 0 ||
        !init_queue(0, config.buffer_capacity, "input", -1) ||
        !init_queue(1, config.buffer_capacity, "sort", -1) ||
        !init_queue(2, config.buffer_capacity, "manual", -1)) {
        return 0;
    }
    for (int i = 0; i < config.directions; i++) {
        if (!init_queue(3 + 2 * i, config.buffer_capacity, "belt", i) ||
            !init_queue(4 + 2 * i, config.output_capacity, "output", i)) {
            return 0;
        }
    }
    for (int i = 0; i < config.parcels; i++) {
        Parcel *parcel = &shared->parcels[i];
        parcel->id = i + 1;
        parcel->owner = -1;
        parcel->source = i % config.lines;
        parcel->random = config.seed + (unsigned int)i * 2654435761U;
        parcel->direction = (int)(random_number(parcel) % config.directions);
        parcel->ready = ipc_counter(0);
        if (parcel->ready < 0) return 0;
        add_job(PARCEL_JOB, i);
    }
    for (int i = 0; i < config.lines; i++) {
        shared->sources[i].id = i;
        shared->sources[i].parcel = -1;
        add_job(SOURCE_JOB, i);
    }
    shared->device_count = config.scanners + config.sorters + config.directions;
    for (int i = 0; i < shared->device_count; i++) {
        Device *device = &shared->devices[i];
        device->id = i;
        device->parcel = -1;
        if (i < config.scanners) {
            device->role = SCANNER;
            device->input = 0;
        } else if (i < config.scanners + config.sorters) {
            device->role = SORTER;
            device->input = 1;
        } else {
            device->role = CONVEYOR;
            device->input = 3 + 2 * (i - config.scanners - config.sorters);
        }
        device->released = ipc_counter(0);
        if (device->released < 0) return 0;
        add_job(DEVICE_JOB, i);
    }
    for (int i = 0; i <= config.directions; i++) {
        add_job(UNLOAD_JOB, i);
    }
    return 1;
}

int run_worker(int slot) {
    Worker *worker = &shared->workers[slot];
    model_lock();
    if (shared->stopping) {
        model_unlock();
        return 0;
    }
    if (worker->pid != 0 || shared->running) {
        model_unlock();
        fprintf(stderr, "Роль занята или сеанс уже остановлен\n");
        return 1;
    }
    worker->pid = getpid();
    shared->registered++;
    char line[200];
    snprintf(line, sizeof(line), "WORKER role=%s index=%d pid=%ld parent=%ld\n",
             job_name(worker->role), worker->index + 1, (long)getpid(), (long)getppid());
    print_text(line);
    if (output_error) model_fail("Ошибка записи журнала участника");
    int started = wait_locked(shared->start_gate);
    model_unlock();
    if (started) {
        if (worker->role == PARCEL_JOB) parcel_work(worker->index);
        else if (worker->role == SOURCE_JOB) source_work(worker->index);
        else if (worker->role == DEVICE_JOB) device_work(worker->index);
        else unload_work(worker->index);
    }
    model_lock();
    if (close_log() < 0) model_fail("Ошибка закрытия журнала участника");
    worker->finished = 1;
    model_unlock();
    return output_error ? 1 : 0;
}

void print_config(void) {
    char line[1800];
    snprintf(line, sizeof(line),
        "CONFIG parcels=%d directions=%d lines=%d scanners=%d sorters=%d "
        "buffer_capacity=%d output_capacity=%d arrival_interval=%d scan_time=%d sort_time=%d "
        "conveyor_length=%d conveyor_speed=%d unload_interval=%d error_percent=%d "
        "retries=%d seed=%u delay_ms=%d check=%d sync=%s controller=%ld\n",
        config.parcels, config.directions, config.lines, config.scanners, config.sorters,
        config.buffer_capacity, config.output_capacity, config.arrival_interval,
        config.scan_time, config.sort_time, config.conveyor_length, config.conveyor_speed,
        config.unload_interval, config.error_percent, config.retries, config.seed,
        config.delay_ms, config.check, shared->named ? "named" : "unnamed", (long)getpid());
    print_text(line);
}

int print_result(void) {
    if (!check_model()) model_fail("Нарушен итоговый баланс модели");
    const char *status = shared->model_error || output_error ? "error" :
                         shared->stop_signal ? "interrupted" : "completed";
    char line[1024];
    snprintf(line, sizeof(line),
        "ИТОГ status=%s planned=%d formed=%d delivered=%d rejected=%d unfinished=%d "
        "not_generated=%d scan_attempts=%llu errors=%llu repeats=%llu waits=%llu "
        "time_ms=%llu processes=%d signal=%d\n",
        status, config.parcels, shared->formed, shared->delivered, shared->rejected,
        shared->formed - shared->delivered - shared->rejected, config.parcels - shared->formed,
        shared->attempts, shared->errors, shared->repeats, shared->waits, elapsed_ms(),
        shared->registered + 1, shared->stop_signal);
    print_text(line);
    if (shared->error_text[0]) fprintf(stderr, "%s\n", shared->error_text);
    if (shared->model_error || output_error) return 1;
    return shared->stop_signal ? 128 + shared->stop_signal : 0;
}
