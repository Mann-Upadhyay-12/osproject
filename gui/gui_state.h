#ifndef PRISM_GUI_STATE_H
#define PRISM_GUI_STATE_H

/*
 * prism-gui: "Packet Tracer for processes".
 *
 * The GUI owns one sim_t and edits it only through sim_api.h / scenario.h.
 * The canvas shows system nodes (Ready Queue, Scheduler, CPU, Memory,
 * Disk/I-O) and one node per process; layout positions live in a layout_t so
 * they save with the scenario. Trace events reach the GUI through the sim
 * observer and feed the event list, the narration ("Explain") and the token
 * animations; all of it is a pure view and never feeds back.
 *
 * Two views share the window: Simulate (the canvas) and Compare (runs the
 * same test cases under several algorithms on private sim_t copies, see
 * compare.c). Neither view touches the other's simulation.
 */

#include "raylib.h"
#include "prism/sim_api.h"
#include "prism/scenario.h"
#include "widgets.h"

typedef enum {
    NODE_READY = 0,
    NODE_SCHED,
    NODE_CPU,
    NODE_MEMORY,
    NODE_DISK,
    NODE_SYS_COUNT,
    NODE_PROC = 100,
    NODE_NONE = -1
} node_kind_t;

typedef struct {
    int kind;   /* node_kind_t */
    int pid;    /* NODE_PROC only */
} node_ref_t;

extern const char *const g_sys_keys[NODE_SYS_COUNT];   /* layout keys */
extern const char *const g_sys_titles[NODE_SYS_COUNT];

#define GUI_EV_CAP      16384
#define GUI_NARR_CAP    1024
#define GUI_HIST_TICKS  4096
#define GUI_MAX_TOKENS  192
#define GUI_MAX_PATH    5

typedef struct {
    int     pid;
    Vector2 pts[GUI_MAX_PATH];
    int     npts;
    double  t0, dur;
    Color   color;
    bool    fault;     /* draw as a page-fault token */
} token_t;

typedef struct {
    bool        open;
    bool        inject;         /* run mode: spawn now instead of editing */
    int         edit_pid;       /* -1 = new process */
    char        name[PRISM_MAX_NAME_LEN];
    int         arrival, priority;
    char        pages[1200];
    int         nb;
    int         kind[PRISM_MAX_BURSTS];      /* burst_kind_t; only CPU/IO are editable */
    int         dur[PRISM_MAX_BURSTS];
    int         target[PRISM_MAX_BURSTS];    /* kept for resource/IPC bursts */
    int         scroll;
    char        error[160];
    uint64_t    rng;
} builder_t;

typedef enum { MODE_EDIT = 0, MODE_RUN } gui_mode_t;
typedef enum { VIEW_SIM = 0, VIEW_COMPARE } gui_view_t;

/* Bottom panes: left (narration / raw events) and middle (charts / table). */
typedef enum { LTAB_EXPLAIN = 0, LTAB_EVENTS, LTAB_COUNT } left_tab_t;
typedef enum { MTAB_GANTT = 0, MTAB_STATES, MTAB_TABLE, MTAB_COUNT } mid_tab_t;

/* One plain-language sentence about a trace event, written when it happens
 * (the observer sees the state at that moment, e.g. who else was waiting). */
typedef struct {
    tick_t tick;
    int    pid;
    int    type;        /* event_type_t */
    char   text[220];
} narr_t;

struct compare;   /* compare.c */

typedef struct gui {
    sim_t      *sim;
    layout_t    layout;
    gui_mode_t  mode;
    bool        playing;
    float       speed;          /* ticks per second */
    double      tick_acc;

    /* canvas */
    Camera2D    cam;
    Rectangle   canvas;         /* screen rect */
    node_ref_t  sel, hover, drag;
    Vector2     drag_off;
    bool        snap;
    bool        panning;
    node_ref_t  ctx_node;       /* right-click menu target */
    Vector2     ctx_pos;
    bool        ctx_open;
    int         palette_drag;   /* node kind being dragged from palette, -1 none */

    /* trace view (filled by the observer) */
    sim_event_t ev[GUI_EV_CAP];
    uint32_t    ev_start, ev_len;
    float       ev_scroll;      /* rows from the bottom */
    bool        hide_hits;
    int         ev_filter_pid;  /* -1 = all */

    /* per-tick history for the timelines: process states and who ran on each core */
    uint8_t     hist[GUI_HIST_TICKS][PRISM_MAX_PROCESSES];
    int8_t      hist_core[GUI_HIST_TICKS][PRISM_MAX_CORES];
    uint32_t    hist_len;

    /* narration ring (Explain tab) */
    narr_t      narr[GUI_NARR_CAP];
    uint32_t    narr_start, narr_len;
    float       narr_scroll;
    bool        replaying;      /* gui_seek is re-running: no animation */

    gui_view_t  view;
    int         ltab, mtab;
    struct compare *cmp;

    /* animation */
    token_t     tok[GUI_MAX_TOKENS];
    int         ntok;
    double      frame_flash[PRISM_MAX_FRAMES];
    double      frame_load[PRISM_MAX_FRAMES];
    double      node_flash[NODE_SYS_COUNT];
    double      anim_now;
    float       anim_dur;

    builder_t   b;

    char        path[256];
    char        status[200];
    double      status_t;
    bool        status_bad;
    int         preset_sel;
    char        presets[16][128];
    const char *preset_items[17];
    int         npresets;
} gui_t;

