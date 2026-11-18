#ifndef PRISM_GUI_STATE_H
#define PRISM_GUI_STATE_H

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
    int kind;
    int pid;
} node_ref_t;

extern const char *const g_sys_keys[NODE_SYS_COUNT];
extern const char *const g_sys_titles[NODE_SYS_COUNT];

#define GUI_EV_CAP      16384
#define GUI_NARR_CAP    1024
#define GUI_HIST_TICKS  4096
#define GUI_MAX_TOKENS  192
#define GUI_MAX_PATH    5

typedef struct {
    int     pid;
    Vector2 points[GUI_MAX_PATH];
    int     num_points;
    double  start_time;
    double  duration;
    Color   color;
    bool    fault;
} token_t;

typedef struct {
    int kind;
    int duration;
    int target;
} builder_burst_t;

typedef struct {
    bool            open;
    bool            inject;
    int             edit_pid;
    char            name[PRISM_MAX_NAME_LEN];
    int             arrival;
    int             priority;
    int             period;
    int             deadline;
    int             jobs;
    bool            affinity[PRISM_MAX_CORES];
    int             parent;
    char            pages[1200];
    int             num_bursts;
    builder_burst_t bursts[PRISM_MAX_BURSTS];
    int             scroll;
    char            error[160];
    uint64_t        rng;
} builder_t;

typedef enum { MODE_EDIT = 0, MODE_RUN } gui_mode_t;
typedef enum { VIEW_SIM = 0, VIEW_COMPARE, VIEW_LABS, VIEW_COUNT } gui_view_t;

typedef enum { LTAB_EXPLAIN = 0, LTAB_EVENTS, LTAB_SYNC, LTAB_REALTIME, LTAB_TREE, LTAB_COUNT } left_tab_t;
typedef enum { MTAB_GANTT = 0, MTAB_STATES, MTAB_TABLE, MTAB_COUNT } mid_tab_t;

typedef struct {
    tick_t tick;
    int    pid;
    int    type;
    char   text[220];
} narration_t;

struct compare;
struct labs;

typedef struct gui {
    sim_t      *sim;
    layout_t    layout;
    gui_mode_t  mode;
    bool        playing;
    float       speed;
    double      tick_accumulator;

    Camera2D    camera;
    Rectangle   canvas;
    node_ref_t  selected, hovered, dragged;
    Vector2     drag_offset;
    bool        snap;
    bool        panning;
    node_ref_t  menu_node;
    Vector2     menu_pos;
    bool        menu_open;
    int         palette_drag;

    sim_event_t events[GUI_EV_CAP];
    uint32_t    events_start, events_len;
    float       events_scroll;
    bool        hide_page_hits;
    int         events_filter_pid;

    uint8_t     state_history[GUI_HIST_TICKS][PRISM_MAX_PROCESSES];
    int8_t      core_history[GUI_HIST_TICKS][PRISM_MAX_CORES];
    uint32_t    history_len;

    narration_t narration[GUI_NARR_CAP];
    uint32_t    narration_start, narration_len;
    float       narration_scroll;
    bool        replaying;

    gui_view_t  view;
    int         left_tab, mid_tab;
    struct compare *compare;
    struct labs    *labs;

    token_t     tokens[GUI_MAX_TOKENS];
    int         num_tokens;
    double      frame_flash[PRISM_MAX_FRAMES];
    double      frame_load[PRISM_MAX_FRAMES];
    double      node_flash[NODE_SYS_COUNT];
    double      anim_now;
    float       anim_duration;

    builder_t   builder;

    char        path[256];
    char        status[200];
    double      status_time;
    bool        status_bad;
    int         preset_selected;
    char        presets[16][128];
    const char *preset_items[17];
    int         num_presets;
} gui_t;

void gui_status(gui_t *g, bool bad, const char *fmt, ...);
void gui_reset_view_state(gui_t *g);
void gui_enter_edit(gui_t *g);
void gui_step_tick(gui_t *g);
void gui_step_event(gui_t *g);
void gui_seek(gui_t *g, tick_t target);
void gui_step_back(gui_t *g);
bool gui_start_running(gui_t *g);
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

