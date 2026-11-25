#ifndef MODEL_H
#define MODEL_H
#include "center.h"
#include <stddef.h>
enum { NEW, SOURCE, INPUT, SCAN, SORT_QUEUE, SORT, BELT_QUEUE, BELT, OUTPUT, MANUAL, DONE, REJECTED };
enum { SOURCE_ROLE = 1, SCANNER_ROLE, SORTER_ROLE, BELT_ROLE, UNLOAD_ROLE };
typedef struct { int id, direction, attempts, recognized, stage; unsigned int random; } Parcel;
typedef struct { int items[MAX_PARCELS], head, count, limit, stage; } Queue;
typedef struct {
    int role, index, client, parcel, finished, waiting, result;
    unsigned int token;
    long long duration;
} Job;
extern Parcel parcels[MAX_PARCELS];
extern Queue queues[MAX_QUEUES];
extern Job jobs[MAX_JOBS];
extern int job_count, queue_count, formed, delivered, rejected, model_error;
extern unsigned long long sequence;
extern long long last_progress;
const char *role_name(int role);
int find_job(const char *role, int index);
void model_init(void);
void model_step(void);
void model_complete(int job, unsigned int token, int result);
int model_check(void);
void model_summary(char *text, size_t size, const char *reason);
void event(const char *name, int parcel, int job, int queue, int result);
void send_job(int index);
#endif
