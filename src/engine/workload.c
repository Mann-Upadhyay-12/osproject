#include "prism/workload.h"
#include "prism/prng.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static char *trim_whitespace(char *str) {
    while (isspace((unsigned char)*str)) str++;
    if (*str == 0) return str;
    char *end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return str;
}

static void parse_bursts(sim_t *sim, sim_pid_t pid, char *burst_str) {
    char *token = strtok(burst_str, ",");
    while (token) {
        token = trim_whitespace(token);
        if (token[0] == 'C' || token[0] == 'c') {
            int dur = atoi(token + 1);
            if (dur > 0) sim_add_burst(sim, pid, BURST_CPU, (uint16_t)dur);
        } else if (token[0] == 'I' || token[0] == 'i') {
            int dur = atoi(token + 1);
            if (dur > 0) sim_add_burst(sim, pid, BURST_IO, (uint16_t)dur);
        } else if (token[0] == 'A' || token[0] == 'a') {
            int rid = atoi(token + 1);
            if (sim->procs[pid].num_bursts < PRISM_MAX_BURSTS) {
                burst_t *b = &sim->procs[pid].bursts[sim->procs[pid].num_bursts++];
                b->kind = BURST_ACQUIRE;
                b->duration = 1;
                b->remaining = 1;
                b->target_id = (int16_t)rid;
            }
        } else if (token[0] == 'R' || token[0] == 'r') {
            int rid = atoi(token + 1);
            if (sim->procs[pid].num_bursts < PRISM_MAX_BURSTS) {
                burst_t *b = &sim->procs[pid].bursts[sim->procs[pid].num_bursts++];
                b->kind = BURST_RELEASE;
                b->duration = 1;
                b->remaining = 1;
                b->target_id = (int16_t)rid;
            }
        } else if (token[0] == 'S' || token[0] == 's') {
            int target = atoi(token + 1);
            if (sim->procs[pid].num_bursts < PRISM_MAX_BURSTS) {
                burst_t *b = &sim->procs[pid].bursts[sim->procs[pid].num_bursts++];
                b->kind = BURST_IPC_SEND;
                b->duration = 1;
                b->remaining = 1;
                b->target_id = (int16_t)target;
                b->extra_data = 100;
            }
        } else if (token[0] == 'W' || token[0] == 'w') {
            int sender = atoi(token + 1);
            if (sim->procs[pid].num_bursts < PRISM_MAX_BURSTS) {
                burst_t *b = &sim->procs[pid].bursts[sim->procs[pid].num_bursts++];
                b->kind = BURST_IPC_RECV;
                b->duration = 1;
                b->remaining = 1;
                b->target_id = (int16_t)sender;
            }
        } else if (token[0] == 'K' || token[0] == 'k') {
            if (sim->procs[pid].num_bursts < PRISM_MAX_BURSTS) {
                burst_t *b = &sim->procs[pid].bursts[sim->procs[pid].num_bursts++];
                b->kind = BURST_CHECKPOINT;
                b->duration = 1;
                b->remaining = 1;
            }
        }
        token = strtok(NULL, ",");
    }
}

static void parse_pages(sim_t *sim, sim_pid_t pid, char *page_str) {
    page_str = trim_whitespace(page_str);
    if (strncmp(page_str, "loop:", 5) == 0) {
        int min_p = 0, max_p = 3, loops = 1;
        sscanf(page_str + 5, "%d:%d:%d", &min_p, &max_p, &loops);
        for (int l = 0; l < loops; l++) {
            for (int p = min_p; p <= max_p; p++) {
                sim_add_page_ref(sim, pid, (uint16_t)p);
            }
        }
    } else if (strncmp(page_str, "seq:", 4) == 0) {
        int min_p = 0, max_p = 5;
        sscanf(page_str + 4, "%d:%d", &min_p, &max_p);
        for (int p = min_p; p <= max_p; p++) {
            sim_add_page_ref(sim, pid, (uint16_t)p);
        }
    } else if (strncmp(page_str, "random:", 7) == 0) {
        int min_p = 0, max_p = 7, count = 10;
        sscanf(page_str + 7, "%d:%d:%d", &min_p, &max_p, &count);
        uint64_t prng = sim->seed + pid * 0x9e3779b97f4a7c15ULL;
        for (int i = 0; i < count; i++) {
            uint32_t val = prng_range(&prng, (uint32_t)min_p, (uint32_t)max_p);
            sim_add_page_ref(sim, pid, (uint16_t)val);
        }
    } else {
        char *token = strtok(page_str, ",");
        while (token) {
            token = trim_whitespace(token);
            if (*token) {
                int p = atoi(token);
                sim_add_page_ref(sim, pid, (uint16_t)p);
            }
            token = strtok(NULL, ",");
        }
    }
}

