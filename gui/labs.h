#ifndef PRISM_GUI_LABS_H
#define PRISM_GUI_LABS_H

#include "gui_state.h"
#include "prism/disk.h"
#include "prism/refstring.h"

#define LAB_SETUP_W   300
#define LAB_ID(lab, n) (3000 + (lab) * 200 + (n))

typedef enum {
    LAB_REFSTRING = 0,
    LAB_DISK,
    LAB_COUNT
} lab_kind_t;

typedef struct {
    char            input[1200];
    int             frames;
    bool            enabled[REPL_COUNT];
    int             page_size;
    uint16_t        refs[REFSTR_MAX_REFS];
    int             count;
    refstr_result_t results[REPL_COUNT];
    bool            dirty;
    char            message[200];
    bool            message_bad;
    int             cursor;
    float           scroll_x;
    float           scroll_y;
    int             hover_policy;
} refstring_lab_t;

typedef struct {
    char          input[400];
    int           cylinders;
    int           head;
    int           direction;
    bool          enabled[DISK_COUNT];
    disk_input_t  queue;
    disk_result_t results[DISK_COUNT];
    bool          valid;
    bool          dirty;
    char          message[160];
    int           cursor;
    int           hover_policy;
} disk_lab_t;

typedef struct labs {
    int             active;
    refstring_lab_t refstring;
    disk_lab_t      disk;
} labs_t;

void lab_refstring_init(refstring_lab_t *lab);
void lab_refstring_draw(gui_t *g, refstring_lab_t *lab, Rectangle r);
bool lab_refstring_load_trace(refstring_lab_t *lab, const char *path);

void lab_disk_init(disk_lab_t *lab);
void lab_disk_draw(gui_t *g, disk_lab_t *lab, Rectangle r);

void lab_draw_section(float x, float *y, const char *title);
void lab_draw_hint(float x, float *y, float w, const char *text);
bool lab_policy_toggle(int id, float x, float *y, float w, int policy, const char *name, bool *enabled, int *hover);
void lab_step_controls(int first_id, float x, float y, int *cursor, int total, const char *unit);
void lab_step_keys(int *cursor, int total);
void lab_card(Rectangle panel, float x, float y, float w, const algo_info_t *info, const char *fallback);
void lab_legend_swatch(float x, float y, int policy);

#endif
