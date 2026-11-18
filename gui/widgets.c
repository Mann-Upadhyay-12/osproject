#include "widgets.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LAYERS      8
#define MAX_CLIPS       8
#define POPUP_ROW_H     24.0f
#define TOOLTIP_LINE_H  18
#define FONT_SIZE       15

typedef struct {
    Font               font;
    bool               own_font;
    int                layer;
    int                layer_stack[MAX_LAYERS];
    int                layer_depth;
    int                modal;
    bool               mouse_used;
    bool               mouse_consumed;
    int                focus;
    int                focus_seen_this_frame;
    char               edit_buffer[1200];
    int                popup_id;
    Rectangle          popup_rect;
    const char *const *popup_items;
    int                popup_count;
    int                popup_result_id;
    int                popup_result;
    int                active_drag;
    char               tooltip[256];
} ui_state_t;

static ui_state_t U;

static void load_system_font(void) {
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
        if (!FileExists(candidates[i])) continue;

        Font font = LoadFontEx(candidates[i], 32, NULL, 0);
        if (font.texture.id == 0) continue;

        SetTextureFilter(font.texture, TEXTURE_FILTER_BILINEAR);
        U.font = font;
        U.own_font = true;
        return;
    }
}

void ui_init(void) {
    memset(&U, 0, sizeof(U));
    U.font = GetFontDefault();
    load_system_font();
}

void ui_shutdown(void) {
    if (U.own_font) UnloadFont(U.font);
}

static bool layer_live(void) {
    return U.modal == 0 || U.layer >= U.modal;
}

static bool hot(Rectangle r) {
    return layer_live() && !U.mouse_consumed && CheckCollisionPointRec(GetMousePosition(), r);
}

static bool clicked(Rectangle r) {
    bool hovered = hot(r);
    if (hovered) U.mouse_used = true;
    return hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

static bool ctrl_down(void) {
    return IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL) || IsKeyDown(KEY_LEFT_SUPER);
}

static void handle_popup_click(void) {
    bool inside = CheckCollisionPointRec(GetMousePosition(), U.popup_rect);
    if (inside) U.mouse_used = true;

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        if (inside) {
            int row = (int)((GetMouseY() - U.popup_rect.y) / POPUP_ROW_H);
            if (row >= 0 && row < U.popup_count) {
                U.popup_result_id = U.popup_id;
                U.popup_result = row;
            }
        }
        U.popup_id = 0;
        U.mouse_consumed = true;
    }
    if (IsKeyPressed(KEY_ESCAPE)) U.popup_id = 0;
}

void ui_begin(void) {
    U.mouse_used = false;
    U.mouse_consumed = false;
    U.focus_seen_this_frame = 0;
    U.popup_result_id = 0;
    U.tooltip[0] = '\0';

    if (U.popup_id) handle_popup_click();
    if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) U.active_drag = 0;
}

static void draw_drop_shadow(Rectangle r, unsigned char alpha) {
    DrawRectangleRec((Rectangle){r.x + 3, r.y + 3, r.width, r.height}, (Color){0, 0, 0, alpha});
}

static void draw_popup(void) {
    Rectangle r = U.popup_rect;
    draw_drop_shadow(r, 90);
    DrawRectangleRec(r, UI_PANEL_HI);
    DrawRectangleLinesEx(r, 1, UI_ACCENT);

    for (int i = 0; i < U.popup_count; i++) {
        Rectangle row = {r.x, r.y + i * POPUP_ROW_H, r.width, POPUP_ROW_H};
        if (CheckCollisionPointRec(GetMousePosition(), row)) DrawRectangleRec(row, ColorAlpha(UI_ACCENT, 0.35f));
        ui_text_clipped((Rectangle){row.x + 8, row.y, row.width - 12, row.height}, FONT_SIZE, UI_TEXT, U.popup_items[i]);
    }
}

