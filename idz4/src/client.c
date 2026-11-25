#include "center.h"
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#ifndef OBSERVER
#define OBSERVER 0
#endif
static char output[4096];
static int length, sent;
static int enqueue(const char *text) {
    if (sent) { memmove(output, output + sent, (size_t)(length - sent)); length -= sent; sent = 0; }
    size_t n = strlen(text);
    if ((size_t)length + n > sizeof(output)) return -1;
    memcpy(output + length, text, n); length += (int)n;
    return 0;
}
static int flush(int fd) {
    if (length == sent) return 0;
    ssize_t n = send(fd, output + sent, (size_t)(length - sent), 0);
    if (n > 0) { sent += (int)n; if (sent == length) sent = length = 0; return 0; }
    return n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
}
static int session(int fd, const char *hello, int *accepted) {
    char input[LINE_SIZE], data[4096]; int used = 0, active = 0, result = 0;
    unsigned int token = 0;
    long long due = 0, heard = now_ms();
    length = sent = 0;
    if (enqueue(hello)) return -1;
    while (!caught_signal && !output_error) {
        if (active && now_ms() >= due) {
            char done[80]; snprintf(done, sizeof(done), "DONE %u %d\n", token, result);
            if (enqueue(done)) return -1;
            print_text(done); active = 0;
        }
        struct pollfd p = {fd, POLLIN, 0};
        if (length > sent) p.events |= POLLOUT;
        if (poll(&p, 1, 10) < 0) { if (errno == EINTR) continue; return -1; }
        if ((p.revents & POLLOUT) && flush(fd)) return -1;
        if (p.revents & POLLIN) {
            ssize_t n = recv(fd, data, sizeof(data), 0);
            if (!n) return -1;
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return -1;
            for (ssize_t k = 0; k < n; k++) {
                if (data[k] == '\n') {
                    input[used] = 0; used = 0; heard = now_ms();
                    if (!strcmp(input, "PING")) { if (enqueue("PONG\n")) return -1; continue; }
                    print_text(input); print_text("\n");
                    if (!strncmp(input, "WELCOME ", 8)) *accepted = 1;
                    if (!strncmp(input, "STOP ", 5)) return 0;
                    if (!strncmp(input, "JOB ", 4)) {
                        unsigned int next; int parcel, answer; long long duration; char extra;
                        if (OBSERVER || sscanf(input, "JOB %u %d %lld %d %c", &next, &parcel, &duration, &answer, &extra) != 4 ||
                            !next || parcel < 1 || parcel > MAX_PARCELS || duration < 0 || duration > 6000000000LL || answer < 0 || answer > 1) return -1;
                        token = next; result = answer; active = 1; due = now_ms() + duration;
                    }
                } else if ((unsigned char)data[k] < 32 || (unsigned char)data[k] > 126 || used >= LINE_SIZE - 1) return -1;
                else input[used++] = data[k];
            }
        }
        if (p.revents & (POLLERR | POLLNVAL)) return -1;
        if ((p.revents & POLLHUP) && !(p.revents & POLLIN)) return -1;
        if (now_ms() - heard > 6000) return -1;
    }
    // выход участника освобождает соединение, но не отменяет посылку
    (void)send(fd, "BYE\n", 4, 0);
    return caught_signal ? 128 + caught_signal : 1;
}
int main(int argc, char **argv) {
    int minimum = OBSERVER ? 3 : 5, port, index = 0, retries = 10;
    if (argc < minimum || number(argv[2], 1, 65535, &port)) {
        fprintf(stderr, "Запуск: %s IPv4 PORT %s[--retry-seconds N]\n", argv[0], OBSERVER ? "" : "ROLE INDEX "); return 2;
    }
    if (!OBSERVER && number(argv[4], 1, MAX_DEVICES + 1, &index)) return 2;
    if (!OBSERVER && strcmp(argv[3], "source") && strcmp(argv[3], "scanner") && strcmp(argv[3], "sorter") && strcmp(argv[3], "belt") && strcmp(argv[3], "unload")) return 2;
    if (argc != minimum) {
        if (argc != minimum + 2 || strcmp(argv[minimum], "--retry-seconds") || number(argv[minimum + 1], 0, 300, &retries)) return 2;
    }
    if (setup_signals()) return 1;
    char hello[100];
    if (OBSERVER) snprintf(hello, sizeof(hello), "HELLO 1 observer\n");
    else snprintf(hello, sizeof(hello), "HELLO 1 worker %s %d\n", argv[3], index);
    long long deadline = now_ms() + (long long)retries * 1000;
    int first = 1;
    while (!caught_signal && !output_error) {
        if (!first && now_ms() >= deadline) break;
        first = 0;
        int fd = connect_server(argv[1], port);
        if (fd >= 0) {
            print_text("CONNECTED\n");
            int accepted = 0;
            int code = session(fd, hello, &accepted); close(fd);
            if (code >= 0) return code;
            print_text("DISCONNECTED\n");
            // после потери рабочего соединения даём время восстановить сеть
            if (accepted) deadline = now_ms() + (long long)retries * 1000;
        }
        if (!retries) break;
        poll(NULL, 0, 200);
    }
    return caught_signal ? 128 + caught_signal : 1;
}
