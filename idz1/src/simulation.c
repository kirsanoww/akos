#include "center.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define MAX_PARCELS 100000
#define MAX_DEVICES 64

enum Stage {
    SOURCE, INPUT, SCAN, SORT_QUEUE, SORT, BELT_QUEUE, BELT,
    OUTPUT, MANUAL, DONE, REJECTED
};

typedef struct {
    int id;
    int direction;
    int attempts;
    int recognized;
    enum Stage stage;
} Parcel;

typedef struct {
    int *items;
    int count;
    int limit;
    int head;
} Queue;

typedef struct {
    int parcel;
    unsigned long long ready;
    int result;
    int waiting;
} Device;

typedef struct {
    int parcel;
    unsigned long long next;
    int waiting;
} Source;

static Parcel parcels[MAX_PARCELS];
static int seen[MAX_PARCELS];
static Device scanners[MAX_DEVICES];
static Device sorters[MAX_DEVICES];
static Device belts[MAX_DEVICES];
static Source sources[MAX_DEVICES];
static Queue input;
static Queue sort_queue;
static Queue manual;
static Queue belt_queues[MAX_DEVICES];
static Queue outputs[MAX_DEVICES];
static int formed, delivered, rejected;
static int model_error;
static unsigned long long tick, attempts, errors, repeats, waits;

static int create_queue(Queue *queue, int limit) {
    queue->items = calloc((size_t)limit, sizeof(int));
    queue->count = 0;
    queue->limit = limit;
    queue->head = 0;
    if (queue->items == NULL) {
        return -1;
    }
    return 0;
}

static int add(Queue *queue, int parcel) {
    if (queue->limit <= 0 || queue->count < 0 || queue->count >= queue->limit ||
        queue->head < 0 || queue->head >= queue->limit ||
        parcel < 0 || parcel >= formed) {
        model_error = 1;
        return -1;
    }
    int tail = (queue->head + queue->count) % queue->limit;
    queue->items[tail] = parcel;
    queue->count++;
    return 0;
}

static int take(Queue *queue) {
    if (queue->limit <= 0 || queue->count <= 0 || queue->count > queue->limit ||
        queue->head < 0 || queue->head >= queue->limit) {
        model_error = 1;
        return -1;
    }
    int parcel = queue->items[queue->head];
    if (parcel < 0 || parcel >= formed) {
        model_error = 1;
        return -1;
    }
    // передвигаем начало очереди, сами элементы остаются на месте
    queue->head = (queue->head + 1) % queue->limit;
    queue->count--;
    return parcel;
}

static void show_event(int parcel, const char *event, const char *text) {
    char line[1024];
    snprintf(line, sizeof(line), "[t=%llu] parcel=%d event=%s %s\n",
             tick, parcels[parcel].id, event, text);
    print_text(line);
}

static void show_wait(int parcel, int *waiting, const char *text) {
    if (*waiting == 0) {
        show_event(parcel, "WAIT", text);
        waits++;
        *waiting = 1;
    }
}

static int prepare_model(void) {
    srand(config.seed);
    for (int i = 0; i < config.lines; i++) {
        sources[i].parcel = -1;
    }
    for (int i = 0; i < config.scanners; i++) {
        scanners[i].parcel = -1;
    }
    for (int i = 0; i < config.sorters; i++) {
        sorters[i].parcel = -1;
    }
    if (create_queue(&input, config.buffer_capacity) < 0 ||
        create_queue(&sort_queue, config.buffer_capacity) < 0 ||
        create_queue(&manual, config.buffer_capacity) < 0) {
        return -1;
    }
    for (int i = 0; i < config.directions; i++) {
        belts[i].parcel = -1;
        if (create_queue(&belt_queues[i], config.buffer_capacity) < 0 ||
            create_queue(&outputs[i], config.output_capacity) < 0) {
            return -1;
        }
    }
    return 0;
}

static void free_memory(void) {
    free(input.items);
    free(sort_queue.items);
    free(manual.items);
    for (int i = 0; i < config.directions; i++) {
        free(belt_queues[i].items);
        free(outputs[i].items);
    }
}

static void unload(void) {
    if (tick == 0 || tick % config.unload_interval != 0) {
        return;
    }
    for (int i = 0; i < config.directions; i++) {
        while (outputs[i].count > 0) {
            int parcel = take(&outputs[i]);
            if (parcel < 0) {
                return;
            }
            parcels[parcel].stage = DONE;
            show_event(parcel, "UNLOAD", "Посылка забрана из выходного накопителя");
        }
    }
    while (manual.count > 0) {
        int parcel = take(&manual);
        if (parcel < 0) {
            return;
        }
        parcels[parcel].stage = REJECTED;
        show_event(parcel, "MANUAL_UNLOAD", "Посылка забрана на ручную обработку");
    }
}

