#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#undef fork
extern pid_t fork(void);

pid_t test_fork(void) {
    static int count;
    const char *text = getenv("AKOS_FAIL_AFTER");
    if (text != NULL && count++ >= atoi(text)) {
        errno = EAGAIN;
        return -1;
    }
    return fork();
}
