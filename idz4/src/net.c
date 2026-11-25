#include "center.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
volatile sig_atomic_t caught_signal;
int output_error;
static void on_signal(int sig) { caught_signal = sig; }
int setup_signals(void) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) || sigaction(SIGTERM, &action, NULL)) return -1;
    action.sa_handler = SIG_IGN;
    return sigaction(SIGPIPE, &action, NULL);
}
int write_all(int fd, const char *text) {
    size_t left = strlen(text);
    while (left) {
        ssize_t n = write(fd, text, left);
        if (n > 0) { text += n; left -= (size_t)n; }
        else if (n < 0 && errno == EINTR) continue;
        else return -1;
    }
    return 0;
}
void print_text(const char *text) { if (write_all(STDOUT_FILENO, text)) output_error = 1; }
long long now_ms(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) return 0;
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
int number(const char *text, int min, int max, int *value) {
    if (!text[0]) return -1;
    for (int i = 0; text[i]; i++) if (text[i] < '0' || text[i] > '9') return -1;
    errno = 0;
    long n = strtol(text, NULL, 10);
    if (errno || n < min || n > max) return -1;
    *value = (int)n;
    return 0;
}
int nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
static int address(const char *ip, int port, struct sockaddr_in *addr) {
    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_port = htons((unsigned short)port);
    return inet_pton(AF_INET, ip, &addr->sin_addr) == 1 ? 0 : -1;
}
int listen_server(const char *ip, int port) {
    struct sockaddr_in addr;
    if (address(ip, port, &addr)) return -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0), yes = 1;
    if (fd < 0) return -1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) ||
        bind(fd, (struct sockaddr *)&addr, sizeof(addr)) || listen(fd, 128) || nonblock(fd)) {
        close(fd); return -1;
    }
    return fd;
}
int connect_server(const char *ip, int port) {
    struct sockaddr_in addr;
    if (address(ip, port, &addr)) return -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (nonblock(fd)) { close(fd); return -1; }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) return fd;
    if (errno != EINPROGRESS) { close(fd); return -1; }
    struct pollfd p = {fd, POLLOUT, 0};
    int error = 0; socklen_t size = sizeof(error);
    if (poll(&p, 1, 1000) <= 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) || error) {
        close(fd); return -1;
    }
    return fd;
}