static void move_belts(void) {
    char text[256];
    int travel_time = config.conveyor_length / config.conveyor_speed;
    if (config.conveyor_length % config.conveyor_speed != 0) {
        travel_time++;
    }
    for (int i = 0; i < config.directions; i++) {
        int parcel = belts[i].parcel;
        if (parcel != -1 && tick >= belts[i].ready) {
            if (outputs[i].count == outputs[i].limit) {
                show_wait(parcel, &belts[i].waiting,
                          "Выход заполнен; конвейер остановлен");
            } else {
                if (add(&outputs[i], parcel) < 0) {
                    return;
                }
                parcels[parcel].stage = OUTPUT;
                delivered++;
                snprintf(text, sizeof(text),
                         "Доставка в выходной накопитель direction=%d", i + 1);
                show_event(parcel, "DELIVER", text);
                belts[i].parcel = -1;
            }
        }
        if (belts[i].parcel == -1 && belt_queues[i].count > 0) {
            parcel = take(&belt_queues[i]);
            if (parcel < 0) {
                return;
            }
            belts[i].parcel = parcel;
            belts[i].ready = tick + travel_time;
            belts[i].waiting = 0;
            parcels[parcel].stage = BELT;
            snprintf(text, sizeof(text), "Конвейер=%d ready=%llu",
                     i + 1, belts[i].ready);
            show_event(parcel, "TRANSPORT_START", text);
        }
    }
}

static void move_sorters(void) {
    char text[256];
    for (int i = 0; i < config.sorters; i++) {
        int parcel = sorters[i].parcel;
        if (parcel != -1 && tick >= sorters[i].ready) {
            int direction = parcels[parcel].direction;
            if (sorters[i].result == 0) {
                snprintf(text, sizeof(text), "Сортировка завершена direction=%d",
                         direction + 1);
                show_event(parcel, "SORT_END", text);
                sorters[i].result = 1;
            }
            if (belt_queues[direction].count == belt_queues[direction].limit) {
                show_wait(parcel, &sorters[i].waiting,
                          "Накопитель перед конвейером заполнен");
            } else {
                if (add(&belt_queues[direction], parcel) < 0) {
                    return;
                }
                parcels[parcel].stage = BELT_QUEUE;
                show_event(parcel, "CONVEYOR_QUEUE", "Переход в очередь конвейера");
                sorters[i].parcel = -1;
            }
        }
        if (sorters[i].parcel == -1 && sort_queue.count > 0) {
            parcel = take(&sort_queue);
            if (parcel < 0) {
                return;
            }
            sorters[i].parcel = parcel;
            sorters[i].ready = tick + config.sort_time;
            sorters[i].result = 0;
            sorters[i].waiting = 0;
            parcels[parcel].stage = SORT;
            snprintf(text, sizeof(text), "Сортировщик=%d ready=%llu",
                     i + 1, sorters[i].ready);
            show_event(parcel, "SORT_START", text);
        }
    }
}

static void move_scanners(void) {
    char text[256];
    for (int i = 0; i < config.scanners; i++) {
        int parcel = scanners[i].parcel;
        if (parcel != -1 && tick >= scanners[i].ready) {
            // result: 0 - чтение ещё не закончено, 1 - успех, 2 - отказ
            if (scanners[i].result == 0) {
                show_event(parcel, "SCAN_END", "Сканирование завершено");
                if (rand() % 100 < config.error_percent) {
                    errors++;
                    show_event(parcel, "SCAN_ERROR", "Ошибка чтения маркировки");
                    if (parcels[parcel].attempts <= config.retries) {
                        parcels[parcel].attempts++;
                        attempts++;
                        repeats++;
                        scanners[i].ready = tick + config.scan_time;
                        snprintf(text, sizeof(text),
                                 "Начало повторного сканирования attempt=%d ready=%llu",
                                 parcels[parcel].attempts, scanners[i].ready);
                        show_event(parcel, "RETRY", text);
                        continue;
                    }
                    scanners[i].result = 2;
                } else {
                    parcels[parcel].recognized = 1;
                    scanners[i].result = 1;
                    snprintf(text, sizeof(text), "Маркировка распознана direction=%d",
                             parcels[parcel].direction + 1);
                    show_event(parcel, "RECOGNIZED", text);
                }
            }
            if (scanners[i].result == 1) {
                if (sort_queue.count == sort_queue.limit) {
                    show_wait(parcel, &scanners[i].waiting,
                              "Накопитель перед сортировкой заполнен");
                } else {
                    if (add(&sort_queue, parcel) < 0) {
                        return;
                    }
                    parcels[parcel].stage = SORT_QUEUE;
                    show_event(parcel, "SORT_QUEUE", "Переход в очередь сортировки");
                    scanners[i].parcel = -1;
                }
            } else {
                if (manual.count == manual.limit) {
                    show_wait(parcel, &scanners[i].waiting,
                              "Накопитель ручной обработки заполнен");
                } else {
                    if (add(&manual, parcel) < 0) {
                        return;
                    }
                    parcels[parcel].stage = MANUAL;
                    rejected++;
                    show_event(parcel, "REJECT",
                               "Маркировка необрабатываема; передача в ручной накопитель");
                    scanners[i].parcel = -1;
                }
            }
        }
        if (scanners[i].parcel == -1 && input.count > 0) {
            parcel = take(&input);
            if (parcel < 0) {
                return;
            }
            scanners[i].parcel = parcel;
            scanners[i].ready = tick + config.scan_time;
            scanners[i].result = 0;
            scanners[i].waiting = 0;
            parcels[parcel].stage = SCAN;
            parcels[parcel].attempts++;
            attempts++;
            snprintf(text, sizeof(text), "Сканер=%d ready=%llu",
                     i + 1, scanners[i].ready);
            show_event(parcel, "SCAN_START", text);
        }
    }
}

