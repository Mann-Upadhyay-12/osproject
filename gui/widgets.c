#include "widgets.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    Font  font;
    bool  own_font;
    int   layer;            /* layer of widgets being drawn now */
    int   layer_stack[8];
    int   layer_depth;
    int   modal;            /* only this layer receives input when > 0 */
    bool  mouse_used;       /* a widget/popup took the mouse this frame */
    bool  mouse_eaten;      /* a popup consumed this frame's click */
    int   focus;            /* text-field id with keyboard focus, 0 = none */
    int   focus_seen;       /* focused field drew this frame */
    char  edit[1200];        /* edit buffer for the focused field */
    /* popup (dropdown list) */
    int   pop_id;
    Rectangle pop_rect;
    const char *const *pop_items;
    int   pop_count;
    int   pop_result_id, pop_result;
    int   active_drag;      /* slider id being dragged */
} ui_state_t;

static ui_state_t U;

void ui_init(void) {
    memset(&U, 0, sizeof(U));
    U.font = GetFontDefault();
    static const char *candidates[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/noto/NotoSans-Regular.ttf",
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/Library/Fonts/Arial.ttf",
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/arial.ttf",
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if (FileExists(candidates[i])) {
            Font f = LoadFontEx(candidates[i], 32, NULL, 0);
            if (f.texture.id != 0) {
                SetTextureFilter(f.texture, TEXTURE_FILTER_BILINEAR);
                U.font = f;
                U.own_font = true;
                break;
            }
        }
    }
}

void ui_shutdown(void) {
    if (U.own_font) UnloadFont(U.font);
}

static bool layer_live(void) {
    return U.modal == 0 || U.layer >= U.modal;
}

static bool hot(Rectangle r) {
    return layer_live() && !U.mouse_eaten && CheckCollisionPointRec(GetMousePosition(), r);
}

static bool clicked(Rectangle r) {
    bool h = hot(r);
    if (h) U.mouse_used = true;
    return h && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

void ui_begin(void) {
    U.mouse_used = false;
    U.mouse_eaten = false;
    U.focus_seen = 0;
    U.pop_result_id = 0;
    if (U.pop_id) {
        bool inside = CheckCollisionPointRec(GetMousePosition(), U.pop_rect);
        if (inside) U.mouse_used = true;
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            if (inside) {
                int idx = (int)((GetMouseY() - U.pop_rect.y) / 24.0f);
                if (idx >= 0 && idx < U.pop_count) {
                    U.pop_result_id = U.pop_id;
                    U.pop_result = idx;
                }
            }
            U.pop_id = 0;
            U.mouse_eaten = true;
        }
        if (IsKeyPressed(KEY_ESCAPE)) U.pop_id = 0;
    }
    if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) U.active_drag = 0;
}

void ui_end(void) {
    /* A focused field that did not draw this frame loses focus. */
    if (U.focus && U.focus_seen != U.focus) U.focus = 0;
    if (U.pop_id) {
        Rectangle r = U.pop_rect;
        DrawRectangleRec((Rectangle){r.x + 3, r.y + 3, r.width, r.height}, (Color){0, 0, 0, 90});
        DrawRectangleRec(r, UI_PANEL_HI);
        DrawRectangleLinesEx(r, 1, UI_ACCENT);
        for (int i = 0; i < U.pop_count; i++) {
            Rectangle row = {r.x, r.y + i * 24.0f, r.width, 24};
            if (CheckCollisionPointRec(GetMousePosition(), row)) DrawRectangleRec(row, ColorAlpha(UI_ACCENT, 0.35f));
            ui_text_clipped((Rectangle){row.x + 8, row.y, row.width - 12, row.height}, 15, UI_TEXT, U.pop_items[i]);
        }
    }
}

void ui_push_layer(int layer) {
    if (U.layer_depth < 8) U.layer_stack[U.layer_depth++] = U.layer;
    U.layer = layer;
}

void ui_pop_layer(void) {
    U.layer = U.layer_depth ? U.layer_stack[--U.layer_depth] : 0;
}

void ui_set_modal(int layer) { U.modal = layer; }

bool ui_input_free(void) {
    return !U.mouse_used && !U.mouse_eaten && U.modal == 0 && U.pop_id == 0;
}

void ui_claim_rect(Rectangle r) {
    if (CheckCollisionPointRec(GetMousePosition(), r)) U.mouse_used = true;
}

bool ui_text_focused(void) { return U.focus != 0; }

/* ------------------------------------------------------------------------ */

void ui_text(float x, float y, float size, Color c, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    DrawTextEx(U.font, buf, (Vector2){x, y}, size, 1, c);
}

float ui_text_width(const char *s, float size) {
    return MeasureTextEx(U.font, s, size, 1).x;
}

static Rectangle g_clip[8];
static int g_nclip;

