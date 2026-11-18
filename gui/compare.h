#ifndef PRISM_GUI_COMPARE_H
#define PRISM_GUI_COMPARE_H

#include "gui_state.h"
#include "prism/metrics.h"

#define CMP_MAX_CASES   17
#define CMP_MAX_ALGS    SCHED_COUNT
#define CMP_LANE_CAP    4096
#define CMP_MAX_TICKS   50000
#define CMP_SWEEP_MAX   32
#define CMP_TBL_COLS    256
#define CMP_TBL_FRAMES  16

#define CMP_WIDGET_ID(n) (2000 + (n))

typedef enum { CMP_SCHED = 0, CMP_VM } cmp_mode_t;

typedef struct {
    bool          valid;
    int           status;
    sim_metrics_t metrics;
    int           ncores;
    uint32_t      len;
    int8_t       *lanes;
} cmp_run_t;

typedef struct {
    char  label[48];
    char  path[256];
    bool  current;
    bool  enabled;
    bool  loaded;
    char  description[640];
    bool  has_pages;
    int   num_procs, cores, frames, quantum;
    char  names[PRISM_MAX_PROCESSES][PRISM_MAX_NAME_LEN];
} cmp_case_t;

typedef struct {
    int16_t page;
    int8_t  pid;
    bool    fault;
    int8_t  loaded;
    int8_t  hit_frame;
    int16_t frame_page[CMP_TBL_FRAMES];
    int8_t  frame_pid[CMP_TBL_FRAMES];
} cmp_col_t;

typedef struct {
    const char *name;
    const char *tooltip;
    bool        lower_better;
    const char *format;
    double    (*value)(const sim_metrics_t *m);
} cmp_metric_t;

typedef struct compare {
    int        mode;
    bool       sched_on[SCHED_COUNT];
    bool       repl_on[REPL_COUNT];
    int        quantum;
    int        frames;
    cmp_case_t cases[CMP_MAX_CASES];
    int        num_cases;
    int        focused_case;
    int        chart_metric[2];
    cmp_run_t  runs[CMP_MAX_CASES][CMP_MAX_ALGS];
    bool       dirty;
    double     run_ms;

    uint32_t   sweep[REPL_COUNT][CMP_SWEEP_MAX + 1];
    int        sweep_min, sweep_max, sweep_current;
    bool       sweep_ready;
    int        table_alg, table_frames, num_columns, table_scroll;
    bool       table_ready, table_truncated;
    cmp_col_t  columns[CMP_TBL_COLS];

    int        hover_alg;
    sim_t     *work, *base;
} compare_t;

int                 cmp_num_algs(const compare_t *c);
bool                cmp_alg_on(const compare_t *c, int a);
const char         *cmp_alg_name(const compare_t *c, int a);
const algo_info_t  *cmp_alg_info(const compare_t *c, int a);
Color               cmp_alg_color(int a);
bool                cmp_case_usable(const compare_t *c, int i);
const cmp_metric_t *cmp_metrics(int mode, int *n);
void                cmp_run_all(gui_t *g);
void                cmp_open_in_sim(gui_t *g, int case_idx, int alg);
const char         *cmp_case_name(int pid, void *user);
bool                cmp_run_ok(const compare_t *c, int case_idx, int alg);

void cmp_draw_gantt_stack(gui_t *g, Rectangle r, int case_idx);
void cmp_draw_sweep_chart(gui_t *g, Rectangle r);
void cmp_draw_frame_table(gui_t *g, Rectangle r);

#endif
