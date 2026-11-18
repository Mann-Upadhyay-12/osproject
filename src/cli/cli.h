#ifndef PRISM_CLI_H
#define PRISM_CLI_H

#include "prism/sim.h"
#include <stdio.h>

#define ANSI_RESET   "\033[0m"
#define ANSI_BOLD    "\033[1m"
#define ANSI_CYAN    "\033[36m"
#define ANSI_GREEN   "\033[32m"
#define ANSI_YELLOW  "\033[33m"
#define ANSI_RED     "\033[31m"
#define ANSI_MAGENTA "\033[35m"
#define ANSI_BLUE    "\033[34m"
#define ANSI_DIM     "\033[2m"

sim_t *cli_sim_alloc(void);

void report_banner(void);
void report_snapshot(const sim_t *s, bool show_gantt);
void report_resources(const sim_t *s);
void report_csv(const sim_t *s);

void experiment_schedulers(const char *workload_path);
void experiment_page_replacement(void);
void experiment_thrashing(void);
void demo_rewind(void);
void demo_compare_schedulers(const char *workload_path);
void demo_compare_replacement(void);
void demo_deadlock(void);

void interactive_shell(sim_t *sim, bool show_gantt);

#endif
