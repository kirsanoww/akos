#include "center.h"
#include "sync.h"

#include <errno.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_DEVICES 64
#define MAX_PARCELS 512
#define MAX_COUNTERS 1100

enum Stage {
    NEW, SOURCE, INPUT, SCAN, SCANNED, SORT_QUEUE, SORT, SORTED,
    BELT_QUEUE, BELT, TRANSPORTED, OUTPUT, MANUAL, DONE, REJECTED
};
enum Role { SCANNER, SORTER, CONVEYOR };

typedef struct {
    int id, direction, attempts, recognized, source, owner;
    unsigned int random;
    enum Stage stage;
    Counter ready;
    pthread_t thread;
    int started;
} Parcel;

typedef struct {
    int *items;
    int count, limit, head, direction;
    const char *name;
    Counter free_slots, used_slots;
} Queue;

typedef struct {
    int id, parcel, repeat;
    enum Role role;
    Queue *input;
    Counter released;
    pthread_t thread;
    int started;
} Device;

typedef struct {
    int id, parcel, started;
    pthread_t thread;
} Source;

typedef struct {
    Queue *queue;
    int started;
    pthread_t thread;
} Unloader;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static Parcel *parcels;
static Queue input, sort_queue, manual;
static Queue belt_queues[MAX_DEVICES], outputs[MAX_DEVICES];
static Device devices[3 * MAX_DEVICES];
static Source sources[MAX_DEVICES];
static Unloader unloaders[MAX_DEVICES + 1];
static Counter *counters[MAX_COUNTERS];
static int counter_count, device_count, created_threads;
static int stopping, model_error, stop_signal, monitor_done;
static int formed, delivered, rejected;
static unsigned long long attempts, errors, repeats, waits, sequence;
static struct timespec start_time;
static const char *error_text;

static unsigned int random_number(Parcel *parcel) {
    parcel->random = parcel->random * 1664525U + 1013904223U;
    return parcel->random;
}

static unsigned long long elapsed_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long long ns = (long long)(now.tv_sec - start_time.tv_sec) * 1000000000LL
                 + now.tv_nsec - start_time.tv_nsec;
    return (unsigned long long)(ns / 1000000LL);
}

// состояние, очереди и строки журнала защищены одним коротким мьютексом
static void stop_locked(void) {
    if (!stopping) {
        stopping = 1;
        for (int i = 0; i < counter_count; i++) {
            counter_close(counters[i]);
        }
    }
}

static void fail_locked(const char *text) {
    model_error = 1;
    error_text = text;
    stop_locked();
}

static int wait_locked(Counter *counter) {
    if (stopping) {
        return 0;
    }
    int result = counter_wait(counter, &lock);
    if (result < 0) {
        fail_locked("Ошибка ожидания семафора");
    }
    return result > 0 && !stopping;
}

static void post_locked(Counter *counter) {
    if (counter_post(counter) < 0) {
        fail_locked("Ошибка освобождения семафора");
    }
}

static int pause_locked(int units) {
    unsigned long long remaining = (unsigned long long)units * config.delay_ms;
    // задержки выполняются без мьютекса, остановку проверяем каждые 20 мс
    do {
        unsigned long long part = remaining > 20 ? 20 : remaining;
        struct timespec pause = {0, (long)part * 1000000L};
        int result = 0;
        pthread_mutex_unlock(&lock);
        if (part == 0) {
            sched_yield();
        } else {
            do {
                result = nanosleep(&pause, &pause);
            } while (result < 0 && errno == EINTR);
        }
        pthread_mutex_lock(&lock);
        if (result < 0) {
            fail_locked("Ошибка задержки");
        }
        if (stopping) {
            return 0;
        }
        remaining -= part;
    } while (remaining > 0);
    return 1;
}

static int mark_place(int *seen, int id, enum Stage stage) {
    if (id < 0 || id >= config.parcels || parcels[id].stage != stage) {
        return 0;
    }
    seen[id]++;
    return seen[id] == 1;
}

