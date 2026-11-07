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

/* Append one non-CPU/IO control burst (resource, IPC, checkpoint). */
static bool spec_add_control(proc_spec_t *spec, burst_kind_t kind, int target, int16_t data) {
    if (proc_spec_add_burst(spec, kind, 1) != SIM_OK) return false;
    burst_t *b = &spec->bursts[spec->num_bursts - 1];
    b->target_id = (int16_t)target;
    b->extra_data = data;
    return true;
}

bool workload_parse_bursts(proc_spec_t *spec, const char *burst_str) {
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", burst_str);
    bool ok = true;
    for (char *token = strtok(buf, ","); token; token = strtok(NULL, ",")) {
        token = trim_whitespace(token);
        if (*token == '\0') continue;
        int arg = atoi(token + 1);
        switch (toupper((unsigned char)token[0])) {
            case 'C':
            case 'I':
                if (arg <= 0 || arg > UINT16_MAX) { ok = false; break; }
                if (proc_spec_add_burst(spec, toupper((unsigned char)token[0]) == 'C' ? BURST_CPU : BURST_IO,
                                        (uint16_t)arg) != SIM_OK) ok = false;
                break;
            case 'A': ok = spec_add_control(spec, BURST_ACQUIRE, arg, 0) && ok; break;
            case 'R': ok = spec_add_control(spec, BURST_RELEASE, arg, 0) && ok; break;
            case 'S': ok = spec_add_control(spec, BURST_IPC_SEND, arg, 100) && ok; break;
            case 'W': ok = spec_add_control(spec, BURST_IPC_RECV, arg, 0) && ok; break;
            case 'K': ok = spec_add_control(spec, BURST_CHECKPOINT, 0, 0) && ok; break;
            default:  ok = false; break;
        }
    }
    return ok;
}

static bool spec_add_page(proc_spec_t *spec, int page) {
    if (page < 0 || page >= PRISM_MAX_VIRTUAL_PAGES) return false;
    return proc_spec_add_ref(spec, (uint16_t)page) == SIM_OK;
}

bool workload_parse_pages(proc_spec_t *spec, const char *page_src, uint64_t seed, int pid) {
    char buf[2048];
    snprintf(buf, sizeof(buf), "%s", page_src);
    char *page_str = trim_whitespace(buf);
    bool ok = true;
    if (strncmp(page_str, "loop:", 5) == 0) {
        int min_p = 0, max_p = 3, loops = 1;
        sscanf(page_str + 5, "%d:%d:%d", &min_p, &max_p, &loops);
        for (int l = 0; l < loops; l++) {
            for (int p = min_p; p <= max_p; p++) ok = spec_add_page(spec, p) && ok;
        }
    } else if (strncmp(page_str, "seq:", 4) == 0) {
        int min_p = 0, max_p = 5;
        sscanf(page_str + 4, "%d:%d", &min_p, &max_p);
        for (int p = min_p; p <= max_p; p++) ok = spec_add_page(spec, p) && ok;
    } else if (strncmp(page_str, "random:", 7) == 0) {
        int min_p = 0, max_p = 7, count = 10;
        sscanf(page_str + 7, "%d:%d:%d", &min_p, &max_p, &count);
        if (min_p < 0 || max_p < min_p) return false;
        /* Seeded per pid so the same file + seed always yields the same string. */
        uint64_t prng = seed + (uint64_t)pid * 0x9e3779b97f4a7c15ULL;
        for (int i = 0; i < count; i++) {
            uint32_t val = prng_range(&prng, (uint32_t)min_p, (uint32_t)max_p);
            ok = spec_add_page(spec, (int)val) && ok;
        }
    } else {
        for (char *token = strtok(page_str, ","); token; token = strtok(NULL, ",")) {
            token = trim_whitespace(token);
            if (*token) ok = spec_add_page(spec, atoi(token)) && ok;
        }
    }
    return ok;
}

/* Install a parsed spec as-is. Files are trusted to the engine's old rules
 * (e.g. a script may start with a control burst), so this skips
 * proc_spec_validate and does not clamp arrival to "now". */
