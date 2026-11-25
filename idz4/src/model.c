#include "model.h"
#include <stdio.h>
#include <string.h>
Parcel parcels[MAX_PARCELS];
Queue queues[MAX_QUEUES];
Job jobs[MAX_JOBS];
int job_count, queue_count, formed, delivered, rejected, model_error;
unsigned long long sequence;
long long last_progress;
static unsigned int next_token;
static int next_parcel;
static unsigned int random_value(Parcel *p) {
    p->random = p->random * 1664525u + 1013904223u;
    return p->random;
}
const char *role_name(int role) {
    switch (role) {
        case SOURCE_ROLE: return "source";
        case SCANNER_ROLE: return "scanner";
        case SORTER_ROLE: return "sorter";
        case BELT_ROLE: return "belt";
        case UNLOAD_ROLE: return "unload";
        default: return "none";
    }
}
int find_job(const char *role, int index) {
    for (int i = 0; i < job_count; i++)
        if (jobs[i].index == index && strcmp(role_name(jobs[i].role), role) == 0) return i;
    return -1;
}
static void add_jobs(int role, int count) {
    for (int i = 1; i <= count; i++) {
        Job *j = &jobs[job_count++];
        j->role = role; j->index = i; j->client = -1; j->parcel = -1;
    }
}
void model_init(void) {
    queue_count = 3 + 2 * config.directions;
    for (int i = 0; i < queue_count; i++) {
        queues[i].limit = config.buffer_capacity;
        if (queues[i].limit > MAX_PARCELS) queues[i].limit = MAX_PARCELS;
        queues[i].stage = BELT_QUEUE;
    }
    queues[0].stage = INPUT; queues[1].stage = SORT_QUEUE;
    for (int i = 0; i < config.directions; i++) {
        int q = 2 + config.directions + i;
        queues[q].stage = OUTPUT;
        queues[q].limit = config.output_capacity;
        if (queues[q].limit > MAX_PARCELS) queues[q].limit = MAX_PARCELS;
    }
    queues[queue_count - 1].stage = MANUAL;
    for (int i = 0; i < config.parcels; i++) {
        parcels[i].id = i + 1;
        parcels[i].random = config.seed ^ ((unsigned int)(i + 1) * 2654435761u);
        parcels[i].direction = (int)(random_value(&parcels[i]) % (unsigned int)config.directions);
    }
    add_jobs(SOURCE_ROLE, config.lines); add_jobs(SCANNER_ROLE, config.scanners);
    add_jobs(SORTER_ROLE, config.sorters); add_jobs(BELT_ROLE, config.directions);
    add_jobs(UNLOAD_ROLE, config.directions + 1);
    last_progress = now_ms();
}
static int input_queue(Job *j) {
    if (j->role == SCANNER_ROLE) return 0;
    if (j->role == SORTER_ROLE) return 1;
    if (j->role == BELT_ROLE) return 2 + j->index - 1;
    if (j->role == UNLOAD_ROLE) {
        if (j->index == config.directions + 1) return queue_count - 1;
        return 2 + config.directions + j->index - 1;
    }
    return -1;
}
static int working_stage(Job *j) {
    if (j->role == SOURCE_ROLE) return SOURCE;
    if (j->role == SCANNER_ROLE) return SCAN;
    if (j->role == SORTER_ROLE) return SORT;
    if (j->role == BELT_ROLE) return BELT;
    return j->index == config.directions + 1 ? MANUAL : OUTPUT;
}
static void start_job(int index) {
    Job *j = &jobs[index]; Parcel *p = &parcels[j->parcel];
    int units = 1;
    if (j->role == SOURCE_ROLE) units = 1 + (int)(random_value(p) % (unsigned int)config.arrival_interval);
    if (j->role == SCANNER_ROLE) {
        units = config.scan_time; p->attempts++;
        j->result = random_value(p) % 100 >= (unsigned int)config.error_percent;
    }
    if (j->role == SORTER_ROLE) units = config.sort_time;
    if (j->role == BELT_ROLE) units = (config.conveyor_length + config.conveyor_speed - 1) / config.conveyor_speed;
    if (j->role == UNLOAD_ROLE) units = config.unload_interval;
    j->duration = (long long)units * config.delay_ms;
    j->token = ++next_token; j->finished = 0; j->waiting = 0;
    p->stage = working_stage(j);
    last_progress = now_ms();
    event("START", j->parcel, index, -1, j->result);
    send_job(index);
}
void model_complete(int index, unsigned int token, int result) {
    Job *j = &jobs[index];
    // повторный ответ не меняет состояние второй раз
    if (j->parcel < 0 || j->token != token || j->finished) return;
    if (result != j->result) { model_error = 1; return; }
    j->finished = 1;
    if (j->role == SCANNER_ROLE && result) parcels[j->parcel].recognized = 1;
    if (j->role == SOURCE_ROLE) formed++;
    last_progress = now_ms();
    event("FINISH", j->parcel, index, -1, result);
}
static int output_queue(Job *j) {
    Parcel *p = &parcels[j->parcel];
    if (j->role == SOURCE_ROLE) return 0;
    if (j->role == SCANNER_ROLE) return j->result ? 1 : queue_count - 1;
    if (j->role == SORTER_ROLE) return 2 + p->direction;
    if (j->role == BELT_ROLE) return 2 + config.directions + p->direction;
    return -1;
}
void model_step(void) {
    for (int i = 0; i < job_count; i++) {
        Job *j = &jobs[i];
        if (j->parcel >= 0 && j->finished) {
            Parcel *p = &parcels[j->parcel];
            if (j->role == SCANNER_ROLE && !j->result && p->attempts <= config.retries) {
                // повтор остаётся на сканере и не блокирует заполненный вход
                if (j->client >= 0) { event("RETRY", j->parcel, i, -1, 0); start_job(i); }
                continue;
            }
            int q = output_queue(j);
            if (q >= 0 && queues[q].count >= queues[q].limit) {
                if (!j->waiting) { j->waiting = 1; event("WAIT", j->parcel, i, q, 0); }
                continue;
            }
            int parcel = j->parcel;
            if (q >= 0) {
                Queue *queue = &queues[q];
                queue->items[(queue->head + queue->count) % MAX_PARCELS] = parcel;
                queue->count++; p->stage = queue->stage;
            } else if (j->index == config.directions + 1) {
                p->stage = REJECTED; rejected++;
            } else { p->stage = DONE; delivered++; }
            j->parcel = -1; j->finished = 0;
            last_progress = now_ms();
            event(q >= 0 ? "PUT" : "FINAL", parcel, i, q, p->stage);
        }
        if (j->parcel >= 0 || j->client < 0) continue;
        int q = input_queue(j), parcel;
        if (j->role == SOURCE_ROLE) {
            if (next_parcel >= config.parcels) continue;
            parcel = next_parcel++;
        } else {
            Queue *queue = &queues[q];
            if (!queue->count) continue;
            parcel = queue->items[queue->head];
            queue->head = (queue->head + 1) % MAX_PARCELS; queue->count--;
        }
        j->parcel = parcel; j->result = 0;
        parcels[parcel].stage = working_stage(j);
        event("TAKE", parcel, i, q, 0);
        start_job(i);
    }
}
int model_check(void) {
    int seen[MAX_PARCELS] = {0}, done = 0, refused = 0;
    for (int q = 0; q < queue_count; q++) {
        Queue *a = &queues[q];
        if (a->count < 0 || a->count > a->limit) return -1;
        for (int k = 0; k < a->count; k++) {
            int n = a->items[(a->head + k) % MAX_PARCELS];
            if (n < 0 || n >= config.parcels || parcels[n].stage != a->stage || seen[n]++) return -1;
            if (a->stage == BELT_QUEUE && parcels[n].direction != q - 2) return -1;
            if (a->stage == OUTPUT && parcels[n].direction != q - 2 - config.directions) return -1;
        }
    }
    for (int j = 0; j < job_count; j++) {
        int n = jobs[j].parcel;
        if (n < 0) continue;
        if (n >= config.parcels || seen[n]++ || parcels[n].stage != working_stage(&jobs[j])) return -1;
        if (jobs[j].role == BELT_ROLE && parcels[n].direction != jobs[j].index - 1) return -1;
    }
    for (int n = 0; n < config.parcels; n++) {
        Parcel *p = &parcels[n];
        if (p->id != n + 1 || p->attempts > config.retries + 1) return -1;
        if (p->stage == NEW || p->stage == DONE || p->stage == REJECTED) { if (seen[n]) return -1; }
        else if (seen[n] != 1) return -1;
        if ((p->stage == SORT_QUEUE || p->stage == SORT || p->stage == BELT_QUEUE ||
             p->stage == BELT || p->stage == OUTPUT || p->stage == DONE) && !p->recognized) return -1;
        if (p->stage == DONE) done++;
        if (p->stage == REJECTED) refused++;
    }
    return done == delivered && refused == rejected && formed <= config.parcels ? 0 : -1;
}
void model_summary(char *text, size_t size, const char *reason) {
    int attempts = 0;
    for (int i = 0; i < config.parcels; i++) attempts += parcels[i].attempts;
    snprintf(text, size, "SUMMARY reason=%s total=%d formed=%d delivered=%d rejected=%d pending=%d attempts=%d\n",
             reason, config.parcels, formed, delivered, rejected, config.parcels - delivered - rejected, attempts);
}
