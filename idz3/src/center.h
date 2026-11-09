#ifndef CENTER_H
#define CENTER_H

#include <semaphore.h>
#include <signal.h>
#include <sys/types.h>
#include <time.h>

#define MAX_PARCELS 512
#define MAX_DEVICES 64
#define MAX_QUEUES (2 * MAX_DEVICES + 3)
#define MAX_WORKERS (MAX_PARCELS + 5 * MAX_DEVICES + 1)
#define MAX_SEMS 1100

typedef struct {
    int parcels, directions, lines, scanners, sorters;
    int buffer_capacity, output_capacity;
    int arrival_interval, scan_time, sort_time;
    int conveyor_length, conveyor_speed, unload_interval;
    int error_percent, retries, delay_ms, check, startup_timeout;
    unsigned int seed;
    char log_path[4096];
} Config;

enum Stage {
    NEW, SOURCE, INPUT, SCAN, SCANNED, SORT_QUEUE, SORT, SORTED,
    BELT_QUEUE, BELT, TRANSPORTED, OUTPUT, MANUAL, DONE, REJECTED
};
enum Role { SCANNER, SORTER, CONVEYOR };
enum Job { PARCEL_JOB, SOURCE_JOB, DEVICE_JOB, UNLOAD_JOB };

typedef struct {
    int id, direction, attempts, recognized, source, owner, ready;
    unsigned int random;
    enum Stage stage;
} Parcel;

typedef struct {
    int items[MAX_PARCELS];
    int count, limit, head, direction, free_slots, used_slots;
    char name[16];
} Queue;

typedef struct {
    int id, parcel, repeat, input, released;
    enum Role role;
} Device;

typedef struct { int id, parcel; } Source;
typedef struct { enum Job role; int index, finished; pid_t pid; } Worker;

// в общей памяти храним номера элементов, а не адреса другого процесса
// таблицы имеют фиксированный верхний предел, как и параметры запуска

typedef struct {
    unsigned int magic;
    Config config;
    int named, sem_count, queue_count, device_count, worker_count;
    int registered, running, stopping, model_error, stop_signal, start_gate;
    int formed, delivered, rejected;
    unsigned long long attempts, errors, repeats, waits, sequence;
    struct timespec start_time;
    char error_text[160];
    pid_t controller;
    Parcel parcels[MAX_PARCELS];
    Queue queues[MAX_QUEUES];
    Device devices[3 * MAX_DEVICES];
    Source sources[MAX_DEVICES];
    Worker workers[MAX_WORKERS];
    sem_t sems[MAX_SEMS];
} Shared;

extern Config config;
extern Shared *shared;
extern int output_error;
extern volatile sig_atomic_t caught_signal;
extern int ipc_owner;

int read_config(int argc, char *argv[]);
int open_log(int append);
void print_text(const char *text);
int close_log(void);
int setup_signals(void);
int ipc_create(const char *session, int named);
int ipc_publish(void);
int ipc_attach(const char *session);
int ipc_counter(int value);
int ipc_wait(int number, int milliseconds);
int ipc_post(int number);
void ipc_close(void);
void model_lock(void);
void model_unlock(void);
void model_stop(int number);
void model_fail(const char *text);
int prepare_model(void);
int check_model(void);
int run_worker(int slot);
void print_config(void);
int print_result(void);
const char *job_name(enum Job role);
int run_controller(const char *session, int mode);
int worker_main(int argc, char *argv[]);
#endif
