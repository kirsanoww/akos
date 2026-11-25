#include "model.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
    int fd, role, job, drop, used, sent, length, half_closed;
    long long heard, ping;
    char input[LINE_SIZE], output[OUT_SIZE];
} Client;
static Client clients[MAX_CLIENTS];
static int log_fd = -1;
static long long started;
static int stopping;
static void record(const char *text) {
    print_text(text);
    if (log_fd >= 0 && write_all(log_fd, text)) output_error = 1;
}
static void queue_text(int i, const char *text) {
    Client *c = &clients[i];
    if (c->fd < 0 || c->drop) return;
    size_t n = strlen(text);
    if (c->sent) {
        memmove(c->output, c->output + c->sent, (size_t)(c->length - c->sent));
        c->length -= c->sent; c->sent = 0;
    }
    // медленный наблюдатель не останавливает остальные соединения
    if ((size_t)c->length + n > sizeof(c->output)) { c->drop = 1; return; }
    memcpy(c->output + c->length, text, n); c->length += (int)n;
}
static void message(int i, const char *format, ...) {
    char text[LINE_SIZE]; va_list args;
    va_start(args, format); int n = vsnprintf(text, sizeof(text), format, args); va_end(args);
    if (n < 0 || n >= (int)sizeof(text)) { model_error = 1; return; }
    queue_text(i, text);
}
void event(const char *name, int parcel, int job, int queue, int result) {
    char text[LINE_SIZE];
    Parcel *p = parcel >= 0 ? &parcels[parcel] : NULL;
    snprintf(text, sizeof(text),
        "E seq=%llu ms=%lld event=%s parcel=%d stage=%d job=%d queue=%d count=%d direction=%d attempts=%d result=%d role=%s index=%d\n",
        ++sequence, now_ms() - started, name, p ? p->id : 0, p ? p->stage : -1,
        job, queue, queue >= 0 ? queues[queue].count : -1, p ? p->direction + 1 : 0,
        p ? p->attempts : 0, result, job >= 0 ? role_name(jobs[job].role) : "none", job >= 0 ? jobs[job].index : 0);
    record(text);
    for (int i = 0; i < MAX_CLIENTS; i++) if (clients[i].role == 2) queue_text(i, text);
    if (config.check && model_check()) model_error = 1;
}
void send_job(int index) {
    Job *j = &jobs[index];
    if (j->client < 0 || j->parcel < 0 || j->finished) return;
    message(j->client, "JOB %u %d %lld %d\n", j->token, j->parcel + 1, j->duration, j->result);
}
static void disconnect_client(int i) {
    Client *c = &clients[i];
    if (c->fd < 0) return;
    close(c->fd); c->fd = -1;
    int job = c->job, role = c->role;
    c->role = 0;
    if (role == 1 && jobs[job].client == i) jobs[job].client = -1;
    if (!stopping) event("DISCONNECT", -1, job, -1, role);
}
static void snapshot(int i) {
    message(i, "BEGIN seq=%llu total=%d directions=%d jobs=%d\n", sequence, config.parcels, config.directions, job_count);
    for (int q = 0; q < queue_count; q++)
        message(i, "QUEUE id=%d count=%d limit=%d stage=%d\n", q, queues[q].count, queues[q].limit, queues[q].stage);
    for (int n = 0; n < config.parcels; n++) {
        int qid = -1, pos = -1, owner = -1;
        for (int q = 0; q < queue_count; q++) for (int k = 0; k < queues[q].count; k++)
            if (queues[q].items[(queues[q].head + k) % MAX_PARCELS] == n) { qid = q; pos = k; }
        for (int j = 0; j < job_count; j++) if (jobs[j].parcel == n) owner = j;
        Parcel *p = &parcels[n];
        message(i, "PARCEL id=%d stage=%d direction=%d recognized=%d attempts=%d queue=%d position=%d job=%d\n",
                p->id, p->stage, p->direction + 1, p->recognized, p->attempts, qid, pos, owner);
    }
    for (int j = 0; j < job_count; j++)
        message(i, "WORKER job=%d role=%s index=%d online=%d parcel=%d finished=%d token=%u\n",
                j, role_name(jobs[j].role), jobs[j].index, jobs[j].client >= 0,
                jobs[j].parcel + 1, jobs[j].finished, jobs[j].token);
    char text[LINE_SIZE]; model_summary(text, sizeof(text), "snapshot"); queue_text(i, text);
    message(i, "END seq=%llu\n", sequence);
}
static void handle_line(int i, char *text) {
    Client *c = &clients[i];
    char *words[6], *save = NULL; int count = 0;
    for (char *s = strtok_r(text, " ", &save); s; s = strtok_r(NULL, " ", &save)) {
        if (count == 6) { c->drop = 1; return; } words[count++] = s;
    }
    if (!count) { c->drop = 1; return; }
    if (!c->role) {
        if (count < 3 || strcmp(words[0], "HELLO") || strcmp(words[1], "1")) { c->drop = 1; return; }
        if (count == 3 && !strcmp(words[2], "observer")) {
            c->role = 2; queue_text(i, "WELCOME observer\n"); snapshot(i);
            event("CONNECT", -1, -1, -1, 2);
        } else if (count == 5 && !strcmp(words[2], "worker")) {
            int index, j;
            if (number(words[4], 1, MAX_DEVICES + 1, &index) || (j = find_job(words[3], index)) < 0 || jobs[j].client >= 0) {
                c->drop = 1; return;
            }
            c->role = 1; c->job = j; jobs[j].client = i;
            message(i, "WELCOME worker %s %d\n", words[3], index);
            event("CONNECT", -1, j, -1, 1); send_job(j);
        } else c->drop = 1;
    } else if (count == 1 && !strcmp(words[0], "PONG")) {
        c->heard = now_ms();
    } else if (count == 1 && !strcmp(words[0], "BYE")) {
        c->drop = 1;
    } else if (c->role == 1 && count == 3 && !strcmp(words[0], "DONE")) {
        int token, result;
        if (number(words[1], 1, 1000000000, &token) || number(words[2], 0, 1, &result)) { c->drop = 1; return; }
        Job *j = &jobs[c->job];
        if ((unsigned int)token == j->token && result != j->result) { c->drop = 1; return; }
        model_complete(c->job, (unsigned int)token, result);
        message(i, "ACK %d\n", token);
    } else c->drop = 1;
    c->heard = now_ms();
}
static void read_client(int i) {
    Client *c = &clients[i]; char data[4096];
    ssize_t n = recv(c->fd, data, sizeof(data), 0);
    if (n == 0) { c->drop = 1; return; }
    if (n < 0) { if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) c->drop = 1; return; }
    for (ssize_t k = 0; k < n && !c->drop; k++) {
        unsigned char ch = (unsigned char)data[k];
        if (ch == '\n') { c->input[c->used] = 0; handle_line(i, c->input); c->used = 0; }
        else if (ch < 32 || ch > 126 || c->used >= LINE_SIZE - 1) c->drop = 1;
        else c->input[c->used++] = (char)ch;
    }
}
static void flush_client(int i) {
    Client *c = &clients[i];
    if (c->length == c->sent || c->drop) return;
    ssize_t n = send(c->fd, c->output + c->sent, (size_t)(c->length - c->sent), 0);
    if (n > 0) { c->sent += (int)n; if (c->sent == c->length) c->sent = c->length = 0; }
    else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) c->drop = 1;
}
static void accept_clients(int listener) {
    for (int k = 0; k < 32; k++) {
        int fd = accept(listener, NULL, NULL);
        if (fd < 0) return;
        int i;
        for (i = 0; i < MAX_CLIENTS; i++) if (clients[i].fd < 0) break;
        if (i == MAX_CLIENTS || nonblock(fd)) { close(fd); continue; }
        Client *c = &clients[i]; memset(c, 0, sizeof(*c));
        c->fd = fd; c->job = -1; c->heard = c->ping = now_ms();
    }
}
int main(int argc, char **argv) {
    int port;
    if (argc == 2 && !strcmp(argv[1], "--help")) { read_config(argc, argv); return 0; }
    if (argc < 3 || number(argv[2], 1, 65535, &port)) {
        fprintf(stderr, "Запуск: center_server IPv4 PORT [параметры модели]\n"); return 2;
    }
    int result = read_config(argc - 2, argv + 2);
    if (result) return result == 1 ? 0 : 2;
    if (setup_signals()) { perror("sigaction"); return 1; }
    if (config.log_path[0]) {
        log_fd = open(config.log_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (log_fd < 0) { perror("log"); return 1; }
    }
    int listener = listen_server(argv[1], port);
    if (listener < 0) { perror("listen"); if (log_fd >= 0) close(log_fd); return 1; }
    for (int i = 0; i < MAX_CLIENTS; i++) clients[i].fd = -1;
    model_init(); started = now_ms();
    char text[LINE_SIZE];
    snprintf(text, sizeof(text), "READY ip=%s port=%d total=%d directions=%d jobs=%d seed=%u\n", argv[1], port, config.parcels, config.directions, job_count, config.seed);
    record(text);
    for (int j = 0; j < job_count; j++) {
        snprintf(text, sizeof(text), "PLAN job=%d role=%s index=%d\n", j, role_name(jobs[j].role), jobs[j].index); record(text);
    }
    const char *reason = "complete"; int code = 0, work_started = 0;
    while (delivered + rejected < config.parcels) {
        if (caught_signal) { reason = "signal"; code = 128 + caught_signal; break; }
        if (output_error || model_error) { reason = "error"; code = 1; break; }
        // не завершаем законную долгую обработку по таймеру простоя
        int running = 0;
        for (int j = 0; j < job_count; j++) if (jobs[j].parcel >= 0 && !jobs[j].finished && jobs[j].client >= 0) running = 1;
        if (!running && now_ms() - last_progress > (long long)config.startup_timeout * 1000) {
            reason = "idle_timeout"; code = 1; break;
        }
        struct pollfd fds[MAX_CLIENTS + 1];
        fds[0] = (struct pollfd){listener, POLLIN, 0};
        for (int i = 0; i < MAX_CLIENTS; i++) {
            short flags = POLLIN;
            if (clients[i].length > clients[i].sent) flags |= POLLOUT;
            fds[i + 1] = (struct pollfd){clients[i].fd, flags, 0};
        }
        int ready = poll(fds, MAX_CLIENTS + 1, 10);
        if (ready < 0 && errno != EINTR) { reason = "poll_error"; code = 1; break; }
        if (fds[0].revents & POLLIN) accept_clients(listener);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            Client *c = &clients[i]; if (c->fd < 0) continue;
            short flags = fds[i + 1].revents;
            if (flags & POLLIN) read_client(i);
            if (flags & POLLOUT) flush_client(i);
            if (flags & (POLLERR | POLLHUP | POLLNVAL)) c->drop = 1;
            long long now = now_ms();
            if (now - c->heard > 5000) c->drop = 1;
            if (c->role && now - c->ping >= 1000) { queue_text(i, "PING\n"); c->ping = now; }
            if (c->drop) disconnect_client(i);
        }
        if (!work_started) {
            int online = 0;
            for (int j = 0; j < job_count; j++) if (jobs[j].client >= 0) online++;
            if (online == job_count) {
                work_started = 1; last_progress = now_ms(); event("RUN", -1, -1, -1, online);
            }
        }
        if (work_started) model_step();
    }
    if (model_check() || model_error || output_error) { reason = "error"; code = 1; }
    stopping = 1; close(listener);
    model_summary(text, sizeof(text), reason); record(text);
    for (int i = 0; i < MAX_CLIENTS; i++) if (clients[i].fd >= 0) {
        queue_text(i, text); message(i, "STOP %s\n", reason);
    }
    // ограниченное время на доставку итогов, даже если клиент перестал читать
    long long end = now_ms() + 2000;
    for (;;) {
        int pending = 0;
        for (int i = 0; i < MAX_CLIENTS; i++) if (clients[i].fd >= 0) {
            flush_client(i);
            Client *c = &clients[i];
            if (c->drop) { disconnect_client(i); continue; }
            if (c->length == c->sent && !c->half_closed) {
                shutdown(c->fd, SHUT_WR); c->half_closed = 1;
            }
            char unused[4096];
            ssize_t n = recv(c->fd, unused, sizeof(unused), 0);
            if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) disconnect_client(i);
            else pending++;
        }
        if (!pending || now_ms() >= end) break;
        poll(NULL, 0, 10);
    }
    for (int i = 0; i < MAX_CLIENTS; i++) disconnect_client(i);
    record("CLEANUP sockets_closed=1\n");
    if (log_fd >= 0 && close(log_fd)) output_error = 1;
    return output_error ? 1 : code;
}