Rectangle canvas_node_rect(const gui_t *g, node_ref_t n);
Vector2   canvas_node_center(const gui_t *g, node_ref_t n);
const char *canvas_node_key(const gui_t *g, node_ref_t n);
void      canvas_draw_node(const gui_t *g, node_ref_t n);
void      canvas_place_process(gui_t *g, int pid);
void      canvas_default_layout(gui_t *g);
void      canvas_update(gui_t *g);
void      canvas_draw(gui_t *g);
Color     proc_color(int pid);
Color     state_color(proc_state_t st);

void anim_observer(const sim_event_t *ev, void *user);
void anim_update(gui_t *g);
void anim_draw(const gui_t *g);
void anim_clear(gui_t *g);

void panels_init(void);
void panel_toolbar(gui_t *g, Rectangle r);
void panel_palette(gui_t *g, Rectangle r);
void panel_inspector(gui_t *g, Rectangle r);
void panel_events(gui_t *g, Rectangle r);
void panel_bottom_left(gui_t *g, Rectangle r);
void panel_bottom_mid(gui_t *g, Rectangle r);
void panel_metrics(gui_t *g, Rectangle r);
Color event_color(event_type_t t);
void panel_context_menu(gui_t *g);
typedef struct { float x, y, w; } cursor_t;
void panel_kv(cursor_t *c, const char *key, const char *fmt, ...);
void panel_heading(cursor_t *c, const char *title);
extern const char *g_sched_items[SCHED_COUNT];
extern const char *g_repl_items[REPL_COUNT];

typedef struct {
    const char *name;
    const char *summary;
    const char *how;
    const char *good;
    const char *bad;
    const char *preemptive;
} algo_info_t;
const algo_info_t *explain_sched_info(sched_policy_t p);
const algo_info_t *explain_repl_info(repl_policy_t p);
float explain_draw_card(const algo_info_t *a, float x, float y, float w);
void  explain_event(gui_t *g, const sim_event_t *ev);
void  explain_narrate(gui_t *g, const sim_event_t *ev, const char *fmt, ...);
const char *explain_process_name(const sim_t *s, int pid);
void  explain_sync_event(gui_t *g, const sim_event_t *ev);
void  sync_describe_stall(const sim_t *s, char *out, size_t size);
void  panel_sync(gui_t *g, Rectangle body);
void  panel_realtime(gui_t *g, Rectangle body);
void  panel_tree(gui_t *g, Rectangle body);
void  explain_tree_event(gui_t *g, const sim_event_t *ev);
int   gui_ready_queue_rows(const sim_t *s);
const char *gui_proc_state_label(const pcb_t *p);
const char *gui_ready_queue_label(const sim_t *s, int row);
void  explain_realtime_event(gui_t *g, const sim_event_t *ev);
bool  explain_realtime_preempt(gui_t *g, const sim_event_t *ev);
void  gui_report_end(gui_t *g);
void  explain_clear(gui_t *g);
void  panel_explain(gui_t *g, Rectangle body);
void  explain_draw_latest(gui_t *g, Rectangle canvas);

typedef struct {
    const int8_t *cells;
    uint32_t      ring;
    int           stride;
    int           ncores;
    uint32_t      len;
} lanes_t;
typedef const char *(*pid_name_fn)(int pid, void *user);
int  gantt_draw(Rectangle r, const lanes_t *L, uint32_t first, uint32_t span, pid_name_fn name, void *user,
                bool axis, int64_t cursor);
void panel_cpu_gantt(gui_t *g, Rectangle body);
void panel_states(gui_t *g, Rectangle body);
void panel_proc_table(gui_t *g, Rectangle body);

void compare_init(gui_t *g);
void compare_free(gui_t *g);
void compare_mark_dirty(gui_t *g);
void compare_view(gui_t *g, Rectangle r);
void compare_setup_args(gui_t *g, const char *mode);

void labs_init(gui_t *g);
void labs_free(gui_t *g);
void labs_view(gui_t *g, Rectangle r);
void labs_select(gui_t *g, const char *key);
void labs_drop_file(gui_t *g, const char *path);

void builder_open_new(gui_t *g, bool inject);
void builder_open_edit(gui_t *g, int pid);
void builder_template(gui_t *g, int which);
void builder_draw(gui_t *g);

#endif
