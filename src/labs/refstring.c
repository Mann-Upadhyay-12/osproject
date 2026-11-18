#include "prism/refstring.h"
#include "prism/sim_api.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *refstr_strerror(refstr_status_t status) {
    switch (status) {
        case REFSTR_OK:             return "ok";
        case REFSTR_ERR_EMPTY:      return "the reference string is empty";
        case REFSTR_ERR_TOO_MANY:   return "too many references (max 256)";
        case REFSTR_ERR_PAGE_RANGE: return "page numbers must be between 0 and 63";
        case REFSTR_ERR_SYNTAX:     return "use page numbers separated by spaces or commas";
        case REFSTR_ERR_FRAMES:     return "frames must be between 1 and 16";
        case REFSTR_ERR_IO:         return "cannot read the trace file";
        case REFSTR_ERR_MEMORY:     return "out of memory";
    }
    return "unknown error";
}

static bool is_separator(char c) {
    return isspace((unsigned char)c) || c == ',' || c == ';';
}

refstr_status_t refstr_parse(const char *text, uint16_t *refs, int max_refs, int *count) {
    *count = 0;
    const char *p = text;

    while (*p) {
        while (*p && is_separator(*p)) p++;
        if (!*p) break;
        if (!isdigit((unsigned char)*p)) return REFSTR_ERR_SYNTAX;

        char *end;
        long page = strtol(p, &end, 10);
        if (*end && !is_separator(*end)) return REFSTR_ERR_SYNTAX;
        if (page < 0 || page > REFSTR_MAX_PAGE) return REFSTR_ERR_PAGE_RANGE;
        if (*count >= max_refs) return REFSTR_ERR_TOO_MANY;

        refs[(*count)++] = (uint16_t)page;
        p = end;
    }
    return *count > 0 ? REFSTR_OK : REFSTR_ERR_EMPTY;
}

typedef struct {
    const sim_t     *sim;
    refstr_result_t *out;
    bool             pending_fault;
    int16_t          pending_victim;
} recorder_t;

static void snapshot_frames(const sim_t *s, refstr_step_t *step) {
    for (int f = 0; f < REFSTR_MAX_FRAMES; f++) {
        bool occupied = f < s->total_frames && s->frames[f].occupied;
        step->frames[f] = occupied ? (int16_t)s->frames[f].vpage : REFSTR_EMPTY;
    }
}

static void record_event(const sim_event_t *ev, void *user) {
    recorder_t *rec = (recorder_t *)user;
    refstr_result_t *out = rec->out;

    switch (ev->type) {
        case EVENT_PAGE_FAULT:
            rec->pending_fault = true;
            break;
        case EVENT_PAGE_EVICT:
            rec->pending_victim = (int16_t)ev->detail1;
            break;
        case EVENT_PAGE_ACCESS: {
            if (out->num_steps >= REFSTR_MAX_REFS) return;
            refstr_step_t *step = &out->steps[out->num_steps++];
            step->page = (int16_t)ev->detail1;
            step->frame = (int8_t)ev->detail2;
            step->fault = rec->pending_fault;
            step->victim = rec->pending_fault ? rec->pending_victim : REFSTR_EMPTY;
            snapshot_frames(rec->sim, step);
            if (step->fault) out->faults++;

            rec->pending_fault = false;
            rec->pending_victim = REFSTR_EMPTY;
            break;
        }
        default:
            break;
    }
}

