#include "center.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static unsigned long long now_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (unsigned long long)now.tv_sec * 1000 + (unsigned long long)now.tv_nsec / 1000000;
}

int run_controller(const char *session, int mode) {
    if (ipc_create(session, mode != 2) < 0) return 1;
    if (open_log(0) < 0 || !prepare_model()) {
        fprintf(stderr, "Не удалось подготовить память, семафоры или журнал\n");
        ipc_close();
        close_log();
        return 1;
    }
    print_config();
    char line[256];
    snprintf(line, sizeof(line), "SESSION name=%s mode=%d workers=%d\n",
             session, mode, shared->worker_count);
    print_text(line);
    if (output_error || ipc_publish() < 0) {
        ipc_close();
        close_log();
        return 1;
    }

    pid_t children[MAX_WORKERS] = {0};
    int reaped[MAX_WORKERS] = {0};
    int created = 0;
    if (mode != 3) {
        for (int i = 0; i < shared->worker_count; i++) {
            if (caught_signal) break;
            pid_t pid = fork();
            if (pid < 0) {
                model_lock();
                model_fail("Не удалось создать все процессы");
                model_unlock();
                break;
            }
            if (pid == 0) {
                ipc_owner = 0;
                int error = run_worker(i);
                if (close_log() < 0) error = 1;
                ipc_close();
                _exit(error ? 1 : 0);
            }
            children[i] = pid;
            created++;
        }
    }

    unsigned long long start = now_ms(), stop_time = 0;
    for (;;) {
        int bad_child = 0, live_children = 0;
        for (int i = 0; i < created; i++) {
            if (reaped[i]) continue;
            int status = 0;
            pid_t result = waitpid(children[i], &status, WNOHANG);
            if (result == children[i]) {
                reaped[i] = 1;
                if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) bad_child = 1;
            } else if (result == 0 || (result < 0 && errno == EINTR)) {
                live_children++;
            } else {
                reaped[i] = 1;
                bad_child = 1;
            }
        }
        model_lock();
        if (bad_child) model_fail("Рабочий процесс завершился с ошибкой");
        if (!shared->running && !shared->stopping) {
            if (shared->registered == shared->worker_count) {
                shared->running = 1;
                for (int i = 0; i < shared->worker_count; i++) {
                    if (ipc_post(shared->start_gate) < 0) model_fail("Ошибка стартового семафора");
                }
            } else if (now_ms() - start >= (unsigned long long)config.startup_timeout * 1000) {
                model_fail("Истёк срок ожидания всех участников");
            }
        }
        if (shared->running && shared->delivered + shared->rejected == config.parcels) {
            model_stop(0);
        }
        int active = 0;
        for (int i = 0; i < shared->worker_count; i++) {
            Worker *worker = &shared->workers[i];
            if (worker->pid <= 0 || worker->finished) continue;
            if (mode == 3 && kill(worker->pid, 0) < 0 && errno == ESRCH) {
                worker->finished = 1;
                model_fail("Исчез независимо запущенный участник");
            } else {
                active++;
            }
        }
        int stopping = shared->stopping;
        if (stopping && !stop_time) stop_time = now_ms();
        int done = stopping && (mode == 3 ? active == 0 : live_children == 0);
        if (stopping && !done && now_ms() - stop_time > 5000) {
            model_fail("Участники не завершились за 5 секунд");
            for (int i = 0; i < shared->worker_count; i++) {
                Worker *worker = &shared->workers[i];
                if (worker->pid > 0 && !worker->finished) kill(worker->pid, SIGKILL);
            }
            for (int i = 0; i < created; i++) {
                if (!reaped[i]) kill(children[i], SIGKILL);
            }
            // для именованных объектов unlink безопасен и при ещё открытых дескрипторах
            if (mode == 3) done = 1;
        }
        model_unlock();
        if (done) break;
        struct timespec pause = {0, 10000000L};
        nanosleep(&pause, NULL);
    }
    model_lock();
    int result = print_result();
    model_unlock();
    ipc_close();
    snprintf(line, sizeof(line), "CLEANUP session=%s\n", session);
    print_text(line);
    if (close_log() < 0) result = 1;
    return result;
}

int worker_main(int argc, char *argv[]) {
    if (argc != 3 && argc != 4) {
        fprintf(stderr, "Запуск: center_worker SESSION plan | ROLE INDEX\n");
        return 2;
    }
    if (ipc_attach(argv[1]) < 0) {
        fprintf(stderr, "Сеанс недоступен или ещё не готов\n");
        return 1;
    }
    if (argc == 3 && strcmp(argv[2], "plan") == 0) {
        model_lock();
        for (int i = 0; i < shared->worker_count; i++) {
            Worker *worker = &shared->workers[i];
            printf("%s %d\n", job_name(worker->role), worker->index + 1);
        }
        model_unlock();
        ipc_close();
        return 0;
    }
    int index = 0;
    if (argc == 4) {
        for (int i = 0; argv[3][i]; i++) {
            if (argv[3][i] < '0' || argv[3][i] > '9' || index > MAX_WORKERS) {
                index = -1;
                break;
            }
            index = index * 10 + argv[3][i] - '0';
        }
    }
    int slot = -1;
    for (int i = 0; i < shared->worker_count; i++) {
        Worker *worker = &shared->workers[i];
        if (strcmp(argv[2], job_name(worker->role)) == 0 && worker->index + 1 == index) slot = i;
    }
    if (slot < 0) {
        fprintf(stderr, "Неизвестная роль или номер участника\n");
        ipc_close();
        return 2;
    }
    if (open_log(1) < 0) {
        model_lock();
        model_fail("Участник не смог открыть журнал");
        model_unlock();
        ipc_close();
        return 1;
    }
    int result = run_worker(slot);
    if (close_log() < 0) result = 1;
    ipc_close();
    return result;
}