bool workload_load_file(const char *filepath, sim_t *sim, workload_config_t *cfg) {
    FILE *fp = fopen(filepath, "r");
    if (!fp) return false;

    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        char *p = trim_whitespace(line);
        if (*p == '\0' || *p == '#' || *p == ';') continue;

        if (strncmp(p, "cores", 5) == 0) {
            int cores = atoi(p + 5);
            if (cfg) { cfg->cores = cores; cfg->has_cores = true; }
            if (sim) sim->num_cores = cores;
        } else if (strncmp(p, "frames", 6) == 0) {
            int frames = atoi(p + 6);
            if (cfg) { cfg->frames = frames; cfg->has_frames = true; }
            if (sim) sim->total_frames = frames;
        } else if (strncmp(p, "disk_latency", 12) == 0) {
            int lat = atoi(p + 12);
            if (cfg) { cfg->disk_latency = lat; cfg->has_disk_latency = true; }
            if (sim) sim->disk_latency = lat;
        } else if (strncmp(p, "resource", 8) == 0) {
            char rname[32];
            int rinst = 1;
            if (sscanf(p + 8, "%31s %d", rname, &rinst) >= 1) {
                if (sim) sim_add_resource(sim, rname, rinst);
            }
        } else if (strncmp(p, "sched", 5) == 0) {
            char val[32];
            sscanf(p + 5, "%31s", val);
            sched_policy_t sp = SCHED_FCFS;
            if (strcmp(val, "rr") == 0) sp = SCHED_RR;
            else if (strcmp(val, "sjf") == 0) sp = SCHED_SJF;
            else if (strcmp(val, "srtf") == 0) sp = SCHED_SRTF;
            else if (strcmp(val, "mlfq") == 0) sp = SCHED_MLFQ;
            else if (strcmp(val, "adaptive") == 0) sp = SCHED_ADAPTIVE;
            else if (strcmp(val, "cfs") == 0) sp = SCHED_CFS;
            else if (strcmp(val, "bfs") == 0) sp = SCHED_BFS;
            if (cfg) { cfg->sched = sp; cfg->has_sched = true; }
            if (sim) sim->sched_policy = sp;
        } else if (strncmp(p, "repl", 4) == 0) {
            char val[32];
            sscanf(p + 4, "%31s", val);
            repl_policy_t rp = REPL_FIFO;
            if (strcmp(val, "lru") == 0) rp = REPL_LRU;
            else if (strcmp(val, "clock") == 0) rp = REPL_CLOCK;
            else if (strcmp(val, "opt") == 0) rp = REPL_OPT;
            if (cfg) { cfg->repl = rp; cfg->has_repl = true; }
            if (sim) sim->repl_policy = rp;
        } else if (strncmp(p, "quantum", 7) == 0) {
            int q = atoi(p + 7);
            if (cfg) { cfg->quantum = (uint16_t)q; cfg->has_quantum = true; }
            if (sim) sim->default_quantum = (uint16_t)q;
        } else if (strncmp(p, "seed", 4) == 0) {
            uint64_t seed = strtoull(p + 4, NULL, 10);
            if (cfg) { cfg->seed = seed; cfg->has_seed = true; }
            if (sim) { sim->seed = seed; sim->prng_state = seed; }
        } else if (strncmp(p, "proc", 4) == 0) {
            char name[PRISM_MAX_NAME_LEN] = "proc";
            int arrival = 0;
            char bursts_buf[256] = "";
            char pages_buf[256] = "";

            char *token = strtok(p + 4, " \t");
            if (token) snprintf(name, sizeof(name), "%s", token);

            while ((token = strtok(NULL, " \t")) != NULL) {
                if (strncmp(token, "arrival=", 8) == 0) {
                    arrival = atoi(token + 8);
                } else if (strncmp(token, "bursts=", 7) == 0) {
                    strncpy(bursts_buf, token + 7, sizeof(bursts_buf) - 1);
                } else if (strncmp(token, "pages=", 6) == 0) {
                    strncpy(pages_buf, token + 6, sizeof(pages_buf) - 1);
                }
            }

            if (sim) {
                sim_pid_t pid = sim_add_process(sim, name, (tick_t)arrival);
                if (pid >= 0) {
                    if (bursts_buf[0]) parse_bursts(sim, pid, bursts_buf);
                    if (pages_buf[0])  parse_pages(sim, pid, pages_buf);
                }
            }
        }
    }

    fclose(fp);
    return true;
}