static const char *next_line(const char *text, char *line, size_t size) {
    size_t length = strcspn(text, "\n");
    snprintf(line, size, "%.*s", (int)length, text);
    return text + length + (text[length] ? 1 : 0);
}

static Vector2 tooltip_size(void) {
    char line[256];
    float width = 0, height = 8;
    for (const char *p = U.tooltip; *p;) {
        p = next_line(p, line, sizeof(line));
        float line_w = ui_text_width(line, 14);
        if (line_w > width) width = line_w;
        height += TOOLTIP_LINE_H;
    }
    return (Vector2){width + 16, height};
}

static void draw_tooltip(void) {
    Vector2 mouse = GetMousePosition();
    Vector2 size = tooltip_size();
    Rectangle r = {mouse.x + 16, mouse.y + 18, size.x, size.y};
    if (r.x + r.width > GetScreenWidth()) r.x = mouse.x - r.width - 8;
    if (r.y + r.height > GetScreenHeight()) r.y = mouse.y - r.height - 8;

    draw_drop_shadow(r, 100);
    DrawRectangleRec(r, (Color){16, 18, 23, 245});
    DrawRectangleLinesEx(r, 1, UI_ACCENT);

    char line[256];
    float y = r.y + 5;
    for (const char *p = U.tooltip; *p;) {
        p = next_line(p, line, sizeof(line));
        ui_text(r.x + 8, y, 14, UI_TEXT, "%s", line);
        y += TOOLTIP_LINE_H;
    }
}

void ui_end(void) {
    bool focused_field_gone = U.focus && U.focus_seen_this_frame != U.focus;
    if (focused_field_gone) U.focus = 0;

    if (U.popup_id) draw_popup();
    if (U.tooltip[0] && !U.popup_id) draw_tooltip();
}

void ui_tooltip(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(U.tooltip, sizeof(U.tooltip), fmt, ap);
    va_end(ap);
}

static size_t fit_line(const char *text, float width, float size) {
    char line[256];
    size_t fits = 0, end = 0;

    while (text[end] && text[end] != '\n') {
        size_t word_end = end;
        while (text[word_end] == ' ') word_end++;
        while (text[word_end] && text[word_end] != ' ' && text[word_end] != '\n') word_end++;
        if (word_end >= sizeof(line)) break;

        snprintf(line, sizeof(line), "%.*s", (int)word_end, text);
        if (fits > 0 && ui_text_width(line, size) > width) break;
        fits = word_end;
        end = word_end;
    }

    bool word_longer_than_line = fits == 0 && text[0] != '\n';
    if (word_longer_than_line) fits = end > 0 ? end : 1;
    return fits;
}

static float wrap_text(float x, float y, float width, float size, Color c, const char *text, bool draw) {
    char line[256];
    float line_h = size + 4, top = y;

    for (const char *p = text; *p;) {
        size_t length = fit_line(p, width, size);
        snprintf(line, sizeof(line), "%.*s", (int)length, p);
        if (draw) ui_text(x, y, size, c, "%s", line);
        y += line_h;

        p += length;
        if (*p == '\n') p++;
        while (*p == ' ') p++;
    }
    return y - top;
}

void ui_push_layer(int layer) {
    if (U.layer_depth < MAX_LAYERS) U.layer_stack[U.layer_depth++] = U.layer;
    U.layer = layer;
}

void ui_pop_layer(void) {
    U.layer = U.layer_depth ? U.layer_stack[--U.layer_depth] : 0;
}

void ui_set_modal(int layer) {
    U.modal = layer;
}

bool ui_input_free(void) {
    return !U.mouse_used && !U.mouse_consumed && U.modal == 0 && U.popup_id == 0;
}

bool ui_content_input(void) {
    return layer_live() && !U.mouse_consumed && U.popup_id == 0;
}

void ui_claim_rect(Rectangle r) {
    if (CheckCollisionPointRec(GetMousePosition(), r)) U.mouse_used = true;
}

bool ui_text_focused(void) {
    return U.focus != 0;
}

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