/* main_gui.c */
void gui_status(gui_t *g, bool bad, const char *fmt, ...);
void gui_reset_view_state(gui_t *g);
void gui_enter_edit(gui_t *g);
void gui_step_tick(gui_t *g);
void gui_step_event(gui_t *g);
void gui_seek(gui_t *g, tick_t target);   /* replay from 0 (or step forward) to target */
void gui_step_back(gui_t *g);
bool gui_start_running(gui_t *g);          /* validate and enter run mode */
bool gui_validate(const gui_t *g, char *msg, size_t n);
void gui_load(gui_t *g, const char *path);
void gui_save(gui_t *g, const char *path);
void gui_new_scenario(gui_t *g);
bool gui_has_node(const gui_t *g, int kind);
void gui_add_sys_node(gui_t *g, int kind, Vector2 world);
void gui_delete_node(gui_t *g, node_ref_t n);
void gui_duplicate_process(gui_t *g, int pid);
void gui_random_processes(gui_t *g, int n);
void gui_fit_view(gui_t *g);

/* canvas.c */
Rectangle canvas_node_rect(const gui_t *g, node_ref_t n);
Vector2   canvas_node_center(const gui_t *g, node_ref_t n);
void      canvas_place_process(gui_t *g, int pid);
void      canvas_default_layout(gui_t *g);
void      canvas_update(gui_t *g);
void      canvas_draw(gui_t *g);
Color     proc_color(int pid);
Color     state_color(proc_state_t st);

/* anim.c */
void anim_observer(const sim_event_t *ev, void *user);
void anim_update(gui_t *g);
void anim_draw(const gui_t *g);
void anim_clear(gui_t *g);

/* panels.c */
void panels_init(void);   /* call once at startup */
void panel_toolbar(gui_t *g, Rectangle r);
void panel_palette(gui_t *g, Rectangle r);
void panel_inspector(gui_t *g, Rectangle r);
void panel_events(gui_t *g, Rectangle r);
void panel_bottom_left(gui_t *g, Rectangle r);
void panel_bottom_mid(gui_t *g, Rectangle r);
void panel_metrics(gui_t *g, Rectangle r);
Color event_color(event_type_t t);
void panel_context_menu(gui_t *g);
typedef struct { float x, y, w; } cursor_t;          /* simple vertical layout */
void panel_kv(cursor_t *c, const char *key, const char *fmt, ...);
void panel_heading(cursor_t *c, const char *title);
extern const char *g_sched_items[SCHED_COUNT];      /* filled by panels_init */
extern const char *g_repl_items[REPL_COUNT];

/* explain.c: narration and algorithm "cards" */
typedef struct {
    const char *name;
    const char *summary;      /* one line */
    const char *how;          /* the rule, in a few sentences */
    const char *good;
    const char *bad;
    const char *preemptive;   /* "yes" / "no" / detail */
} algo_info_t;
const algo_info_t *explain_sched_info(sched_policy_t p);
const algo_info_t *explain_repl_info(repl_policy_t p);
float explain_draw_card(const algo_info_t *a, float x, float y, float w);   /* returns height */
void  explain_event(gui_t *g, const sim_event_t *ev);    /* observer hook */
void  explain_clear(gui_t *g);
void  panel_explain(gui_t *g, Rectangle body);
void  explain_draw_latest(gui_t *g, Rectangle canvas);   /* caption over the canvas */

/* gantt.c: timelines and the per-process results table */
typedef struct {
    const int8_t *cells;   /* cells[(t % ring) * stride + core] = pid or -1 */
    uint32_t      ring;    /* ring capacity in ticks (or the length, if not a ring) */
    int           stride;
    int           ncores;
    uint32_t      len;     /* ticks recorded */
} lanes_t;
typedef const char *(*pid_name_fn)(int pid, void *user);
/* Textbook Gantt chart: one lane per core, merged blocks with process names,
 * a tick axis. Shows ticks [first, first+span). Returns the hovered tick or -1. */
int  gantt_draw(Rectangle r, const lanes_t *L, uint32_t first, uint32_t span, pid_name_fn name, void *user,
                bool axis, int64_t cursor);
void panel_cpu_gantt(gui_t *g, Rectangle body);
void panel_states(gui_t *g, Rectangle body);
void panel_proc_table(gui_t *g, Rectangle body);

/* compare.c / compare_view.c */
void compare_init(gui_t *g);
void compare_free(gui_t *g);
void compare_mark_dirty(gui_t *g);
void compare_view(gui_t *g, Rectangle r);
void compare_setup_args(gui_t *g, const char *mode);   /* --compare sched|vm */

/* builder.c */
void builder_open_new(gui_t *g, bool inject);
void builder_open_edit(gui_t *g, int pid);
void builder_template(gui_t *g, int which);
void builder_draw(gui_t *g);

#endif
