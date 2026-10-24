#include "sync.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int counter_init(Counter *counter, int count) {
    counter->closed = 0;
    counter->waiters = 0;
#ifdef __APPLE__
    // macOS не поддерживает sem_init, используем именованный семафор
    static unsigned int number;
    char name[32];
    snprintf(name, sizeof(name), "/akos_%ld_%u", (long)getpid(), number++);
    counter->sem = sem_open(name, O_CREAT | O_EXCL, 0600, (unsigned int)count);
    if (counter->sem == SEM_FAILED) {
        return -1;
    }
    if (sem_unlink(name) < 0) {
        sem_close(counter->sem);
        return -1;
    }
#else
    counter->sem = &counter->storage;
    if (sem_init(counter->sem, 0, (unsigned int)count) < 0) {
        return -1;
    }
#endif
    return 0;
}

int counter_wait(Counter *counter, pthread_mutex_t *lock) {
    if (counter->closed) {
        return 0;
    }
    counter->waiters++;
    pthread_mutex_unlock(lock);
    int result;
    do {
        result = sem_wait(counter->sem);
    } while (result < 0 && errno == EINTR);
    pthread_mutex_lock(lock);
    counter->waiters--;
    if (result < 0) {
        return -1;
    }
    return !counter->closed;
}

int counter_post(Counter *counter) {
    if (counter->closed) {
        return 0;
    }
    return sem_post(counter->sem);
}

void counter_close(Counter *counter) {
    if (counter->closed) {
        return;
    }
    counter->closed = 1;
    // будим и тех, кто уже ждёт, и тех, кто только входит в sem_wait
    for (int i = 0; i < counter->waiters; i++) {
        sem_post(counter->sem);
    }
}

void counter_destroy(Counter *counter) {
#ifdef __APPLE__
    sem_close(counter->sem);
#else
    sem_destroy(counter->sem);
#endif
}