static Rectangle g_clips[MAX_CLIPS];
static int g_num_clips;

static Rectangle intersect(Rectangle a, Rectangle b) {
    float left = a.x > b.x ? a.x : b.x;
    float top = a.y > b.y ? a.y : b.y;
    float right = a.x + a.width < b.x + b.width ? a.x + a.width : b.x + b.width;
    float bottom = a.y + a.height < b.y + b.height ? a.y + a.height : b.y + b.height;
    return (Rectangle){left, top, right > left ? right - left : 0, bottom > top ? bottom - top : 0};
}

static void apply_scissor(Rectangle r) {
    BeginScissorMode((int)r.x, (int)r.y, (int)r.width, (int)r.height);
}

void ui_scissor_push(Rectangle r) {
    if (g_num_clips > 0) r = intersect(r, g_clips[g_num_clips - 1]);
    if (g_num_clips < MAX_CLIPS) g_clips[g_num_clips++] = r;
    apply_scissor(r);
}

void ui_scissor_pop(void) {
    if (g_num_clips > 0) g_num_clips--;
    if (g_num_clips > 0) {
        apply_scissor(g_clips[g_num_clips - 1]);
    } else {
        EndScissorMode();
    }
}

static void truncate_with_dots(char *text, size_t capacity, float max_width, float size) {
    if (ui_text_width(text, size) <= max_width) return;

    size_t length = strlen(text);
    if (length > capacity - 3) length = capacity - 3;
    while (length > 0) {
        length--;
        text[length] = '.';
        text[length + 1] = '.';
        text[length + 2] = '\0';
        if (ui_text_width(text, size) <= max_width) return;
    }
}

void ui_text_clipped(Rectangle r, float size, Color c, const char *s) {
    char text[256];
    snprintf(text, sizeof(text), "%s", s);
    truncate_with_dots(text, sizeof(text), r.width, size);
    DrawTextEx(U.font, text, (Vector2){r.x, r.y + (r.height - size) / 2}, size, 1, c);
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
    bool hovered = enabled && hot(r);
    bool pressed = enabled && clicked(r);
    if (!enabled && hot(r)) U.mouse_used = true;

    Color fill = active ? ColorAlpha(UI_ACCENT, 0.55f) : (hovered ? UI_PANEL_HI : UI_PANEL);
    if (hovered && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) fill = ColorAlpha(UI_ACCENT, 0.4f);
    DrawRectangleRounded(r, 0.25f, 4, fill);
    DrawRectangleRoundedLinesEx(r, 0.25f, 4, 1, active ? UI_ACCENT : UI_BORDER);

    float text_w = ui_text_width(label, FONT_SIZE);
    ui_text(r.x + (r.width - text_w) / 2, r.y + (r.height - FONT_SIZE) / 2, FONT_SIZE,
            enabled ? UI_TEXT : UI_TEXT_DIM, "%s", label);
    return pressed;
}

bool ui_button(int id, Rectangle r, const char *label) {
    return ui_button_ex(id, r, label, true, false);
}

static void type_characters(size_t capacity, bool digits_only) {
    int ch;
    while ((ch = GetCharPressed()) != 0) {
        size_t length = strlen(U.edit_buffer);
        bool allowed = digits_only ? ((ch >= '0' && ch <= '9') || (ch == '-' && length == 0))
                                   : (ch >= 32 && ch < 127);
        bool has_room = length + 1 < sizeof(U.edit_buffer) && length + 1 < capacity;
        if (allowed && has_room) {
            U.edit_buffer[length] = (char)ch;
            U.edit_buffer[length + 1] = '\0';
        }
    }
}

static void handle_deletion(void) {
    bool backspace = IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE);
    if (ctrl_down() && (IsKeyPressed(KEY_A) || IsKeyPressed(KEY_BACKSPACE))) {
        U.edit_buffer[0] = '\0';
    } else if (backspace && U.edit_buffer[0]) {
        U.edit_buffer[strlen(U.edit_buffer) - 1] = '\0';
    }
}