void ui_scissor_push(Rectangle r) {
    if (g_nclip > 0) {
        Rectangle t = g_clip[g_nclip - 1];
        float x0 = r.x > t.x ? r.x : t.x, y0 = r.y > t.y ? r.y : t.y;
        float x1 = (r.x + r.width < t.x + t.width) ? r.x + r.width : t.x + t.width;
        float y1 = (r.y + r.height < t.y + t.height) ? r.y + r.height : t.y + t.height;
        r = (Rectangle){x0, y0, x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0};
    }
    if (g_nclip < 8) g_clip[g_nclip++] = r;
    BeginScissorMode((int)r.x, (int)r.y, (int)r.width, (int)r.height);
}

void ui_scissor_pop(void) {
    if (g_nclip > 0) g_nclip--;
    if (g_nclip > 0) {
        Rectangle t = g_clip[g_nclip - 1];
        BeginScissorMode((int)t.x, (int)t.y, (int)t.width, (int)t.height);
    } else {
        EndScissorMode();
    }
}

/* Truncation instead of scissoring so it also works inside Mode2D. */
void ui_text_clipped(Rectangle r, float size, Color c, const char *s) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", s);
    size_t n = strlen(buf);
    if (ui_text_width(buf, size) > r.width) {
        if (n > sizeof(buf) - 3) n = sizeof(buf) - 3;
        while (n > 0) {
            n--;
            buf[n] = '.'; buf[n + 1] = '.'; buf[n + 2] = '\0';
            if (ui_text_width(buf, size) <= r.width) break;
        }
    }
    DrawTextEx(U.font, buf, (Vector2){r.x, r.y + (r.height - size) / 2}, size, 1, c);
}

void ui_panel(Rectangle r, const char *title) {
    DrawRectangleRec(r, UI_PANEL);
    DrawRectangleLinesEx(r, 1, UI_BORDER);
    if (title) {
        DrawRectangle((int)r.x, (int)r.y, (int)r.width, 24, UI_PANEL_HI);
        ui_text(r.x + 8, r.y + 4, 15, UI_TEXT_DIM, "%s", title);
    }
}

bool ui_button_ex(int id, Rectangle r, const char *label, bool enabled, bool active) {
    (void)id;
    bool h = enabled && hot(r);
    bool c = enabled && clicked(r);
    if (!enabled && hot(r)) U.mouse_used = true;
    Color bg = active ? ColorAlpha(UI_ACCENT, 0.55f) : (h ? UI_PANEL_HI : UI_PANEL);
    if (h && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) bg = ColorAlpha(UI_ACCENT, 0.4f);
    DrawRectangleRounded(r, 0.25f, 4, bg);
    DrawRectangleRoundedLinesEx(r, 0.25f, 4, 1, active ? UI_ACCENT : UI_BORDER);
    float w = ui_text_width(label, 15);
    ui_text(r.x + (r.width - w) / 2, r.y + (r.height - 15) / 2, 15, enabled ? UI_TEXT : UI_TEXT_DIM, "%s", label);
    return c;
}

bool ui_button(int id, Rectangle r, const char *label) {
    return ui_button_ex(id, r, label, true, false);
}

/* Shared editing for text/int fields. Returns true when the edit commits. */
static bool edit_field(int id, Rectangle r, char *buf, size_t cap, bool digits_only) {
    bool committed = false;
    if (clicked(r) && U.focus != id) {
        U.focus = id;
        snprintf(U.edit, sizeof(U.edit), "%s", buf);
    } else if (U.focus == id && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !hot(r)) {
        snprintf(buf, cap, "%s", U.edit);
        U.focus = 0;
        committed = true;
    }
    bool focused = (U.focus == id);
    if (focused) {
        U.focus_seen = id;
        int ch;
        while ((ch = GetCharPressed()) != 0) {
            size_t n = strlen(U.edit);
            bool ok = digits_only ? ((ch >= '0' && ch <= '9') || (ch == '-' && n == 0))
                                  : (ch >= 32 && ch < 127);
            if (ok && n + 1 < sizeof(U.edit) && n + 1 < cap) {
                U.edit[n] = (char)ch;
                U.edit[n + 1] = '\0';
            }
        }
        bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL) || IsKeyDown(KEY_LEFT_SUPER);
        if (ctrl && (IsKeyPressed(KEY_A) || IsKeyPressed(KEY_BACKSPACE))) {
            U.edit[0] = '\0';   /* no selection model: "select all" means start over */
        } else if ((IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) && U.edit[0]) {
            U.edit[strlen(U.edit) - 1] = '\0';
        }
        if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER) || IsKeyPressed(KEY_TAB)) {
            snprintf(buf, cap, "%s", U.edit);
            U.focus = 0;
            committed = true;
        } else if (IsKeyPressed(KEY_ESCAPE)) {
            U.focus = 0;
        }
    }
    DrawRectangleRec(r, focused ? (Color){18, 20, 25, 255} : (Color){28, 31, 38, 255});
    DrawRectangleLinesEx(r, 1, focused ? UI_ACCENT : (hot(r) ? UI_TEXT_DIM : UI_BORDER));
    const char *shown = focused ? U.edit : buf;
    Rectangle inner = {r.x + 6, r.y, r.width - 10, r.height};
    float w = ui_text_width(shown, 15);
    float off = (w > inner.width - 8) ? (w - (inner.width - 8)) : 0;   /* keep caret visible */
    ui_scissor_push(inner);
    ui_text(inner.x - off, r.y + (r.height - 15) / 2, 15, UI_TEXT, "%s", shown);
    if (focused && ((int)(GetTime() * 2) % 2 == 0)) {
        DrawRectangle((int)(inner.x - off + w + 1), (int)(r.y + 5), 2, (int)(r.height - 10), UI_ACCENT);
    }
    ui_scissor_pop();
    return committed;
}

