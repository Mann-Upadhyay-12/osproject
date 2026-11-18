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

static bool spec_add_control(proc_spec_t *spec, burst_kind_t kind, int target, int16_t data) {
    if (proc_spec_add_burst(spec, kind, 1) != SIM_OK) return false;
    burst_t *b = &spec->bursts[spec->num_bursts - 1];
    b->target_id = (int16_t)target;
    b->extra_data = data;
    return true;
}

static bool parse_one_burst(proc_spec_t *spec, const char *token) {
    char code = (char)toupper((unsigned char)token[0]);
    int arg = atoi(token + 1);

    switch (code) {
        case 'C':
        case 'I':
            if (arg <= 0 || arg > UINT16_MAX) return false;
            return proc_spec_add_burst(spec, code == 'C' ? BURST_CPU : BURST_IO, (uint16_t)arg) == SIM_OK;
        case 'A': return spec_add_control(spec, BURST_ACQUIRE, arg, 0);
        case 'R': return spec_add_control(spec, BURST_RELEASE, arg, 0);
        case 'S': return spec_add_control(spec, BURST_IPC_SEND, arg, 100);
        case 'W': return spec_add_control(spec, BURST_IPC_RECV, arg, 0);
        case 'K': return spec_add_control(spec, BURST_CHECKPOINT, 0, 0);
        default:  return false;
    }
}

bool workload_parse_bursts(proc_spec_t *spec, const char *burst_str) {
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", burst_str);

    bool all_ok = true;
    for (char *token = strtok(buf, ","); token; token = strtok(NULL, ",")) {
        token = trim_whitespace(token);
        if (*token == '\0') continue;
        if (!parse_one_burst(spec, token)) all_ok = false;
    }
    return all_ok;
}

static bool spec_add_page(proc_spec_t *spec, int page) {
    if (page < 0 || page >= PRISM_MAX_VIRTUAL_PAGES) return false;
    return proc_spec_add_ref(spec, (uint16_t)page) == SIM_OK;
}

static bool add_page_range(proc_spec_t *spec, int first, int last) {
    bool all_ok = true;
    for (int page = first; page <= last; page++) {
        if (!spec_add_page(spec, page)) all_ok = false;
    }
    return all_ok;
}

static bool parse_loop_pattern(proc_spec_t *spec, const char *args) {
    int first = 0, last = 3, loops = 1;
    sscanf(args, "%d:%d:%d", &first, &last, &loops);

    bool all_ok = true;
    for (int i = 0; i < loops; i++) {
        if (!add_page_range(spec, first, last)) all_ok = false;
    }
    return all_ok;
}

static bool parse_seq_pattern(proc_spec_t *spec, const char *args) {
    int first = 0, last = 5;
    sscanf(args, "%d:%d", &first, &last);
    return add_page_range(spec, first, last);
}

static bool parse_random_pattern(proc_spec_t *spec, const char *args, uint64_t seed, int pid) {
    int lowest = 0, highest = 7, count = 10;
    sscanf(args, "%d:%d:%d", &lowest, &highest, &count);
    if (lowest < 0 || highest < lowest) return false;

    uint64_t prng = seed + (uint64_t)pid * 0x9e3779b97f4a7c15ULL;
    bool all_ok = true;
    for (int i = 0; i < count; i++) {
        uint32_t page = prng_range(&prng, (uint32_t)lowest, (uint32_t)highest);
        if (!spec_add_page(spec, (int)page)) all_ok = false;
    }
    return all_ok;
}

static bool parse_page_list(proc_spec_t *spec, char *list) {
    bool all_ok = true;
    for (char *token = strtok(list, ","); token; token = strtok(NULL, ",")) {
        token = trim_whitespace(token);
        if (*token && !spec_add_page(spec, atoi(token))) all_ok = false;
    }
    return all_ok;
}

bool workload_parse_pages(proc_spec_t *spec, const char *page_src, uint64_t seed, int pid) {
    char buf[2048];
    snprintf(buf, sizeof(buf), "%s", page_src);
    char *pattern = trim_whitespace(buf);

    if (strncmp(pattern, "loop:", 5) == 0)   return parse_loop_pattern(spec, pattern + 5);
    if (strncmp(pattern, "seq:", 4) == 0)    return parse_seq_pattern(spec, pattern + 4);
    if (strncmp(pattern, "random:", 7) == 0) return parse_random_pattern(spec, pattern + 7, seed, pid);
    return parse_page_list(spec, pattern);
}

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

