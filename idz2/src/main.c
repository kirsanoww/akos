#include "center.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char *argv[]) {
    int result = read_config(argc, argv);
    if (result != 0) {
        return result == 1 ? output_error : 2;
    }

    // все потоки наследуют маску, сигналы принимает отдельный sigwait
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    sigaddset(&signals, SIGUSR1);
    int error = pthread_sigmask(SIG_BLOCK, &signals, NULL);
    if (error != 0) {
        fprintf(stderr, "Не удалось настроить сигналы: %s\n", strerror(error));
        return 1;
    }
    struct sigaction action = {0};
    sigemptyset(&action.sa_mask);
    action.sa_handler = SIG_IGN;
    if (sigaction(SIGPIPE, &action, NULL) < 0) {
        perror("sigaction");
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
