#include "cli.h"
#include "prism/sim_api.h"
#include "prism/workload.h"
#include <ctype.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#define EXIT_USAGE      2
#define SHELL_HISTORY   500

typedef struct {
    const char     *workload_file;
    int             cores;
    int             frames;
    int             latency;
    sched_policy_t  sched;
    repl_policy_t   repl;
    uint64_t        seed;
    const char     *sched_name;
    const char     *repl_name;
    bool            cores_given;
    bool            frames_given;
    bool            latency_given;
    bool            csv;
    bool            gantt;
    bool            step_trace;
    bool            interactive;
    bool            ran_experiment;
} cli_options_t;

static void print_usage(FILE *out) {
    fprintf(out,
        "Usage: prism-cli [options]\n"
        "  --workload FILE     scenario/workload file (default: built-in mixed workload)\n"
        "  --cores N           CPU cores (default 1)\n"
        "  --frames N          physical frames (default 4)\n"
        "  --latency N         disk latency of a page fault, in ticks (default 3)\n"
        "  --sched NAME        fcfs | sjf | srtf | rr | mlfq | adaptive | cfs | bfs | rm | edf | smp (default rr)\n"
        "  --repl NAME         fifo | lru | clock | opt (default lru)\n"
        "  --seed N            PRNG seed (default 42)\n"
        "  --csv               per-process results as CSV\n"
        "  --no-gantt          omit the Gantt chart\n"
        "  --step, -s          print the machine state after every tick\n"
        "  --interactive, -i   step / rewind / inject faults from a prompt\n"
        "  --exp1 | --exp2 | --exp3 | --all-exp\n"
        "  --compare-sched | --compare-vm | --deadlock-demo | --rewind-demo\n"
        "  --help, -h\n"
        "Flags given explicitly override the matching line of the workload file.\n");
}

static bool is_flag(const char *arg, const char *long_name, const char *short_name) {
    return strcmp(arg, long_name) == 0 || (short_name && strcmp(arg, short_name) == 0);
}

static bool run_demo_flag(const char *arg, cli_options_t *opt) {
    if      (is_flag(arg, "--exp1", NULL))            experiment_schedulers(opt->workload_file);
    else if (is_flag(arg, "--exp2", NULL))            experiment_page_replacement();
    else if (is_flag(arg, "--exp3", NULL))            experiment_thrashing();
    else if (is_flag(arg, "--compare-sched", NULL))   demo_compare_schedulers(opt->workload_file);
    else if (is_flag(arg, "--compare-vm", NULL))      demo_compare_replacement();
    else if (is_flag(arg, "--deadlock-demo", NULL))   demo_deadlock();
    else if (is_flag(arg, "--rewind-demo", NULL)) {
        demo_rewind();
        return true;
    } else if (is_flag(arg, "--all-exp", "--experiments")) {
        demo_compare_schedulers(opt->workload_file);
        demo_compare_replacement();
        demo_deadlock();
        demo_rewind();
    } else {
        return false;
    }
    opt->ran_experiment = true;
    return true;
}

static int parse_args(int argc, char **argv, cli_options_t *opt) {
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        bool has_value = i + 1 < argc;

        if (has_value && is_flag(arg, "--workload", NULL)) {
            opt->workload_file = argv[++i];
        } else if (has_value && is_flag(arg, "--cores", NULL)) {
            opt->cores = atoi(argv[++i]);
            opt->cores_given = true;
        } else if (has_value && is_flag(arg, "--frames", NULL)) {
            opt->frames = atoi(argv[++i]);
            opt->frames_given = true;
        } else if (has_value && is_flag(arg, "--latency", NULL)) {
            opt->latency = atoi(argv[++i]);
            opt->latency_given = true;
        } else if (has_value && is_flag(arg, "--sched", NULL)) {
            opt->sched_name = argv[++i];
            if (!sched_lookup(opt->sched_name, &opt->sched)) {
                fprintf(stderr, "Error: unknown scheduler '%s' (fcfs, sjf, srtf, rr, mlfq, adaptive, cfs, bfs)\n", opt->sched_name);
                return EXIT_USAGE;
            }
        } else if (has_value && is_flag(arg, "--repl", NULL)) {
            opt->repl_name = argv[++i];
            if (!repl_lookup(opt->repl_name, &opt->repl)) {
                fprintf(stderr, "Error: unknown replacement policy '%s' (fifo, lru, clock, opt)\n", opt->repl_name);
                return EXIT_USAGE;
            }
        } else if (has_value && is_flag(arg, "--seed", NULL)) {
            opt->seed = strtoull(argv[++i], NULL, 10);
        } else if (is_flag(arg, "--csv", NULL)) {
            opt->csv = true;
        } else if (is_flag(arg, "--no-gantt", NULL)) {
            opt->gantt = false;
        } else if (is_flag(arg, "--step", "-s")) {
            opt->step_trace = true;
        } else if (is_flag(arg, "--interactive", "-i")) {
            opt->interactive = true;
        } else if (is_flag(arg, "--help", "-h")) {
            print_usage(stdout);
            return EXIT_SUCCESS;
        } else if (!run_demo_flag(arg, opt)) {
            fprintf(stderr, "Error: unknown or incomplete option '%s'\n\n", arg);
            print_usage(stderr);
            return EXIT_USAGE;
        }
    }
    return -1;
}

static int apply_overrides(sim_t *sim, const cli_options_t *opt) {
    int rc = SIM_OK;
    if (rc == SIM_OK && opt->cores_given)   rc = sim_set_cores(sim, opt->cores);
    if (rc == SIM_OK && opt->frames_given)  rc = sim_set_frames(sim, opt->frames);
    if (rc == SIM_OK && opt->latency_given) rc = sim_set_disk_latency(sim, opt->latency);
    if (rc == SIM_OK && opt->sched_name)    rc = sim_set_sched(sim, opt->sched_name, 0);
    if (rc == SIM_OK && opt->repl_name)     rc = sim_set_repl(sim, opt->repl_name);
    return rc;
}