bool ui_text_field(int id, Rectangle r, char *buf, size_t cap) {
    return edit_field(id, r, buf, cap, false);
}

bool ui_int_field(int id, Rectangle r, int *value, int min, int max) {
    bool changed = false;
    float bw = r.height;
    Rectangle minus = {r.x, r.y, bw, r.height};
    Rectangle plus = {r.x + r.width - bw, r.y, bw, r.height};
    Rectangle mid = {r.x + bw + 2, r.y, r.width - 2 * bw - 4, r.height};
    int step = IsKeyDown(KEY_LEFT_SHIFT) ? 10 : 1;
    if (ui_button(id * 3 + 1, minus, "-") && *value > min) { *value = (*value - step < min) ? min : *value - step; changed = true; }
    if (ui_button(id * 3 + 2, plus, "+") && *value < max) { *value = (*value + step > max) ? max : *value + step; changed = true; }
    if (hot(mid) && U.focus != id) {
        float wh = GetMouseWheelMove();
        if (wh > 0 && *value < max) { (*value)++; changed = true; }
        if (wh < 0 && *value > min) { (*value)--; changed = true; }
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", *value);
    if (edit_field(id, mid, buf, sizeof(buf), true)) {
        int v = atoi(buf);
        if (v < min) v = min;
        if (v > max) v = max;
        if (v != *value) { *value = v; changed = true; }
    }
    return changed;
}

bool ui_dropdown(int id, Rectangle r, const char *const *items, int count, int *selected) {
    bool changed = false;
    if (U.pop_result_id == id) {
        changed = (*selected != U.pop_result);
        *selected = U.pop_result;
        U.pop_result_id = 0;
    }
    bool open = (U.pop_id == id);
    if (clicked(r) && !open) {
        U.pop_id = id;
        U.pop_items = items;
        U.pop_count = count;
        float h = 24.0f * count;
        float y = r.y + r.height + 2;
        if (y + h > GetScreenHeight()) y = r.y - h - 2;
        U.pop_rect = (Rectangle){r.x, y, r.width < 140 ? 140 : r.width, h};
        open = true;
    }
    DrawRectangleRounded(r, 0.2f, 4, hot(r) || open ? UI_PANEL_HI : UI_PANEL);
    DrawRectangleRoundedLinesEx(r, 0.2f, 4, 1, open ? UI_ACCENT : UI_BORDER);
    const char *label = (*selected >= 0 && *selected < count) ? items[*selected] : "-";
    ui_text_clipped((Rectangle){r.x + 8, r.y, r.width - 26, r.height}, 15, UI_TEXT, label);
    float cx = r.x + r.width - 13, cy = r.y + r.height / 2;
    DrawTriangle((Vector2){cx - 5, cy - 2}, (Vector2){cx, cy + 4}, (Vector2){cx + 5, cy - 2}, UI_TEXT_DIM);
    return changed;
}

bool ui_checkbox(int id, Rectangle r, const char *label, bool *value) {
    (void)id;
    Rectangle box = {r.x, r.y + (r.height - 16) / 2, 16, 16};
    Rectangle hit = {r.x, r.y, 22 + ui_text_width(label, 15), r.height};
    bool c = clicked(hit);
    if (c) *value = !*value;
    DrawRectangleRec(box, (Color){28, 31, 38, 255});
    DrawRectangleLinesEx(box, 1, hot(hit) ? UI_ACCENT : UI_BORDER);
    if (*value) DrawRectangle((int)box.x + 4, (int)box.y + 4, 8, 8, UI_ACCENT);
    ui_text(r.x + 22, r.y + (r.height - 15) / 2, 15, UI_TEXT, "%s", label);
    return c;
}

bool ui_slider(int id, Rectangle r, float *value, float min, float max) {
    if (clicked(r)) U.active_drag = id;
    bool changed = false;
    if (U.active_drag == id) {
        float t = (GetMouseX() - r.x) / r.width;
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        float v = min + t * (max - min);
        changed = (v != *value);
        *value = v;
        U.mouse_used = true;
    }
    float t = (*value - min) / (max - min);
    DrawRectangleRounded((Rectangle){r.x, r.y + r.height / 2 - 3, r.width, 6}, 1, 4, UI_BORDER);
    DrawRectangleRounded((Rectangle){r.x, r.y + r.height / 2 - 3, r.width * t, 6}, 1, 4, UI_ACCENT);
    DrawCircle((int)(r.x + r.width * t), (int)(r.y + r.height / 2), 7, hot(r) || U.active_drag == id ? UI_TEXT : UI_TEXT_DIM);
    return changed;
}