static sim_pid_t workload_install_spec(sim_t *sim, const proc_spec_t *spec) {
    sim_pid_t pid = sim_add_process(sim, spec->name, spec->arrival);
    if (pid < 0) return pid;
    pcb_t *p = &sim->procs[pid];
    p->priority = spec->priority;
    p->base_priority = spec->priority;
    p->num_bursts = spec->num_bursts;
    memcpy(p->bursts, spec->bursts, spec->num_bursts * sizeof(spec->bursts[0]));
    p->num_page_refs = spec->num_page_refs;
    memcpy(p->page_refs, spec->page_refs, spec->num_page_refs * sizeof(spec->page_refs[0]));
    return pid;
}

bool workload_load_file(const char *filepath, sim_t *sim, workload_config_t *cfg) {
    FILE *fp = fopen(filepath, "r");
    if (!fp) return false;

    char line[4096];
    int lineno = 0;
    while (fgets(line, sizeof(line), fp)) {
        lineno++;
        char *p = trim_whitespace(line);
        if (*p == '\0' || *p == '#' || *p == ';') continue;

        if (strncmp(p, "cores", 5) == 0) {
            int cores = atoi(p + 5);
            if (cores < 1 || cores > PRISM_MAX_CORES) {
                fprintf(stderr, "%s:%d: cores must be 1..%d\n", filepath, lineno, PRISM_MAX_CORES);
                continue;
            }
            if (cfg) { cfg->cores = cores; cfg->has_cores = true; }
            if (sim) sim->num_cores = cores;
        } else if (strncmp(p, "frames", 6) == 0) {
            int frames = atoi(p + 6);
            if (frames < 1 || frames > PRISM_MAX_FRAMES) {
                fprintf(stderr, "%s:%d: frames must be 1..%d\n", filepath, lineno, PRISM_MAX_FRAMES);
                continue;
            }
            if (cfg) { cfg->frames = frames; cfg->has_frames = true; }
            if (sim) { sim->total_frames = frames; sim->configured_frames = frames; }
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
            char val[32] = "";
            sscanf(p + 5, "%31s", val);
            sched_policy_t sp = SCHED_FCFS;
            if (!sched_lookup(val, &sp)) {
                fprintf(stderr, "%s:%d: unknown scheduler '%s', using fcfs\n", filepath, lineno, val);
            }
            if (cfg) { cfg->sched = sp; cfg->has_sched = true; }
            if (sim) sim->sched_policy = sp;
        } else if (strncmp(p, "repl", 4) == 0) {
            char val[32] = "";
            sscanf(p + 4, "%31s", val);
            repl_policy_t rp = REPL_FIFO;
            if (!repl_lookup(val, &rp)) {
                fprintf(stderr, "%s:%d: unknown replacement policy '%s', using fifo\n", filepath, lineno, val);
            }
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
        } else if (strncmp(p, "proc", 4) == 0 && isspace((unsigned char)p[4])) {
            proc_spec_t spec;
            char bursts_buf[1024] = "";
            char pages_buf[2048] = "";

            char *token = strtok(p + 4, " \t");
            proc_spec_init(&spec, token);

            while ((token = strtok(NULL, " \t")) != NULL) {
                if (strncmp(token, "arrival=", 8) == 0) {
                    spec.arrival = (tick_t)strtoull(token + 8, NULL, 10);
                } else if (strncmp(token, "priority=", 9) == 0) {
                    spec.priority = (uint16_t)atoi(token + 9);
                } else if (strncmp(token, "bursts=", 7) == 0) {
                    snprintf(bursts_buf, sizeof(bursts_buf), "%s", token + 7);
                } else if (strncmp(token, "pages=", 6) == 0) {
                    snprintf(pages_buf, sizeof(pages_buf), "%s", token + 6);
                }
            }

            if (sim) {
                if (bursts_buf[0] && !workload_parse_bursts(&spec, bursts_buf)) {
                    fprintf(stderr, "%s:%d: bad or too many bursts in '%s' (skipped)\n", filepath, lineno, bursts_buf);
                }
                if (pages_buf[0] && !workload_parse_pages(&spec, pages_buf, sim->seed, sim->num_processes)) {
                    fprintf(stderr, "%s:%d: page refs must be 0..%d, at most %d (bad ones skipped)\n",
                            filepath, lineno, PRISM_MAX_VIRTUAL_PAGES - 1, PRISM_MAX_PAGE_REFS);
                }
                if (workload_install_spec(sim, &spec) < 0) {
                    fprintf(stderr, "%s:%d: process table full (max %d)\n", filepath, lineno, PRISM_MAX_PROCESSES);
                }
            }
        }
        /* Unknown keywords (e.g. "layout" lines read by scenario_load) are ignored. */
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
