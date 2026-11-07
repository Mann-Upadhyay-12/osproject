#ifndef PRISM_GUI_WIDGETS_H
#define PRISM_GUI_WIDGETS_H

/*
 * Tiny immediate-mode widget set on top of raylib (no raygui dependency).
 *
 * Input routing:
 *  - Widgets take an id (any unique int per frame) for focus/popup state.
 *  - ui_push_layer(n) / ui_pop_layer(): while a modal (layer > 0) is open,
 *    only widgets drawn on the top layer react. ui_input_free() tells the
 *    canvas whether the mouse belongs to it this frame.
 *  - Dropdown lists are drawn at ui_end() so they sit on top of everything,
 *    and a click on an open list is consumed before other widgets see it.
 */

#include "raylib.h"
#include <stdbool.h>
#include <stddef.h>

/* Theme */
#define UI_BG        (Color){ 24,  27,  33, 255 }
#define UI_PANEL     (Color){ 33,  37,  45, 255 }
#define UI_PANEL_HI  (Color){ 44,  49,  60, 255 }
#define UI_BORDER    (Color){ 62,  68,  82, 255 }
#define UI_TEXT      (Color){220, 224, 232, 255 }
#define UI_TEXT_DIM  (Color){140, 147, 160, 255 }
#define UI_ACCENT    (Color){ 86, 156, 255, 255 }
#define UI_WARN      (Color){240, 170,  60, 255 }
#define UI_BAD       (Color){235,  85,  85, 255 }
#define UI_GOOD      (Color){ 90, 200, 120, 255 }

void  ui_init(void);           /* after InitWindow: loads a system TTF if one exists */
void  ui_shutdown(void);
void  ui_begin(void);          /* start of frame, before any widget */
void  ui_end(void);            /* end of frame, after all widgets (draws popups) */

void  ui_push_layer(int layer);
void  ui_pop_layer(void);
void  ui_set_modal(int layer); /* topmost interactive layer; 0 = none */
bool  ui_input_free(void);     /* mouse not over a widget / popup / modal */
void  ui_claim_rect(Rectangle r); /* mark a panel area as UI-owned this frame */
bool  ui_text_focused(void);   /* a text field has keyboard focus */

/* Text */
void  ui_text(float x, float y, float size, Color c, const char *fmt, ...);
float ui_text_width(const char *s, float size);
void  ui_text_clipped(Rectangle r, float size, Color c, const char *s); /* left, v-centred, truncated with ".." */
/* Nested clipping (raylib's scissor is not nestable). */
void  ui_scissor_push(Rectangle r);
void  ui_scissor_pop(void);

/* Widgets */
bool  ui_button(int id, Rectangle r, const char *label);
bool  ui_button_ex(int id, Rectangle r, const char *label, bool enabled, bool active);
bool  ui_text_field(int id, Rectangle r, char *buf, size_t cap);     /* true on commit (Enter/blur) */
bool  ui_int_field(int id, Rectangle r, int *value, int min, int max); /* [-] n [+], typing, wheel */
bool  ui_dropdown(int id, Rectangle r, const char *const *items, int count, int *selected);
bool  ui_checkbox(int id, Rectangle r, const char *label, bool *value);
bool  ui_slider(int id, Rectangle r, float *value, float min, float max);
void  ui_panel(Rectangle r, const char *title);

#endif