static bool finish_editing_on_key(char *buf, size_t capacity) {
    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER) || IsKeyPressed(KEY_TAB)) {
        snprintf(buf, capacity, "%s", U.edit_buffer);
        U.focus = 0;
        return true;
    }
    if (IsKeyPressed(KEY_ESCAPE)) U.focus = 0;
    return false;
}

static void draw_field(Rectangle r, const char *shown, bool focused) {
    DrawRectangleRec(r, focused ? (Color){18, 20, 25, 255} : (Color){28, 31, 38, 255});
    DrawRectangleLinesEx(r, 1, focused ? UI_ACCENT : (hot(r) ? UI_TEXT_DIM : UI_BORDER));

    Rectangle inner = {r.x + 6, r.y, r.width - 10, r.height};
    float text_w = ui_text_width(shown, FONT_SIZE);
    float scroll = text_w > inner.width - 8 ? text_w - (inner.width - 8) : 0;

    ui_scissor_push(inner);
    ui_text(inner.x - scroll, r.y + (r.height - FONT_SIZE) / 2, FONT_SIZE, UI_TEXT, "%s", shown);
    bool caret_visible = (int)(GetTime() * 2) % 2 == 0;
    if (focused && caret_visible) {
        DrawRectangle((int)(inner.x - scroll + text_w + 1), (int)(r.y + 5), 2, (int)(r.height - 10), UI_ACCENT);
    }
    ui_scissor_pop();
}

static bool edit_field(int id, Rectangle r, char *buf, size_t capacity, bool digits_only) {
    bool committed = false;

    if (clicked(r) && U.focus != id) {
        U.focus = id;
        snprintf(U.edit_buffer, sizeof(U.edit_buffer), "%s", buf);
    } else if (U.focus == id && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !hot(r)) {
        snprintf(buf, capacity, "%s", U.edit_buffer);
        U.focus = 0;
        committed = true;
    }

    bool focused = U.focus == id;
    if (focused) {
        U.focus_seen_this_frame = id;
        type_characters(capacity, digits_only);
        handle_deletion();
        if (finish_editing_on_key(buf, capacity)) committed = true;
    }

    draw_field(r, focused ? U.edit_buffer : buf, focused);
    return committed;
}

bool ui_text_field(int id, Rectangle r, char *buf, size_t cap) {
    return edit_field(id, r, buf, cap, false);
}

static int clamp_int(int value, int min, int max) {
    if (value < min) return min;
    if (value > max) return max;
    return value;
}

bool ui_int_field(int id, Rectangle r, int *value, int min, int max) {
    bool changed = false;
    float button_w = r.height;
    Rectangle minus = {r.x, r.y, button_w, r.height};
    Rectangle plus = {r.x + r.width - button_w, r.y, button_w, r.height};
    Rectangle middle = {r.x + button_w + 2, r.y, r.width - 2 * button_w - 4, r.height};
    int step = IsKeyDown(KEY_LEFT_SHIFT) ? 10 : 1;

    if (ui_button(id * 3 + 1, minus, "-") && *value > min) {
        *value = clamp_int(*value - step, min, max);
        changed = true;
    }
    if (ui_button(id * 3 + 2, plus, "+") && *value < max) {
        *value = clamp_int(*value + step, min, max);
        changed = true;
    }

    if (hot(middle) && U.focus != id) {
        float wheel = GetMouseWheelMove();
        if (wheel > 0 && *value < max) { (*value)++; changed = true; }
        if (wheel < 0 && *value > min) { (*value)--; changed = true; }
    }

    char text[32];
    snprintf(text, sizeof(text), "%d", *value);
    if (edit_field(id, middle, text, sizeof(text), true)) {
        int typed = clamp_int(atoi(text), min, max);
        if (typed != *value) {
            *value = typed;
            changed = true;
        }
    }
    return changed;
}