static int check_queue(Queue *queue, enum Stage stage, int *seen) {
    if (queue->count < 0 || queue->count > queue->limit ||
        queue->head < 0 || queue->head >= queue->limit) {
        return 0;
    }
    for (int i = 0; i < queue->count; i++) {
        int id = queue->items[(queue->head + i) % queue->limit];
        if (!mark_place(seen, id, stage) || parcels[id].owner != -1 ||
            (queue->direction >= 0 && parcels[id].direction != queue->direction)) {
            return 0;
        }
    }
    return 1;
}

static int check_model(void) {
    int seen[MAX_PARCELS] = {0};
    if (!check_queue(&input, INPUT, seen) ||
        !check_queue(&sort_queue, SORT_QUEUE, seen) ||
        !check_queue(&manual, MANUAL, seen)) {
        return 0;
    }
    for (int i = 0; i < config.directions; i++) {
        if (!check_queue(&belt_queues[i], BELT_QUEUE, seen) ||
            !check_queue(&outputs[i], OUTPUT, seen)) {
            return 0;
        }
    }
    for (int i = 0; i < config.lines; i++) {
        int id = sources[i].parcel;
        if (id != -1 && (!mark_place(seen, id, SOURCE) || parcels[id].source != i)) {
            return 0;
        }
    }
    for (int i = 0; i < device_count; i++) {
        Device *device = &devices[i];
        int id = device->parcel;
        if (id == -1) {
            continue;
        }
        enum Stage stage = device->role == SCANNER ? SCAN :
                           device->role == SORTER ? SORT : BELT;
        if (id < 0 || id >= config.parcels || parcels[id].owner != i ||
            (parcels[id].stage != stage && parcels[id].stage != stage + 1) ||
            !mark_place(seen, id, parcels[id].stage)) {
            return 0;
        }
    }
    int count = 0, good = 0, bad = 0;
    for (int i = 0; i < config.parcels; i++) {
        Parcel *parcel = &parcels[i];
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
    return count == formed && good == delivered && bad == rejected;
}

static void event(int id, const char *name, const char *text) {
    char line[1024];
    sequence++;
    snprintf(line, sizeof(line),
             "[seq=%llu time_ms=%llu] parcel=%d event=%s %s\n",
             sequence, elapsed_ms(), parcels[id].id, name, text);
    print_text(line);
    if (output_error) {
        fail_locked("Ошибка записи вывода или журнала");
    } else if (config.check && !check_model()) {
        fail_locked("Нарушен инвариант модели");
    }
}

static int put(Queue *queue, int id, enum Stage stage, const char *name) {
    char text[256];
    if (queue->count == queue->limit) {
        waits++;
        snprintf(text, sizeof(text), "queue=%s direction=%d Накопитель заполнен",
                 queue->name, queue->direction + 1);
        event(id, "WAIT", text);
    }
    if (!wait_locked(&queue->free_slots)) {
        return 0;
    }
    if (queue->count >= queue->limit) {
        fail_locked("Переполнение накопителя");
        return 0;
    }
    Parcel *parcel = &parcels[id];
    if (parcel->stage == SOURCE) {
        sources[parcel->source].parcel = -1;
    } else {
        Device *device = &devices[parcel->owner];
        device->parcel = -1;
        device->repeat = 0;
        post_locked(&device->released);
    }
    parcel->owner = -1;
    parcel->stage = stage;
    int tail = (queue->head + queue->count) % queue->limit;
    queue->items[tail] = id;
    queue->count++;
    delivered += stage == OUTPUT;
    rejected += stage == MANUAL;
    snprintf(text, sizeof(text), "queue=%s direction=%d count=%d capacity=%d Переход в накопитель",
             queue->name, parcel->direction + 1, queue->count, queue->limit);
    event(id, name, text);
    post_locked(&queue->used_slots);
    return !stopping;
}

static int take(Queue *queue, int owner, enum Stage stage) {
    if (!wait_locked(&queue->used_slots)) {
        return -1;
    }
    if (queue->count <= 0) {
        fail_locked("Извлечение из пустого накопителя");
        return -1;
    }
    int id = queue->items[queue->head];
    queue->head = (queue->head + 1) % queue->limit;
    queue->count--;
    parcels[id].stage = stage;
    parcels[id].owner = owner;
    if (owner >= 0) {
        devices[owner].parcel = id;
    }
    post_locked(&queue->free_slots);
    return id;
}

static void *source_thread(void *data) {
    Source *source = data;
    pthread_mutex_lock(&lock);
    for (int id = source->id; id < config.parcels && !stopping; id += config.lines) {
        int interval = 1 + (int)(random_number(&parcels[id]) % config.arrival_interval);
        if (id >= config.lines && !pause_locked(interval)) {
            break;
        }
        source->parcel = id;
        parcels[id].stage = SOURCE;
        formed++;
        char text[128];
        snprintf(text, sizeof(text), "source=%d Посылка поступила на входную линию", source->id + 1);
        event(id, "ARRIVE", text);
        if (!put(&input, id, INPUT, "INPUT")) {
            break;
        }
    }
    pthread_mutex_unlock(&lock);
    return NULL;
}

static void *device_thread(void *data) {
    Device *device = data;
    pthread_mutex_lock(&lock);
    while (!stopping) {
        enum Stage stage = device->role == SCANNER ? SCAN :
                           device->role == SORTER ? SORT : BELT;
        int id = take(device->input, device->id, stage);
        if (id < 0) {
            break;
        }
        Parcel *parcel = &parcels[id];
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
                attempts++;
            }
            char text[256];
            snprintf(text, sizeof(text), "device=%d direction=%d attempt=%d duration_ms=%llu Начало обработки",
                     device->id + 1, parcel->direction + 1, parcel->attempts,
                     (unsigned long long)duration * config.delay_ms);
            event(id, start, text);
            if (stopping || !pause_locked(duration)) {
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
                    errors++;
                    event(id, "SCAN_ERROR", "Ошибка чтения маркировки");
                }
            }
            post_locked(&parcel->ready);
            // устройство остаётся занятым, пока посылка не перейдёт дальше
            if (!wait_locked(&device->released)) {
                break;
            }
        } while (device->repeat && !stopping);
    }
    pthread_mutex_unlock(&lock);
    return NULL;
}

