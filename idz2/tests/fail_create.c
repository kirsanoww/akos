// только тестовая сборка: проверяем остановку после частичного создания потоков
#undef pthread_create
#include <pthread.h>
#include <errno.h>
#include <stdlib.h>

int test_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                       void *(*function)(void *), void *data) {
    static int count;
    const char *value = getenv("AKOS_FAIL_AFTER");
    if (value != NULL && count++ >= atoi(value)) {
        return EAGAIN;
    }
    return pthread_create(thread, attr, function, data);
}
