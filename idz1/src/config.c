#include "center.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Config config;

static void set_defaults(void) {
    config.parcels = 20;
    config.directions = 3;
    config.lines = 2;
    config.scanners = 2;
    config.sorters = 2;
    config.buffer_capacity = 3;
    config.output_capacity = 2;
    config.arrival_interval = 3;
    config.scan_time = 2;
    config.sort_time = 2;
    config.conveyor_length = 6;
    config.conveyor_speed = 2;
    config.unload_interval = 5;
    config.error_percent = 20;
    config.retries = 2;
    config.seed = 34;
    config.delay_ms = 0;
    config.check = 0;
    config.log_path = NULL;
}

static void show_help(void) {
    print_text(
        "Вариант 34. Сортировочный центр посылок.\n"
        "Запуск: sorting_center [параметр значение]...\n"
        "  --parcels N           число посылок, 0..100000 (20)\n"
        "  --directions N        число направлений, 1..64 (3)\n"
        "  --lines N             число входных линий, 1..64 (2)\n"
        "  --scanners N          число сканеров, 1..64 (2)\n"
        "  --sorters N           число сортировщиков, 1..64 (2)\n"
        "  --buffer-capacity N   вместимость промежуточных и ручного накопителей (3)\n"
        "  --output-capacity N   вместимость каждого выхода (2)\n"
        "  --arrival-interval N  интервал прихода от 1 до N тактов (3)\n"
        "  --scan-time N         время сканирования в тактах (2)\n"
        "  --sort-time N         время сортировки в тактах (2)\n"
        "  --conveyor-length N   длина конвейера (6)\n"
        "  --conveyor-speed N    скорость конвейера за такт (2)\n"
        "  --unload-interval N   период разгрузки в тактах (5)\n"
        "  --error-percent N    вероятность ошибки, целый процент 0..100 (20)\n"
        "  --retries N           число повторных сканирований, 0..100 (2)\n"
        "  --seed N              начальное значение генератора, 0..4294967295 (34)\n"
        "  --delay-ms N          пауза между тактами, 0..60000 мс (0)\n"
        "  --log PATH            файл журнала, перезаписывается\n"
        "  --check               полная проверка модели после каждого такта\n"
        "  --help                справка\n"
        "Вместимости, времена, интервалы, длина и скорость: 1..100000.\n"
        "Ctrl+C завершает программу с частичной статистикой.\n");
}

int read_config(int argc, char *argv[]) {
    set_defaults();

    for (int i = 1; i < argc; i++) {
        const char *name = argv[i];
        if (strcmp(name, "--help") == 0) {
            show_help();
            return 1;
        }
        if (strcmp(name, "--check") == 0) {
            config.check = 1;
            continue;
        }
        if (i + 1 >= argc) {
            fprintf(stderr, "Нет значения для %s\n", name);
            return -1;
        }
        i++;
        const char *text = argv[i];
        if (strcmp(name, "--log") == 0) {
            if (text[0] == '\0') {
                fprintf(stderr, "Пустой путь журнала\n");
                return -1;
            }
            config.log_path = text;
            continue;
        }

        // принимаем только целые неотрицательные числа
        if (text[0] == '\0') {
            fprintf(stderr, "Пустое значение для %s\n", name);
            return -1;
        }
        for (int j = 0; text[j] != '\0'; j++) {
            if (text[j] < '0' || text[j] > '9') {
                fprintf(stderr, "Некорректное число: %s\n", text);
                return -1;
            }
        }
        errno = 0;
        unsigned long number = strtoul(text, NULL, 10);
        if (errno == ERANGE || number > 4294967295UL) {
            fprintf(stderr, "Слишком большое число: %s\n", text);
            return -1;
        }
        if (strcmp(name, "--seed") == 0) {
            config.seed = (unsigned int)number;
            continue;
        }
        if (number > 100000) {
            fprintf(stderr, "Значение %s превышает 100000\n", name);
            return -1;
        }
        int value = (int)number;
        int min = 1;
        int max = 100000;

        if (strcmp(name, "--parcels") == 0) {
            config.parcels = value;
            min = 0;
        } else if (strcmp(name, "--directions") == 0) {
            config.directions = value;
            max = 64;
        } else if (strcmp(name, "--lines") == 0) {
            config.lines = value;
            max = 64;
        } else if (strcmp(name, "--scanners") == 0) {
            config.scanners = value;
            max = 64;
        } else if (strcmp(name, "--sorters") == 0) {
            config.sorters = value;
            max = 64;
        } else if (strcmp(name, "--buffer-capacity") == 0) {
            config.buffer_capacity = value;
        } else if (strcmp(name, "--output-capacity") == 0) {
            config.output_capacity = value;
        } else if (strcmp(name, "--arrival-interval") == 0) {
            config.arrival_interval = value;
        } else if (strcmp(name, "--scan-time") == 0) {
            config.scan_time = value;
        } else if (strcmp(name, "--sort-time") == 0) {
            config.sort_time = value;
        } else if (strcmp(name, "--conveyor-length") == 0) {
            config.conveyor_length = value;
        } else if (strcmp(name, "--conveyor-speed") == 0) {
            config.conveyor_speed = value;
        } else if (strcmp(name, "--unload-interval") == 0) {
            config.unload_interval = value;
        } else if (strcmp(name, "--error-percent") == 0) {
            config.error_percent = value;
            min = 0;
            max = 100;
        } else if (strcmp(name, "--retries") == 0) {
            config.retries = value;
            min = 0;
            max = 100;
        } else if (strcmp(name, "--delay-ms") == 0) {
            config.delay_ms = value;
            min = 0;
            max = 60000;
        } else {
            fprintf(stderr, "Неизвестный параметр: %s\n", name);
            return -1;
        }
        if (value < min || value > max) {
            fprintf(stderr, "%s: допустимый диапазон %d..%d\n", name, min, max);
            return -1;
        }
    }
    return 0;
}