static void *parcel_thread(void *data) {
    Parcel *parcel = data;
    int id = parcel->id - 1;
    pthread_mutex_lock(&lock);
    while (wait_locked(&parcel->ready)) {
        if (parcel->recognized) {
            break;
        }
        if (parcel->attempts > config.retries) {
            put(&manual, id, MANUAL, "REJECT");
            pthread_mutex_unlock(&lock);
            return NULL;
        }
        // решение о повторе и переходах принимает поток самой посылки
        repeats++;
        devices[parcel->owner].repeat = 1;
        event(id, "RETRY", "Запрошено повторное чтение маркировки");
        post_locked(&devices[parcel->owner].released);
    }
    if (!stopping && put(&sort_queue, id, SORT_QUEUE, "SORT_QUEUE") &&
        wait_locked(&parcel->ready) &&
        put(&belt_queues[parcel->direction], id, BELT_QUEUE, "CONVEYOR_QUEUE") &&
        wait_locked(&parcel->ready)) {
        put(&outputs[parcel->direction], id, OUTPUT, "DELIVER");
    }
    pthread_mutex_unlock(&lock);
    return NULL;
}

static void *unload_thread(void *data) {
    Queue *queue = ((Unloader *)data)->queue;
    pthread_mutex_lock(&lock);
    while (!stopping && pause_locked(config.unload_interval)) {
        while (queue->count > 0 && !stopping) {
            int is_manual = queue == &manual;
            int id = take(queue, -1, is_manual ? REJECTED : DONE);
            if (id < 0) {
                break;
            }
            event(id, is_manual ? "MANUAL_UNLOAD" : "UNLOAD", "Накопитель освобождён");
        }
    }
    pthread_mutex_unlock(&lock);
    return NULL;
}