static void add_parcels(void) {
    char text[256];
    // каждый такт начинаем с другой линии, чтобы все могли попасть на вход
    int first = (int)(tick % config.lines);
    for (int step = 0; step < config.lines; step++) {
        int i = (first + step) % config.lines;
        if (sources[i].parcel == -1 && formed < config.parcels && tick >= sources[i].next) {
            int parcel = formed;
            formed++;
            parcels[parcel].id = parcel + 1;
            parcels[parcel].direction = rand() % config.directions;
            parcels[parcel].stage = SOURCE;
            sources[i].parcel = parcel;
            sources[i].waiting = 0;
            snprintf(text, sizeof(text), "Посылка поступила к входной линии source=%d", i + 1);
            show_event(parcel, "ARRIVE", text);
        }
        if (sources[i].parcel != -1) {
            int parcel = sources[i].parcel;
            if (input.count == input.limit) {
                show_wait(parcel, &sources[i].waiting,
                          "Входной накопитель заполнен; источник ожидает");
            } else {
                if (add(&input, parcel) < 0) {
                    return;
                }
                parcels[parcel].stage = INPUT;
                show_event(parcel, "INPUT", "Приём во входной накопитель");
                sources[i].parcel = -1;
                sources[i].next = tick + 1 + rand() % config.arrival_interval;
            }
        }
    }
}

static int check_parcel(int parcel, enum Stage stage, int direction) {
    if (parcel < 0 || parcel >= formed) {
        return -1;
    }
    if (seen[parcel] != 0 || parcels[parcel].stage != stage || parcels[parcel].id != parcel + 1) {
        return -1;
    }
    if (parcels[parcel].direction < 0 || parcels[parcel].direction >= config.directions) {
        return -1;
    }
    if (direction != -1 && parcels[parcel].direction != direction) {
        return -1;
    }
    if (stage == SORT_QUEUE || stage == SORT || stage == BELT_QUEUE ||
        stage == BELT || stage == OUTPUT || stage == DONE) {
        if (parcels[parcel].recognized == 0) {
            return -1;
        }
    }
    if (parcels[parcel].attempts > config.retries + 1) {
        return -1;
    }
    if (stage == MANUAL || stage == REJECTED) {
        if (parcels[parcel].recognized || parcels[parcel].attempts != config.retries + 1) {
            return -1;
        }
    }
    seen[parcel] = 1;
    return 0;
}

static int check_queue(Queue *queue, enum Stage stage, int direction) {
    if (queue->limit <= 0 || queue->count < 0 || queue->count > queue->limit ||
        queue->head < 0 || queue->head >= queue->limit) {
        return -1;
    }
    for (int i = 0; i < queue->count; i++) {
        int index = (queue->head + i) % queue->limit;
        if (check_parcel(queue->items[index], stage, direction) < 0) {
            return -1;
        }
    }
    return 0;
}

