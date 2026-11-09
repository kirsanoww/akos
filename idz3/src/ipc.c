#include "center.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

Shared *shared;
volatile sig_atomic_t caught_signal;
int ipc_owner;
static int named_mode, memory_owned;
static char prefix[80], memory_name[96];
static sem_t *handles[MAX_SEMS];
static int made[MAX_SEMS];

static void on_signal(int number) {
    caught_signal = number;
}

int setup_signals(void) {
    struct sigaction action = {0};
    sigemptyset(&action.sa_mask);
    action.sa_handler = on_signal;
    if (sigaction(SIGINT, &action, NULL) < 0 ||
        sigaction(SIGTERM, &action, NULL) < 0) {
        return -1;
    }
    action.sa_handler = SIG_IGN;
    return sigaction(SIGPIPE, &action, NULL);
}

static int names(const char *session) {
    size_t length = strlen(session);
    if (length == 0 || length > 24) {
        return -1;
    }
    for (size_t i = 0; i < length; i++) {
        char c = session[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_')) {
            return -1;
        }
    }
    snprintf(prefix, sizeof(prefix), "/ak3_%lu_%s", (unsigned long)getuid(), session);
    snprintf(memory_name, sizeof(memory_name), "%s_mem", prefix);
    return 0;
}

static void sem_name(char *buffer, size_t size, int number) {
    snprintf(buffer, size, "%s_s%d", prefix, number);
}

static int make_sem(int number, int value) {
    if (named_mode) {
        char name[96];
        sem_name(name, sizeof(name), number);
        handles[number] = sem_open(name, O_CREAT | O_EXCL, 0600, (unsigned int)value);
        if (handles[number] == SEM_FAILED) {
            handles[number] = NULL;
            return -1;
        }
    } else {
        handles[number] = &shared->sems[number];
        if (sem_init(handles[number], 1, (unsigned int)value) < 0) {
            handles[number] = NULL;
            return -1;
        }
    }
    made[number] = 1;
    return 0;
}

int ipc_create(const char *session, int named) {
    if (names(session) < 0) {
        fprintf(stderr, "Имя сеанса: 1..24 латинские буквы, цифры или _\n");
        return -1;
    }
    int fd = shm_open(memory_name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        perror("shm_open: сеанс уже существует или недоступен");
        return -1;
    }
    memory_owned = ipc_owner = 1;
    named_mode = named;
    if (ftruncate(fd, sizeof(Shared)) < 0) {
        close(fd);
        ipc_close();
        return -1;
    }
    shared = mmap(NULL, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (shared == MAP_FAILED) {
        shared = NULL;
        ipc_close();
        return -1;
    }
    memset(shared, 0, sizeof(*shared));
    shared->config = config;
    shared->named = named;
    shared->controller = getpid();
    shared->sem_count = 1; // нулевой семафор защищает всё состояние
    clock_gettime(CLOCK_MONOTONIC, &shared->start_time);
    return 0;
}

int ipc_counter(int value) {
    int number = shared->sem_count;
    if (number >= MAX_SEMS || make_sem(number, value) < 0) {
        return -1;
    }
    shared->sem_count++;
    return number;
}

int ipc_publish(void) {
    // блокировка с номером 0 появляется последней: память уже подготовлена
    shared->magic = 0x414b3301U;
    return make_sem(0, 1);
}

int ipc_attach(const char *session) {
    if (names(session) < 0) {
        return -1;
    }
    named_mode = 1;
    int fd = shm_open(memory_name, O_RDWR, 0);
    if (fd < 0) {
        return -1;
    }
    struct stat info;
    if (fstat(fd, &info) < 0 || info.st_size != (off_t)sizeof(Shared)) {
        close(fd);
        return -1;
    }
    shared = mmap(NULL, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (shared == MAP_FAILED) {
        shared = NULL;
        return -1;
    }
    char name[96];
    sem_name(name, sizeof(name), 0);
    handles[0] = sem_open(name, 0);
    if (handles[0] == SEM_FAILED) {
        handles[0] = NULL;
        ipc_close();
        return -1;
    }
    model_lock();
    int valid = shared->magic == 0x414b3301U && shared->named &&
                shared->sem_count > 1 && shared->sem_count <= MAX_SEMS &&
                !shared->stopping;
    if (valid) {
        config = shared->config;
    }
    model_unlock();
    if (!valid) {
        ipc_close();
        return -1;
    }
    for (int i = 1; i < shared->sem_count; i++) {
        sem_name(name, sizeof(name), i);
        handles[i] = sem_open(name, 0);
        if (handles[i] == SEM_FAILED) {
            handles[i] = NULL;
            ipc_close();
            return -1;
        }
    }
    return 0;
}

int ipc_wait(int number, int milliseconds) {
    struct timespec end;
    if (clock_gettime(CLOCK_REALTIME, &end) < 0) {
        return -1;
    }
    end.tv_nsec += (long)(milliseconds % 1000) * 1000000L;
    end.tv_sec += milliseconds / 1000 + end.tv_nsec / 1000000000L;
    end.tv_nsec %= 1000000000L;
    int result = sem_timedwait(handles[number], &end);
    if (result == 0) {
        return 1;
    }
    return errno == EINTR || errno == ETIMEDOUT ? 0 : -1;
}

int ipc_post(int number) {
    return sem_post(handles[number]);
}

void model_lock(void) {
    // ограничение ожидания позволяет обнаружить гибель владельца блокировки
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        int result = ipc_wait(0, 100);
        if (result > 0) {
            break;
        }
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (result < 0 || now.tv_sec - start.tv_sec >= 5) {
            fprintf(stderr, "Не удалось получить блокировку общей памяти\n");
            if (ipc_owner) {
                for (int i = 0; i < shared->worker_count; i++) {
                    if (shared->workers[i].pid > 0 && !shared->workers[i].finished) {
                        kill(shared->workers[i].pid, SIGKILL);
                    }
                }
                while (waitpid(-1, NULL, 0) > 0 || errno == EINTR) { }
            }
            ipc_close();
            _exit(1);
        }
    }
    if (caught_signal) {
        model_stop(caught_signal);
    }
}

void model_unlock(void) {
    if (ipc_post(0) < 0) {
        perror("sem_post lock");
        _exit(1);
    }
}

void ipc_close(void) {
    for (int i = 0; i < MAX_SEMS; i++) {
        if (handles[i] != NULL) {
            if (named_mode) {
                sem_close(handles[i]);
            } else if (ipc_owner && made[i]) {
                sem_destroy(handles[i]);
            }
            handles[i] = NULL;
        }
        if (named_mode && ipc_owner && made[i]) {
            char name[96];
            sem_name(name, sizeof(name), i);
            sem_unlink(name);
        }
        made[i] = 0;
    }
    if (shared != NULL) {
        munmap(shared, sizeof(Shared));
        shared = NULL;
    }
    if (memory_owned && ipc_owner) {
        shm_unlink(memory_name);
        memory_owned = 0;
    }
}
