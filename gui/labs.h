#ifndef PRISM_GUI_LABS_H
#define PRISM_GUI_LABS_H

#include "gui_state.h"
#include "prism/refstring.h"

#define LAB_SETUP_W   300
#define LAB_ID(lab, n) (3000 + (lab) * 200 + (n))

typedef enum {
    LAB_REFSTRING = 0,
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

typedef struct labs {
    int             active;
    refstring_lab_t refstring;
} labs_t;

void lab_refstring_init(refstring_lab_t *lab);
void lab_refstring_draw(gui_t *g, refstring_lab_t *lab, Rectangle r);
bool lab_refstring_load_trace(refstring_lab_t *lab, const char *path);

void lab_draw_section(float x, float *y, const char *title);
void lab_draw_hint(float x, float *y, float w, const char *text);

#endif