static void *signal_thread(void *data) {
    (void)data;
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    sigaddset(&signals, SIGUSR1);
    for (;;) {
        int number = 0;
        int result = sigwait(&signals, &number);
        pthread_mutex_lock(&lock);
        if (result != 0) {
            fail_locked("Ошибка sigwait");
        } else if (number == SIGINT || number == SIGTERM) {
            stop_signal = number;
            stop_locked();
        }
        int done = monitor_done || model_error || stop_signal;
        pthread_mutex_unlock(&lock);
        if (done) {
            return NULL;
        }
    }
}

static int init_counter(Counter *counter, int value) {
    if (counter_count >= MAX_COUNTERS || counter_init(counter, value) < 0) {
        return 0;
    }
    counters[counter_count++] = counter;
    return 1;
}

static int init_queue(Queue *queue, int limit, const char *name, int direction) {
    queue->items = calloc((size_t)limit, sizeof(int));
    queue->limit = limit;
    queue->name = name;
    queue->direction = direction;
    return queue->items != NULL && init_counter(&queue->free_slots, limit) &&
           init_counter(&queue->used_slots, 0);
}

static int prepare_model(void) {
    parcels = calloc((size_t)(config.parcels + 1), sizeof(Parcel));
    if (parcels == NULL ||
        !init_queue(&input, config.buffer_capacity, "input", -1) ||
        !init_queue(&sort_queue, config.buffer_capacity, "sort", -1) ||
        !init_queue(&manual, config.buffer_capacity, "manual", -1)) {
        return 0;
    }
    for (int i = 0; i < config.directions; i++) {
        if (!init_queue(&belt_queues[i], config.buffer_capacity, "belt", i) ||
            !init_queue(&outputs[i], config.output_capacity, "output", i)) {
            return 0;
        }
        unloaders[i].queue = &outputs[i];
    }
    unloaders[config.directions].queue = &manual;
    for (int i = 0; i < config.parcels; i++) {
        Parcel *parcel = &parcels[i];
        parcel->id = i + 1;
        parcel->owner = -1;
        parcel->source = i % config.lines;
        parcel->random = config.seed + (unsigned int)i * 2654435761U;
        parcel->direction = (int)(random_number(parcel) % config.directions);
        if (!init_counter(&parcel->ready, 0)) {
            return 0;
        }
    }
    for (int i = 0; i < config.lines; i++) {
        sources[i].id = i;
        sources[i].parcel = -1;
    }
    device_count = config.scanners + config.sorters + config.directions;
    for (int i = 0; i < device_count; i++) {
        Device *device = &devices[i];
        device->id = i;
        device->parcel = -1;
        if (i < config.scanners) {
            device->role = SCANNER;
            device->input = &input;
        } else if (i < config.scanners + config.sorters) {
            device->role = SORTER;
            device->input = &sort_queue;
        } else {
            device->role = CONVEYOR;
            device->input = &belt_queues[i - config.scanners - config.sorters];
        }
        if (!init_counter(&device->released, 0)) {
            return 0;
        }
    }
    return 1;
}

static int start_thread(pthread_t *thread, void *(*function)(void *), void *data) {
    int error = pthread_create(thread, NULL, function, data);
    if (error != 0) {
        fprintf(stderr, "Не удалось создать поток: %s\n", strerror(error));
        fail_locked("Не удалось создать все потоки");
        return 0;
    }
    created_threads++;
    return 1;
}

static void join_thread(pthread_t thread) {
    int error = pthread_join(thread, NULL);
    if (error != 0) {
        // нельзя освобождать общую память, если поток не удалось присоединить
        fprintf(stderr, "Ошибка pthread_join: %s\n", strerror(error));
        exit(1);
    }
}

static void free_model(void) {
    for (int i = 0; i < counter_count; i++) {
        counter_destroy(counters[i]);
    }
    free(input.items);
    free(sort_queue.items);
    free(manual.items);
    for (int i = 0; i < config.directions; i++) {
        free(belt_queues[i].items);
        free(outputs[i].items);
    }
    free(parcels);
}