static int build_simulation(sim_t *sim, const cli_options_t *opt) {
    sim_init(sim, opt->cores, opt->frames, opt->latency, opt->sched, opt->repl, opt->seed);

    if (!opt->workload_file) {
        workload_generate_mixed(sim, 4, opt->seed);
        return EXIT_SUCCESS;
    }

    workload_config_t cfg = {0};
    if (!workload_load_file(opt->workload_file, sim, &cfg)) {
        fprintf(stderr, "Error: Failed to load workload file '%s'\n", opt->workload_file);
        return EXIT_FAILURE;
    }

    int rc = apply_overrides(sim, opt);
    if (rc != SIM_OK) {
        fprintf(stderr, "Error: cannot apply command-line options: %s\n", sim_strerror(rc));
        return EXIT_USAGE;
    }
    return EXIT_SUCCESS;
}

typedef struct {
    sim_t *states;
    int    count;
} history_t;

static void history_push(history_t *h, const sim_t *sim) {
    if (h->states && h->count < SHELL_HISTORY) sim_snapshot(sim, &h->states[h->count++]);
}

static bool history_pop(history_t *h, sim_t *sim) {
    if (h->count <= 1) return false;
    h->count--;
    sim_restore(sim, &h->states[h->count - 1]);
    return true;
}

static void shell_kill(sim_t *sim, const char *args) {
    int pid = atoi(args);
    if (sim_inject_kill(sim, pid)) {
        printf("%s[Injected Fault: Process %d Terminated]%s\n", ANSI_RED, pid, ANSI_RESET);
    } else {
        printf("%s[Failed to kill PID %d]%s\n", ANSI_RED, pid, ANSI_RESET);
    }
}

static void shell_shrink_ram(sim_t *sim, const char *args) {
    int frames = atoi(args);
    if (frames > 0 && sim_inject_shrink_ram(sim, frames)) {
        printf("%s[Injected Fault: RAM shrunk to %d frames]%s\n", ANSI_MAGENTA, frames, ANSI_RESET);
    } else {
        printf("%s[Failed to shrink RAM to %d frames]%s\n", ANSI_RED, frames, ANSI_RESET);
    }
}

void interactive_shell(sim_t *sim, bool show_gantt) {
    history_t history = { .states = malloc(SHELL_HISTORY * sizeof(sim_t)), .count = 0 };
    history_push(&history, sim);

    printf("\n%s%s[CLI INTERACTIVE CONTROL SHELL]%s\n", ANSI_BOLD, ANSI_GREEN, ANSI_RESET);
    printf("Commands: [Enter/s] Step | [r] Run to End | [b] Step Back / Rewind | [k PID] Kill | [d N] Shrink RAM | [q] Quit\n");
    report_snapshot(sim, show_gantt);

    char line[128];
    while (!sim_is_done(sim)) {
        printf("\n%sPRISM (tick %" PRIu64 ") > %s", ANSI_CYAN, sim->tick, ANSI_RESET);
        if (!fgets(line, sizeof(line), stdin)) break;

        char command = (char)tolower((unsigned char)line[0]);
        const char *args = line + 1;

        if (command == 'q') break;

        if (command == 'r') {
            sim_run(sim);
            report_snapshot(sim, show_gantt);
            break;
        }

        if (command == 'b') {
            if (!history_pop(&history, sim)) {
                printf("%s[Cannot rewind further: at initial state]%s\n", ANSI_RED, ANSI_RESET);
                continue;
            }
            printf("%s[Rewound to tick %" PRIu64 "]%s\n", ANSI_YELLOW, sim->tick, ANSI_RESET);
        } else if (command == 'k') {
            shell_kill(sim, args);
        } else if (command == 'd') {
            shell_shrink_ram(sim, args);
        } else {
            history_push(&history, sim);
            sim_step(sim);
        }
        report_snapshot(sim, show_gantt);
    }
    free(history.states);
}

static void run_step_trace(sim_t *sim, bool show_gantt) {
    printf("\n%s%s[EXECUTING STEP TRACE]%s\n", ANSI_BOLD, ANSI_CYAN, ANSI_RESET);
    while (sim_step(sim)) {
        printf("\n%s--- ADVANCED TO TICK %" PRIu64 " ---%s\n", ANSI_BOLD, sim->tick, ANSI_RESET);
        report_snapshot(sim, show_gantt);
    }
}

int main(int argc, char **argv) {
    cli_options_t opt = {
        .cores = 1, .frames = 4, .latency = 3,
        .sched = SCHED_RR, .repl = REPL_LRU, .seed = 42,
        .gantt = true,
    };

    int exit_code = parse_args(argc, argv, &opt);
    if (exit_code >= 0) return exit_code;
    if (opt.ran_experiment) return EXIT_SUCCESS;

    if (!opt.csv) report_banner();

    sim_t *sim = cli_sim_alloc();
    exit_code = build_simulation(sim, &opt);

    if (exit_code != EXIT_SUCCESS) {
        free(sim);
        return exit_code;
    }

    if (opt.interactive) {
        interactive_shell(sim, opt.gantt);
        free(sim);
        return EXIT_SUCCESS;
    }

    if (opt.step_trace) {
        run_step_trace(sim, opt.gantt);
    } else {
        sim_run(sim);
    }

    if (opt.csv) {
        report_csv(sim);
    } else {
        report_snapshot(sim, opt.gantt);
    }
    free(sim);
    return EXIT_SUCCESS;
}
