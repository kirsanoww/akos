#include "center.h"

#include <stdio.h>

volatile sig_atomic_t stop_signal = 0;

static void stop_program(int number) {
    // основной цикл сам напечатает итог и закроет файл
    stop_signal = number;
}

int main(int argc, char *argv[]) {
    int result = read_config(argc, argv);
    if (result == 1) {
        return output_error;
    }
    if (result == -1) {
        return 2;
    }

    struct sigaction action = {0};
    sigemptyset(&action.sa_mask);
    action.sa_handler = stop_program;
    if (sigaction(SIGINT, &action, NULL) < 0 ||
        sigaction(SIGTERM, &action, NULL) < 0) {
        perror("Не удалось настроить обработку сигналов");
        return 1;
    }

    action.sa_handler = SIG_IGN;
    if (sigaction(SIGPIPE, &action, NULL) < 0) {
        perror("Не удалось настроить SIGPIPE");
        return 1;
    }

    if (open_log() < 0) {
        return 1;
    }
    result = run_model();
    if (close_log() < 0) {
        return 1;
    }
    return result;
}