int run_model(void) {
    clock_gettime(CLOCK_MONOTONIC, &start_time);
    if (!prepare_model()) {
        fprintf(stderr, "Не удалось создать память или семафоры модели\n");
        free_model();
        return 1;
    }
    char line[1536];
    snprintf(line, sizeof(line),
        "CONFIG parcels=%d directions=%d lines=%d scanners=%d sorters=%d "
        "buffer_capacity=%d output_capacity=%d arrival_interval=%d scan_time=%d sort_time=%d "
        "conveyor_length=%d conveyor_speed=%d unload_interval=%d error_percent=%d "
        "retries=%d seed=%u delay_ms=%d check=%d sync=semaphores\n",
        config.parcels, config.directions, config.lines, config.scanners, config.sorters,
        config.buffer_capacity, config.output_capacity, config.arrival_interval,
        config.scan_time, config.sort_time, config.conveyor_length, config.conveyor_speed,
        config.unload_interval, config.error_percent, config.retries, config.seed,
        config.delay_ms, config.check);
    print_text(line);

    pthread_t monitor;
    pthread_mutex_lock(&lock);
    int monitor_started = start_thread(&monitor, signal_thread, NULL);
    if (output_error) {
        fail_locked("Ошибка записи вывода");
    }
    for (int i = 0; i < config.parcels && !stopping; i++) {
        parcels[i].started = start_thread(&parcels[i].thread, parcel_thread, &parcels[i]);
    }
    for (int i = 0; i < config.lines && !stopping; i++) {
        sources[i].started = start_thread(&sources[i].thread, source_thread, &sources[i]);
    }
    for (int i = 0; i < device_count && !stopping; i++) {
        devices[i].started = start_thread(&devices[i].thread, device_thread, &devices[i]);
    }
    for (int i = 0; i <= config.directions && !stopping; i++) {
        unloaders[i].started = start_thread(&unloaders[i].thread, unload_thread, &unloaders[i]);
    }
    snprintf(line, sizeof(line), "THREADS created=%d parcels=%d sources=%d devices=%d unloaders=%d signal=%d\n",
             created_threads, config.parcels, config.lines, device_count,
             config.directions + 1, monitor_started);
    print_text(line);
    if (output_error) {
        fail_locked("Ошибка записи вывода");
    }
    pthread_mutex_unlock(&lock);

    for (int i = 0; i < config.lines; i++) {
        if (sources[i].started) {
            join_thread(sources[i].thread);
        }
    }
    for (int i = 0; i < config.parcels; i++) {
        if (parcels[i].started) {
            join_thread(parcels[i].thread);
        }
    }
    pthread_mutex_lock(&lock);
    stop_locked();
    pthread_mutex_unlock(&lock);
    for (int i = 0; i < device_count; i++) {
        if (devices[i].started) {
            join_thread(devices[i].thread);
        }
    }
    for (int i = 0; i <= config.directions; i++) {
        if (unloaders[i].started) {
            join_thread(unloaders[i].thread);
        }
    }
    pthread_mutex_lock(&lock);
    monitor_done = 1;
    pthread_mutex_unlock(&lock);
    if (monitor_started) {
        pthread_kill(monitor, SIGUSR1);
        join_thread(monitor);
    }
    if (!check_model()) {
        model_error = 1;
        error_text = "Нарушен итоговый баланс модели";
    }
    const char *status = model_error || output_error ? "error" :
                         stop_signal ? "interrupted" : "completed";
    snprintf(line, sizeof(line),
        "ИТОГ status=%s planned=%d formed=%d delivered=%d rejected=%d unfinished=%d "
        "not_generated=%d scan_attempts=%llu errors=%llu repeats=%llu waits=%llu "
        "time_ms=%llu threads=%d signal=%d\n",
        status, config.parcels, formed, delivered, rejected, formed - delivered - rejected,
        config.parcels - formed, attempts, errors, repeats, waits, elapsed_ms(), created_threads, stop_signal);
    print_text(line);
    if (error_text != NULL) {
        fprintf(stderr, "%s\n", error_text);
    }
    free_model();
    pthread_mutex_destroy(&lock);
    if (model_error || output_error) {
        return 1;
    }
    return stop_signal ? 128 + stop_signal : 0;
}
