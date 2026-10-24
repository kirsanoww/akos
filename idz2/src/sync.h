#ifndef SYNC_H
#define SYNC_H

#include <pthread.h>
#include <semaphore.h>

typedef struct {
    int closed;
    int waiters;
    sem_t *sem;
#ifndef __APPLE__
    sem_t storage;
#endif
} Counter;

int counter_init(Counter *counter, int count);
// эти функции вызываются с захваченным мьютексом модели
int counter_wait(Counter *counter, pthread_mutex_t *lock);
int counter_post(Counter *counter);
void counter_close(Counter *counter);
void counter_destroy(Counter *counter);

#endif