refstr_status_t refstr_simulate(repl_policy_t policy, int num_frames, const uint16_t *refs, int count,
                                refstr_result_t *out) {
    memset(out, 0, sizeof(*out));
    out->policy = policy;
    out->num_frames = num_frames;
    if (count <= 0) return REFSTR_ERR_EMPTY;
    if (count > REFSTR_MAX_REFS) return REFSTR_ERR_TOO_MANY;
    if (num_frames < 1 || num_frames > REFSTR_MAX_FRAMES) return REFSTR_ERR_FRAMES;
    for (int i = 0; i < count; i++) {
        if (refs[i] > REFSTR_MAX_PAGE) return REFSTR_ERR_PAGE_RANGE;
    }

    sim_t *sim = malloc(sizeof(sim_t));
    if (!sim) return REFSTR_ERR_MEMORY;

    sim_init(sim, 1, num_frames, 1, SCHED_FCFS, policy, 42);
    sim_pid_t pid = sim_add_process(sim, "refs", 0);
    sim_add_burst(sim, pid, BURST_CPU, (uint16_t)count);
    for (int i = 0; i < count; i++) sim_add_page_ref(sim, pid, refs[i]);

    recorder_t rec = {sim, out, false, REFSTR_EMPTY};
    sim_set_observer(sim, record_event, &rec);
    sim_run(sim);

    free(sim);
    return REFSTR_OK;
}

typedef struct {
    uint64_t pages[PRISM_MAX_VIRTUAL_PAGES];
    int      count;
} page_numbering_t;

static int number_for_page(page_numbering_t *numbering, uint64_t page) {
    for (int i = 0; i < numbering->count; i++) {
        if (numbering->pages[i] == page) return i;
    }
    if (numbering->count >= PRISM_MAX_VIRTUAL_PAGES) return -1;
    numbering->pages[numbering->count] = page;
    return numbering->count++;
}

static bool address_from_line(const char *line, uint64_t *address) {
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '\0' || *line == '\n' || *line == '#' || *line == '=') return false;

    bool lackey_record = (line[0] == 'I' || line[0] == 'L' || line[0] == 'S' || line[0] == 'M') &&
                         (line[1] == ' ' || line[1] == '\t');
    if (lackey_record) {
        if (line[0] == 'I') return false;
        line++;
        while (*line == ' ' || *line == '\t') line++;
        char *end;
        *address = strtoull(line, &end, 16);
        return end != line;
    }

    char *end;
    *address = strtoull(line, &end, 0);
    return end != line;
}

static void add_trace_line(const char *line, uint32_t page_size, page_numbering_t *numbering,
                                      uint16_t *refs, int max_refs, trace_info_t *info) {
    uint64_t address;
    if (!address_from_line(line, &address)) return;
    if (info->count >= max_refs) {
        info->truncated = true;
        return;
    }

    int page = number_for_page(numbering, address / page_size);
    if (page < 0) {
        info->truncated = true;
        return;
    }
    refs[info->count++] = (uint16_t)page;
}

static void finish_trace(const page_numbering_t *numbering, trace_info_t *info) {
    info->distinct_pages = numbering->count;
}

refstr_status_t refstr_parse_trace_text(const char *text, uint32_t page_size, uint16_t *refs, int max_refs,
                                        trace_info_t *info) {
    memset(info, 0, sizeof(*info));
    if (page_size == 0) page_size = 4096;

    page_numbering_t numbering = {{0}, 0};
    char line[256];
    for (const char *p = text; *p && !info->truncated;) {
        size_t length = strcspn(p, "\n");
        snprintf(line, sizeof(line), "%.*s", (int)(length < sizeof(line) ? length : sizeof(line) - 1), p);
        info->lines_read++;
        add_trace_line(line, page_size, &numbering, refs, max_refs, info);
        p += length + (p[length] ? 1 : 0);
    }
    finish_trace(&numbering, info);
    return info->count > 0 ? REFSTR_OK : REFSTR_ERR_EMPTY;
}

refstr_status_t refstr_load_trace(const char *path, uint32_t page_size, uint16_t *refs, int max_refs,
                                  trace_info_t *info) {
    memset(info, 0, sizeof(*info));
    if (page_size == 0) page_size = 4096;

    FILE *f = fopen(path, "r");
    if (!f) return REFSTR_ERR_IO;

    page_numbering_t numbering = {{0}, 0};
    char line[256];
    while (!info->truncated && fgets(line, sizeof(line), f)) {
        info->lines_read++;
        add_trace_line(line, page_size, &numbering, refs, max_refs, info);
    }
    fclose(f);

    finish_trace(&numbering, info);
    return info->count > 0 ? REFSTR_OK : REFSTR_ERR_EMPTY;
}
