#ifndef PRISM_GUI_STATE_H
#define PRISM_GUI_STATE_H

/*
 * prism-gui: "Packet Tracer for processes".
 *
 * The GUI owns one sim_t and edits it only through sim_api.h / scenario.h.
 * The canvas shows system nodes (Ready Queue, Scheduler, CPU, Memory,
 * Disk/I-O) and one node per process; layout positions live in a layout_t so
 * they save with the scenario. Trace events reach the GUI through the sim
 * observer and feed the event list and the token animations; animation is a
 * pure view and never feeds back into the simulation.
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

    /* per-tick state history for the Gantt strip */
    uint8_t     hist[GUI_HIST_TICKS][PRISM_MAX_PROCESSES];
    uint32_t    hist_len;

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
bool gui_validate(const gui_t *g, char *msg, size_t n);
void gui_load(gui_t *g, const char *path);
void gui_save(gui_t *g, const char *path);
void gui_new_scenario(gui_t *g);
bool gui_has_node(const gui_t *g, int kind);
void gui_add_sys_node(gui_t *g, int kind, Vector2 world);
void gui_delete_node(gui_t *g, node_ref_t n);
void gui_duplicate_process(gui_t *g, int pid);
void gui_random_processes(gui_t *g, int n);

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
void panel_gantt(gui_t *g, Rectangle r);
void panel_metrics(gui_t *g, Rectangle r);
void panel_context_menu(gui_t *g);
typedef struct { float x, y, w; } cursor_t;          /* simple vertical layout */
void panel_kv(cursor_t *c, const char *key, const char *fmt, ...);
void panel_heading(cursor_t *c, const char *title);
extern const char *g_sched_items[SCHED_COUNT];      /* filled by panels_init */
extern const char *g_repl_items[REPL_COUNT];

/* builder.c */
void builder_open_new(gui_t *g, bool inject);
void builder_open_edit(gui_t *g, int pid);
void builder_template(gui_t *g, int which);
void builder_draw(gui_t *g);

#endif
