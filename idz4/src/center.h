#ifndef CENTER_H
#define CENTER_H
#include <signal.h>
#include <stdint.h>
#define MAX_PARCELS 512
#define MAX_DEVICES 16
#define MAX_JOBS (5 * MAX_DEVICES + 1)
#define MAX_QUEUES (2 * MAX_DEVICES + 3)
#define MAX_CLIENTS 256
#define LINE_SIZE 512
#define OUT_SIZE 131072

typedef struct {
    int parcels, directions, lines, scanners, sorters;
    int buffer_capacity, output_capacity;
    int arrival_interval, scan_time, sort_time;
    int conveyor_length, conveyor_speed, unload_interval;
    int error_percent, retries, delay_ms, check, startup_timeout;
    unsigned int seed;
    char log_path[4096];
} Config;
extern Config config;
extern volatile sig_atomic_t caught_signal;
extern int output_error;
int read_config(int argc, char **argv);
void print_text(const char *text);
int write_all(int fd, const char *text);
int setup_signals(void);
long long now_ms(void);
int number(const char *text, int min, int max, int *value);
int nonblock(int fd);
int connect_server(const char *ip, int port);
int listen_server(const char *ip, int port);
#endif
