#include "center.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int log_file = -1;
int output_error = 0;

static int write_text(int file, const char *text) {
    size_t length = strlen(text);
    size_t sent = 0;

    // дописываем строку, если write записал только её часть
    while (sent < length) {
        ssize_t count = write(file, text + sent, length - sent);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return -1;
        }
        sent += (size_t)count;
    }
    return 0;
}

int open_log(void) {
    if (config.log_path != NULL) {
        log_file = open(config.log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (log_file < 0) {
            perror("Не удалось открыть журнал");
            return -1;
        }
    }
    return 0;
}

void print_text(const char *text) {
    if (write_text(STDOUT_FILENO, text) < 0) {
        output_error = 1;
    }
    if (log_file >= 0) {
        if (write_text(log_file, text) < 0) {
            output_error = 1;
        }
    }
}

int close_log(void) {
    if (log_file >= 0) {
        if (close(log_file) < 0) {
            output_error = 1;
        }
    }
    if (output_error) {
        fprintf(stderr, "Ошибка записи вывода или журнала.\n");
        return -1;
    }
    return 0;
}