typedef struct {
    const char        *path;
    int                lineno;
    sim_t             *sim;
    workload_config_t *cfg;
} parse_ctx_t;

static bool starts_with(const char *line, const char *keyword) {
    return strncmp(line, keyword, strlen(keyword)) == 0;
}

static void parse_cores(parse_ctx_t *ctx, const char *value) {
    int cores = atoi(value);
    if (cores < 1 || cores > PRISM_MAX_CORES) {
        fprintf(stderr, "%s:%d: cores must be 1..%d\n", ctx->path, ctx->lineno, PRISM_MAX_CORES);
        return;
    }
    if (ctx->cfg) { ctx->cfg->cores = cores; ctx->cfg->has_cores = true; }
    if (ctx->sim) ctx->sim->num_cores = cores;
}

static void parse_frames(parse_ctx_t *ctx, const char *value) {
    int frames = atoi(value);
    if (frames < 1 || frames > PRISM_MAX_FRAMES) {
        fprintf(stderr, "%s:%d: frames must be 1..%d\n", ctx->path, ctx->lineno, PRISM_MAX_FRAMES);
        return;
    }
    if (ctx->cfg) { ctx->cfg->frames = frames; ctx->cfg->has_frames = true; }
    if (ctx->sim) { ctx->sim->total_frames = frames; ctx->sim->configured_frames = frames; }
}

static void parse_disk_latency(parse_ctx_t *ctx, const char *value) {
    int latency = atoi(value);
    if (ctx->cfg) { ctx->cfg->disk_latency = latency; ctx->cfg->has_disk_latency = true; }
    if (ctx->sim) ctx->sim->disk_latency = latency;
}

static void parse_resource(parse_ctx_t *ctx, const char *value) {
    char name[32];
    int instances = 1;
    if (sscanf(value, "%31s %d", name, &instances) >= 1 && ctx->sim) {
        sim_add_resource(ctx->sim, name, instances);
    }
}

static void parse_sched(parse_ctx_t *ctx, const char *value) {
    char name[32] = "";
    sscanf(value, "%31s", name);

    sched_policy_t policy = SCHED_FCFS;
    if (!sched_lookup(name, &policy)) {
        fprintf(stderr, "%s:%d: unknown scheduler '%s', using fcfs\n", ctx->path, ctx->lineno, name);
    }
    if (ctx->cfg) { ctx->cfg->sched = policy; ctx->cfg->has_sched = true; }
    if (ctx->sim) ctx->sim->sched_policy = policy;
}

static void parse_repl(parse_ctx_t *ctx, const char *value) {
    char name[32] = "";
    sscanf(value, "%31s", name);

    repl_policy_t policy = REPL_FIFO;
    if (!repl_lookup(name, &policy)) {
        fprintf(stderr, "%s:%d: unknown replacement policy '%s', using fifo\n", ctx->path, ctx->lineno, name);
    }
    if (ctx->cfg) { ctx->cfg->repl = policy; ctx->cfg->has_repl = true; }
    if (ctx->sim) ctx->sim->repl_policy = policy;
}

static void parse_quantum(parse_ctx_t *ctx, const char *value) {
    uint16_t quantum = (uint16_t)atoi(value);
    if (ctx->cfg) { ctx->cfg->quantum = quantum; ctx->cfg->has_quantum = true; }
    if (ctx->sim) ctx->sim->default_quantum = quantum;
}

static void parse_seed(parse_ctx_t *ctx, const char *value) {
    uint64_t seed = strtoull(value, NULL, 10);
    if (ctx->cfg) { ctx->cfg->seed = seed; ctx->cfg->has_seed = true; }
    if (ctx->sim) { ctx->sim->seed = seed; ctx->sim->prng_state = seed; }
}

static void parse_proc(parse_ctx_t *ctx, char *fields) {
    proc_spec_t spec;
    char bursts[1024] = "";
    char pages[2048] = "";

    char *token = strtok(fields, " \t");
    proc_spec_init(&spec, token);

    while ((token = strtok(NULL, " \t")) != NULL) {
        if (starts_with(token, "arrival=")) {
            spec.arrival = (tick_t)strtoull(token + 8, NULL, 10);
        } else if (starts_with(token, "priority=")) {
            spec.priority = (uint16_t)atoi(token + 9);
        } else if (starts_with(token, "bursts=")) {
            snprintf(bursts, sizeof(bursts), "%s", token + 7);
        } else if (starts_with(token, "pages=")) {
            snprintf(pages, sizeof(pages), "%s", token + 6);
        }
    }

    sim_t *sim = ctx->sim;
    if (!sim) return;

    if (bursts[0] && !workload_parse_bursts(&spec, bursts)) {
        fprintf(stderr, "%s:%d: bad or too many bursts in '%s' (skipped)\n", ctx->path, ctx->lineno, bursts);
    }
    if (pages[0] && !workload_parse_pages(&spec, pages, sim->seed, sim->num_processes)) {
        fprintf(stderr, "%s:%d: page refs must be 0..%d, at most %d (bad ones skipped)\n",
                ctx->path, ctx->lineno, PRISM_MAX_VIRTUAL_PAGES - 1, PRISM_MAX_PAGE_REFS);
    }
    if (workload_install_spec(sim, &spec) < 0) {
        fprintf(stderr, "%s:%d: process table full (max %d)\n", ctx->path, ctx->lineno, PRISM_MAX_PROCESSES);
    }
}