static int check_model(void) {
    int good = 0;
    int bad = 0;
    for (int i = 0; i < formed; i++) {
        seen[i] = 0;
    }
    if (check_queue(&input, INPUT, -1) < 0 ||
        check_queue(&sort_queue, SORT_QUEUE, -1) < 0 ||
        check_queue(&manual, MANUAL, -1) < 0) {
        return -1;
    }
    for (int i = 0; i < config.lines; i++) {
        if (sources[i].parcel != -1) {
            if (check_parcel(sources[i].parcel, SOURCE, -1) < 0) {
                return -1;
            }
        }
    }
    for (int i = 0; i < config.scanners; i++) {
        if (scanners[i].parcel != -1) {
            if (check_parcel(scanners[i].parcel, SCAN, -1) < 0) {
                return -1;
            }
        }
    }
    for (int i = 0; i < config.sorters; i++) {
        if (sorters[i].parcel != -1) {
            if (check_parcel(sorters[i].parcel, SORT, -1) < 0) {
                return -1;
            }
        }
    }
    for (int i = 0; i < config.directions; i++) {
        if (check_queue(&belt_queues[i], BELT_QUEUE, i) < 0 ||
            check_queue(&outputs[i], OUTPUT, i) < 0) {
            return -1;
        }
        if (belts[i].parcel != -1) {
            if (check_parcel(belts[i].parcel, BELT, i) < 0) {
                return -1;
            }
        }
    }
    // каждая созданная посылка должна встретиться ровно один раз
    for (int i = 0; i < formed; i++) {
        enum Stage stage = parcels[i].stage;
        if (stage == DONE || stage == REJECTED) {
            if (check_parcel(i, stage, -1) < 0) {
                return -1;
            }
        }
        if (seen[i] == 0) {
            return -1;
        }
        if (stage == DONE || stage == OUTPUT) {
            good++;
        }
        if (stage == REJECTED || stage == MANUAL) {
            bad++;
        }
    }
    if (good != delivered || bad != rejected) {
        return -1;
    }
    return 0;
}

static int pause_model(void) {
    struct timespec pause;
    pause.tv_sec = config.delay_ms / 1000;
    pause.tv_nsec = (long)(config.delay_ms % 1000) * 1000000L;
    while (nanosleep(&pause, &pause) < 0) {
        if (errno != EINTR) {
            perror("Ошибка задержки");
            return -1;
        }
        if (stop_signal) {
            break;
        }
    }
    return 0;
}

int run_model(void) {
    char text[2048];
    int result = 0;
    if (prepare_model() < 0) {
        fprintf(stderr, "Недостаточно памяти\n");
        free_memory();
        return 1;
    }
    print_text("Вариант 34. Последовательная модель сортировочного центра\n");
    snprintf(text, sizeof(text),
             "CONFIG parcels=%d directions=%d lines=%d scanners=%d sorters=%d "
             "buffer_capacity=%d output_capacity=%d arrival_interval=%d scan_time=%d "
             "sort_time=%d conveyor_length=%d conveyor_speed=%d unload_interval=%d "
             "error_percent=%d retries=%d seed=%u delay_ms=%d check=%d\n",
             config.parcels, config.directions, config.lines, config.scanners, config.sorters,
             config.buffer_capacity, config.output_capacity, config.arrival_interval,
             config.scan_time, config.sort_time, config.conveyor_length, config.conveyor_speed,
             config.unload_interval, config.error_percent, config.retries, config.seed,
             config.delay_ms, config.check);
    print_text(text);

    while (delivered + rejected < config.parcels && !stop_signal && !output_error) {
        // сначала освобождаем выход, затем обрабатываем предыдущие этапы
        unload();
        move_belts();
        move_sorters();
        move_scanners();
        add_parcels();
        if (model_error || (config.check && check_model() < 0)) {
            print_text("Нарушен инвариант модели.\n");
            result = 1;
            break;
        }
        if (delivered + rejected == config.parcels) {
            break;
        }
        if (config.delay_ms > 0 && !stop_signal && !output_error) {
            if (pause_model() < 0) {
                result = 1;
                break;
            }
        }
        if (stop_signal || output_error) {
            break;
        }
        tick++;
    }

    // перед завершением проверяем всю модель в любом режиме
    if (result == 0 && check_model() < 0) {
        print_text("Нарушен инвариант модели.\n");
        result = 1;
    }

    const char *status = "completed";
    if (output_error || result == 1) {
        result = 1;
        status = "error";
        print_text("\nОшибка моделирования.\n");
    } else if (stop_signal) {
        result = 128 + stop_signal;
        status = "interrupted";
        print_text("\nМоделирование прервано пользователем.\n");
    } else {
        print_text("\nВсе посылки обработаны.\n");
    }
    snprintf(text, sizeof(text),
             "ИТОГ status=%s planned=%d formed=%d delivered=%d rejected=%d "
             "unfinished=%d not_generated=%d scan_attempts=%llu errors=%llu "
             "repeats=%llu waits=%llu ticks=%llu\n",
             status, config.parcels, formed, delivered, rejected,
             formed - delivered - rejected, config.parcels - formed,
             attempts, errors, repeats, waits, tick);
    print_text(text);
    free_memory();
    return result;
}