bool workload_generate_mixed(sim_t *sim, int num_procs, uint64_t seed) {
    uint64_t st = seed ? seed : 0x42ULL;
    for (int i = 0; i < num_procs; i++) {
        char name[PRISM_MAX_NAME_LEN];
        snprintf(name, sizeof(name), "P%d", i + 1);
        tick_t arrival = (tick_t)(i * 2);
        sim_pid_t pid = sim_add_process(sim, name, arrival);
        if (pid < 0) return false;

        /* Mix CPU and I/O */
        int type = prng_range(&st, 0, 2);
        if (type == 0) {
            /* CPU bound */
            sim_add_burst(sim, pid, BURST_CPU, 20);
            for (int r = 0; r < 12; r++) {
                sim_add_page_ref(sim, pid, (uint16_t)prng_range(&st, 0, 4));
            }
        } else if (type == 1) {
            /* I/O bound */
            sim_add_burst(sim, pid, BURST_CPU, 3);
            sim_add_burst(sim, pid, BURST_IO, 8);
            sim_add_burst(sim, pid, BURST_CPU, 3);
            for (int r = 0; r < 4; r++) {
                sim_add_page_ref(sim, pid, (uint16_t)prng_range(&st, 0, 2));
            }
        } else {
            /* Interactive */
            sim_add_burst(sim, pid, BURST_CPU, 6);
            sim_add_burst(sim, pid, BURST_IO, 4);
            sim_add_burst(sim, pid, BURST_CPU, 6);
            for (int r = 0; r < 8; r++) {
                sim_add_page_ref(sim, pid, (uint16_t)prng_range(&st, 0, 5));
            }
        }
    }
    return true;
}

bool workload_generate_belady(sim_t *sim) {
    /* Classic Belady reference string: 1, 2, 3, 4, 1, 2, 5, 1, 2, 3, 4, 5 */
    static const uint16_t belady_refs[] = {1, 2, 3, 4, 1, 2, 5, 1, 2, 3, 4, 5};
    sim_pid_t pid = sim_add_process(sim, "BeladyProc", 0);
    if (pid < 0) return false;
    sim_add_burst(sim, pid, BURST_CPU, 12);
    for (size_t i = 0; i < sizeof(belady_refs) / sizeof(belady_refs[0]); i++) {
        sim_add_page_ref(sim, pid, belady_refs[i]);
    }
    return true;
}

bool workload_generate_thrashing(sim_t *sim, int num_procs) {
    for (int i = 0; i < num_procs; i++) {
        char name[PRISM_MAX_NAME_LEN];
        snprintf(name, sizeof(name), "Thrash%d", i + 1);
        sim_pid_t pid = sim_add_process(sim, name, (tick_t)i);
        if (pid < 0) return false;
        sim_add_burst(sim, pid, BURST_CPU, 30);
        /* Working set of 3 pages per process.
         * For 4 processes, total working set = 12 pages.
         * With 12 frames: fits completely (cold faults only).
         * With 6 frames: partial thrashing.
         * With 3 frames: severe thrashing. */
        for (int p = 0; p < 25; p++) {
            sim_add_page_ref(sim, pid, (uint16_t)(p % 3));
        }
    }
    return true;
}