static void parse_line(parse_ctx_t *ctx, char *line) {
    if      (starts_with(line, "cores"))        parse_cores(ctx, line + 5);
    else if (starts_with(line, "frames"))       parse_frames(ctx, line + 6);
    else if (starts_with(line, "disk_latency")) parse_disk_latency(ctx, line + 12);
    else if (starts_with(line, "resource"))     parse_resource(ctx, line + 8);
    else if (starts_with(line, "sched"))        parse_sched(ctx, line + 5);
    else if (starts_with(line, "repl"))         parse_repl(ctx, line + 4);
    else if (starts_with(line, "quantum"))      parse_quantum(ctx, line + 7);
    else if (starts_with(line, "seed"))         parse_seed(ctx, line + 4);
    else if (starts_with(line, "proc") && isspace((unsigned char)line[4])) parse_proc(ctx, line + 4);
}

static bool is_blank_or_comment(const char *line) {
    return *line == '\0' || *line == '#' || *line == ';';
}

bool workload_load_file(const char *filepath, sim_t *sim, workload_config_t *cfg) {
    FILE *fp = fopen(filepath, "r");
    if (!fp) return false;

    parse_ctx_t ctx = { .path = filepath, .lineno = 0, .sim = sim, .cfg = cfg };
    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), fp)) {
        ctx.lineno++;
        char *line = trim_whitespace(buffer);
        if (!is_blank_or_comment(line)) parse_line(&ctx, line);
    }

    fclose(fp);
    return true;
}

static void add_random_refs(sim_t *sim, sim_pid_t pid, uint64_t *prng, int count, uint32_t highest_page) {
    for (int i = 0; i < count; i++) {
        sim_add_page_ref(sim, pid, (uint16_t)prng_range(prng, 0, highest_page));
    }
}

static void make_cpu_bound(sim_t *sim, sim_pid_t pid, uint64_t *prng) {
    sim_add_burst(sim, pid, BURST_CPU, 20);
    add_random_refs(sim, pid, prng, 12, 4);
}

static void make_io_bound(sim_t *sim, sim_pid_t pid, uint64_t *prng) {
    sim_add_burst(sim, pid, BURST_CPU, 3);
    sim_add_burst(sim, pid, BURST_IO, 8);
    sim_add_burst(sim, pid, BURST_CPU, 3);
    add_random_refs(sim, pid, prng, 4, 2);
}

static void make_balanced(sim_t *sim, sim_pid_t pid, uint64_t *prng) {
    sim_add_burst(sim, pid, BURST_CPU, 6);
    sim_add_burst(sim, pid, BURST_IO, 4);
    sim_add_burst(sim, pid, BURST_CPU, 6);
    add_random_refs(sim, pid, prng, 8, 5);
}

bool workload_generate_mixed(sim_t *sim, int num_procs, uint64_t seed) {
    uint64_t prng = seed ? seed : 0x42ULL;

    for (int i = 0; i < num_procs; i++) {
        char name[PRISM_MAX_NAME_LEN];
        snprintf(name, sizeof(name), "P%d", i + 1);
        sim_pid_t pid = sim_add_process(sim, name, (tick_t)(i * 2));
        if (pid < 0) return false;

        switch (prng_range(&prng, 0, 2)) {
            case 0:  make_cpu_bound(sim, pid, &prng); break;
            case 1:  make_io_bound(sim, pid, &prng); break;
            default: make_balanced(sim, pid, &prng); break;
        }
    }
    return true;
}

bool workload_generate_belady(sim_t *sim) {
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
        for (int ref = 0; ref < 25; ref++) {
            sim_add_page_ref(sim, pid, (uint16_t)(ref % 3));
        }
    }
    return true;
}
