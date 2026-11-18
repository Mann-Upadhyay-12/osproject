#ifndef PRISM_REFSTRING_H
#define PRISM_REFSTRING_H

#include "prism/types.h"

#define REFSTR_MAX_REFS    PRISM_MAX_PAGE_REFS
#define REFSTR_MAX_FRAMES  16
#define REFSTR_MAX_PAGE    (PRISM_MAX_VIRTUAL_PAGES - 1)
#define REFSTR_EMPTY       (-1)

typedef struct {
    int16_t page;
    bool    fault;
    int8_t  frame;
    int16_t victim;
    int16_t frames[REFSTR_MAX_FRAMES];
} refstr_step_t;

typedef struct {
    repl_policy_t policy;
    int           num_frames;
    int           num_steps;
    int           faults;
    refstr_step_t steps[REFSTR_MAX_REFS];
} refstr_result_t;

typedef enum {
    REFSTR_OK = 0,
    REFSTR_ERR_EMPTY,
    REFSTR_ERR_TOO_MANY,
    REFSTR_ERR_PAGE_RANGE,
    REFSTR_ERR_SYNTAX,
    REFSTR_ERR_FRAMES,
    REFSTR_ERR_IO,
    REFSTR_ERR_MEMORY
} refstr_status_t;

const char *refstr_strerror(refstr_status_t status);

refstr_status_t refstr_parse(const char *text, uint16_t *refs, int max_refs, int *count);

refstr_status_t refstr_simulate(repl_policy_t policy, int num_frames, const uint16_t *refs, int count,
                                refstr_result_t *out);

typedef struct {
    int       count;
    int       distinct_pages;
    int       lines_read;
    bool      truncated;
} trace_info_t;

refstr_status_t refstr_load_trace(const char *path, uint32_t page_size, uint16_t *refs, int max_refs,
                                  trace_info_t *info);

refstr_status_t refstr_parse_trace_text(const char *text, uint32_t page_size, uint16_t *refs, int max_refs,
                                        trace_info_t *info);

#endif
