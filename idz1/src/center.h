#ifndef CENTER_H
#define CENTER_H

#include <signal.h>

typedef struct {
    int parcels, directions, lines, scanners, sorters;
    int buffer_capacity, output_capacity;
    int arrival_interval, scan_time, sort_time;
    int conveyor_length, conveyor_speed, unload_interval;
    int error_percent, retries, delay_ms;
    int check;
    unsigned int seed;
    const char *log_path;
} Config;

extern Config config;
extern int output_error;
extern volatile sig_atomic_t stop_signal;

int read_config(int argc, char *argv[]);
int open_log(void);
void print_text(const char *text);
int close_log(void);
int run_model(void);

#endif