static void open_popup(int id, Rectangle anchor, const char *const *items, int count) {
    float height = POPUP_ROW_H * count;
    float y = anchor.y + anchor.height + 2;
    if (y + height > GetScreenHeight()) y = anchor.y - height - 2;

    U.popup_id = id;
    U.popup_items = items;
    U.popup_count = count;
    U.popup_rect = (Rectangle){anchor.x, y, anchor.width < 140 ? 140 : anchor.width, height};
}

bool ui_dropdown(int id, Rectangle r, const char *const *items, int count, int *selected) {
    bool changed = false;
    if (U.popup_result_id == id) {
        changed = *selected != U.popup_result;
        *selected = U.popup_result;
        U.popup_result_id = 0;
    }

    bool open = U.popup_id == id;
    if (clicked(r) && !open) {
        open_popup(id, r, items, count);
        open = true;
    }

    DrawRectangleRounded(r, 0.2f, 4, hot(r) || open ? UI_PANEL_HI : UI_PANEL);
    DrawRectangleRoundedLinesEx(r, 0.2f, 4, 1, open ? UI_ACCENT : UI_BORDER);

    const char *label = *selected >= 0 && *selected < count ? items[*selected] : "-";
    ui_text_clipped((Rectangle){r.x + 8, r.y, r.width - 26, r.height}, FONT_SIZE, UI_TEXT, label);

    float cx = r.x + r.width - 13, cy = r.y + r.height / 2;
    DrawTriangle((Vector2){cx - 5, cy - 2}, (Vector2){cx, cy + 4}, (Vector2){cx + 5, cy - 2}, UI_TEXT_DIM);
    return changed;
}

bool ui_checkbox(int id, Rectangle r, const char *label, bool *value) {
    (void)id;
    Rectangle box = {r.x, r.y + (r.height - 16) / 2, 16, 16};
    Rectangle hit = {r.x, r.y, 22 + ui_text_width(label, 15), r.height};
    bool toggled = clicked(hit);
    if (toggled) *value = !*value;
    DrawRectangleRec(box, (Color){28, 31, 38, 255});
    DrawRectangleLinesEx(box, 1, hot(hit) ? UI_ACCENT : UI_BORDER);
    if (*value) DrawRectangle((int)box.x + 4, (int)box.y + 4, 8, 8, UI_ACCENT);
    ui_text(r.x + 22, r.y + (r.height - FONT_SIZE) / 2, FONT_SIZE, UI_TEXT, "%s", label);
    return toggled;
}

bool ui_slider(int id, Rectangle r, float *value, float min, float max) {
    if (clicked(r)) U.active_drag = id;
    bool changed = false;
    if (U.active_drag == id) {
        float fraction = (GetMouseX() - r.x) / r.width;
        if (fraction < 0) fraction = 0;
        if (fraction > 1) fraction = 1;
        float dragged = min + fraction * (max - min);
        changed = dragged != *value;
        *value = dragged;
        U.mouse_used = true;
    }

    float fraction = (*value - min) / (max - min);
    float track_y = r.y + r.height / 2 - 3;
    bool highlighted = hot(r) || U.active_drag == id;
    DrawRectangleRounded((Rectangle){r.x, track_y, r.width, 6}, 1, 4, UI_BORDER);
    DrawRectangleRounded((Rectangle){r.x, track_y, r.width * fraction, 6}, 1, 4, UI_ACCENT);
    DrawCircle((int)(r.x + r.width * fraction), (int)(r.y + r.height / 2), 7, highlighted ? UI_TEXT : UI_TEXT_DIM);
    return changed;
}

float ui_text_wrapped(float x, float y, float w, float size, Color c, const char *s) {
    return wrap_text(x, y, w, size, c, s, true);
}

float ui_text_wrapped_height(float w, float size, const char *s) {
    return wrap_text(0, 0, w, size, BLANK, s, false);
}
