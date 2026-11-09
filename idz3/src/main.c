#include "center.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef CENTER_MODE
#define CENTER_MODE 1
#endif

int main(int argc, char *argv[]) {
    if (setup_signals() < 0) {
        perror("sigaction");
        return 1;
    }
    if (CENTER_MODE == 4) return worker_main(argc, argv);

    char generated[25];
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    snprintf(generated, sizeof(generated), "p%ld_%ld", (long)getpid(), now.tv_nsec);
    const char *session = NULL;
    char **args = calloc((size_t)argc + 1, sizeof(char *));
    if (args == NULL) return 1;
    int count = 1;
    args[0] = argv[0];
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--session") == 0) {
            if (++i >= argc || session != NULL) {
                fprintf(stderr, "После --session нужно одно имя сеанса\n");
                free(args);
                return 2;
            }
            session = argv[i];
        } else {
            args[count++] = argv[i];
        }
    }
    int result = read_config(count, args);
    free(args);
    if (result != 0) return result == 1 ? output_error : 2;
    if (session == NULL && CENTER_MODE == 3) {
        fprintf(stderr, "Для независимых процессов нужен --session NAME\n");
        return 2;
    }
    if (session == NULL) session = generated;
    // независимо запущенные участники могут иметь другой рабочий каталог
    if (config.log_path[0] && config.log_path[0] != '/') {
        char cwd[4096], absolute[4096];
        if (getcwd(cwd, sizeof(cwd)) == NULL ||
            snprintf(absolute, sizeof(absolute), "%s/%s", cwd, config.log_path) >= (int)sizeof(absolute)) {
            fprintf(stderr, "Слишком длинный путь журнала\n");
            return 2;
        }
        strcpy(config.log_path, absolute);
    }
    return run_controller(session, CENTER_MODE);
}
