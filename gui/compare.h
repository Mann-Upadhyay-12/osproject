#ifndef PRISM_GUI_COMPARE_H
#define PRISM_GUI_COMPARE_H

/*
 * Compare view state (private to compare.c / compare_view.c).
 *
 * Every run is sim_prepare_variant(base) + run on a scratch sim_t, so the
 * test case, seed and config are identical and only the algorithm differs.
 * The Simulate view's sim is used read-only as the "current scenario" case.
 */

#include "gui_state.h"
#include "prism/metrics.h"

#define CMP_MAX_CASES   17          /* current scenario + 16 presets */
#define CMP_MAX_ALGS    SCHED_COUNT /* >= REPL_COUNT */
#define CMP_LANE_CAP    4096        /* Gantt ticks kept per run */
#define CMP_MAX_TICKS   50000       /* runs that do not finish by then are flagged */
#define CMP_SWEEP_MAX   32
#define CMP_TBL_COLS    256
#define CMP_TBL_FRAMES  16

typedef enum { CMP_SCHED = 0, CMP_VM } cmp_mode_t;

typedef struct {
    bool          valid;
    int           rc;               /* SIM_OK or the error from preparing the run */
    sim_metrics_t m;
    int           ncores;
    uint32_t      len;              /* ticks recorded in lanes */
    int8_t       *lanes;            /* [CMP_LANE_CAP * PRISM_MAX_CORES] */
} cmp_run_t;

typedef struct {
    char  label[48];
    char  path[256];
    bool  current;                  /* the Simulate view's scenario */
    bool  on;
    bool  loaded;                   /* metadata below is valid */
    char  desc[640];                /* leading '#' comment lines of the file */
    bool  has_pages;
    int   nproc, cores, frames, quantum;
    char  names[PRISM_MAX_PROCESSES][PRISM_MAX_NAME_LEN];
} cmp_case_t;

/* One reference in the textbook frame table. */
typedef struct {
    int16_t page;
    int8_t  pid;
    bool    fault;
    int8_t  loaded;                 /* frame the page was loaded into, or -1 on a hit */
    int8_t  hit_frame;
    int16_t fpage[CMP_TBL_FRAMES];  /* frame contents after the reference (-1 = free) */
    int8_t  fpid[CMP_TBL_FRAMES];
} cmp_col_t;

typedef struct {
    const char *name;
    const char *tip;
    bool        lower_better;
    const char *fmt;
    double    (*get)(const sim_metrics_t *m);
} cmp_metric_t;

typedef struct compare {
    int        mode;                         /* cmp_mode_t */
    bool       sched_on[SCHED_COUNT];
    bool       repl_on[REPL_COUNT];
    int        quantum;                      /* 0 = each test case's own */
    int        frames;                       /* 0 = each test case's own */
    cmp_case_t cases[CMP_MAX_CASES];
    int        ncases;
    int        focus;                        /* test case shown in detail */
    int        metric[2];                    /* bar chart metric per mode */
    cmp_run_t  runs[CMP_MAX_CASES][CMP_MAX_ALGS];
    bool       dirty;
    double     run_ms;

    /* Page replacement extras, for the focused case */
    uint32_t   sweep[REPL_COUNT][CMP_SWEEP_MAX + 1];
    int        sweep_lo, sweep_hi, sweep_cur;
    bool       sweep_ok;
    int        tbl_alg, tbl_frames, ncol, tbl_scroll;
    bool       tbl_ok, tbl_trunc;
    cmp_col_t  col[CMP_TBL_COLS];

    int        hover_alg;                    /* algorithm card shown in the setup panel */
    sim_t     *work, *base;                  /* scratch simulations */
} compare_t;

/* compare.c */
int                 cmp_nalgs(const compare_t *c);
bool                cmp_alg_on(const compare_t *c, int a);
const char         *cmp_alg_name(const compare_t *c, int a);
const algo_info_t  *cmp_alg_info(const compare_t *c, int a);
Color               cmp_alg_color(int a);
bool                cmp_case_usable(const compare_t *c, int i);
const cmp_metric_t *cmp_metrics(int mode, int *n);
void                cmp_run_all(gui_t *g);
void                cmp_open_in_sim(gui_t *g, int case_idx, int alg);
const char         *cmp_case_name(int pid, void *user);   /* pid_name_fn, user = cmp_case_t* */

#endif
