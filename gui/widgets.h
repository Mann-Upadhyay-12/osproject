#ifndef PRISM_GUI_WIDGETS_H
#define PRISM_GUI_WIDGETS_H

#include "raylib.h"
#include <stdbool.h>
#include <stddef.h>

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

void  ui_init(void);
void  ui_shutdown(void);
void  ui_begin(void);
void  ui_end(void);

void  ui_push_layer(int layer);
void  ui_pop_layer(void);
void  ui_set_modal(int layer);
bool  ui_input_free(void);
bool  ui_content_input(void);
void  ui_claim_rect(Rectangle r);
bool  ui_text_focused(void);

void  ui_text(float x, float y, float size, Color c, const char *fmt, ...);
float ui_text_width(const char *s, float size);
void  ui_text_clipped(Rectangle r, float size, Color c, const char *s);
float ui_text_wrapped(float x, float y, float w, float size, Color c, const char *s);
float ui_text_wrapped_height(float w, float size, const char *s);
void  ui_tooltip(const char *fmt, ...);
void  ui_scissor_push(Rectangle r);
void  ui_scissor_pop(void);

bool  ui_button(int id, Rectangle r, const char *label);
bool  ui_button_ex(int id, Rectangle r, const char *label, bool enabled, bool active);
bool  ui_text_field(int id, Rectangle r, char *buf, size_t cap);
bool  ui_int_field(int id, Rectangle r, int *value, int min, int max);
bool  ui_dropdown(int id, Rectangle r, const char *const *items, int count, int *selected);
bool  ui_checkbox(int id, Rectangle r, const char *label, bool *value);
bool  ui_slider(int id, Rectangle r, float *value, float min, float max);
void  ui_panel(Rectangle r, const char *title);

#endif
