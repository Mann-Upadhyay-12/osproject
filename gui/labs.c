#include "labs.h"
#include "compare.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAB_BAR_H 34

static const char *LAB_NAMES[LAB_COUNT] = {
    [LAB_REFSTRING] = "Reference string",
    [LAB_DISK]      = "Disk scheduling",
};

static const char *LAB_KEYS[LAB_COUNT] = {
    [LAB_REFSTRING] = "refstring",
    [LAB_DISK]      = "disk",
};

void labs_init(gui_t *g) {
    labs_t *labs = calloc(1, sizeof(labs_t));
    if (!labs) {
        fprintf(stderr, "prism-gui: out of memory\n");
        exit(1);
    }
    lab_refstring_init(&labs->refstring);
    lab_disk_init(&labs->disk);
    g->labs = labs;
}

void labs_free(gui_t *g) {
    free(g->labs);
    g->labs = NULL;
}

void labs_select(gui_t *g, const char *key) {
    g->view = VIEW_LABS;
    for (int i = 0; i < LAB_COUNT; i++) {
        if (strcmp(key, LAB_KEYS[i]) == 0) g->labs->active = i;
    }
}

void labs_drop_file(gui_t *g, const char *path) {
    labs_t *labs = g->labs;
    labs->active = LAB_REFSTRING;
    if (lab_refstring_load_trace(&labs->refstring, path)) {
        gui_status(g, false, "Imported page trace %s", GetFileName(path));
    } else {
        gui_status(g, true, "Cannot import %s as a page trace", GetFileName(path));
    }
}

void lab_draw_section(float x, float *y, const char *title) {
    ui_text(x, *y, 13, UI_ACCENT, "%s", title);
    *y += 20;
}

void lab_draw_hint(float x, float *y, float w, const char *text) {
    *y += ui_text_wrapped(x, *y, w, 12, UI_TEXT_DIM, text) + 6;
}

void lab_legend_swatch(float x, float y, int policy) {
    DrawRectangleRounded((Rectangle){x, y, 12, 12}, 0.3f, 4, cmp_alg_color(policy));
}

bool lab_policy_toggle(int id, float x, float *y, float w, int policy, const char *name, bool *enabled, int *hover) {
    Rectangle row = {x, *y, w, 22};
    if (CheckCollisionPointRec(GetMousePosition(), row)) *hover = policy;

    lab_legend_swatch(x, *y + 5, policy);
    bool changed = ui_checkbox(id, (Rectangle){x + 20, *y, w - 20, 22}, name, enabled);
    *y += 24;
    return changed;
}

void lab_step_controls(int first_id, float x, float y, int *cursor, int total, const char *unit) {
    if (ui_button(first_id, (Rectangle){x, y, 36, 24}, "|<")) *cursor = 0;
    if (ui_button(first_id + 1, (Rectangle){x + 40, y, 36, 24}, "<") && *cursor > 0) (*cursor)--;
    if (ui_button(first_id + 2, (Rectangle){x + 80, y, 36, 24}, ">") && *cursor < total) (*cursor)++;
    if (ui_button(first_id + 3, (Rectangle){x + 120, y, 36, 24}, ">|")) *cursor = total;
    ui_text(x + 166, y + 4, 14, UI_TEXT_DIM, "showing %d of %d %s  (Left / Right keys)", *cursor, total, unit);
}

void lab_step_keys(int *cursor, int total) {
    if (ui_text_focused()) return;
    if ((IsKeyPressed(KEY_RIGHT) || IsKeyPressedRepeat(KEY_RIGHT)) && *cursor < total) (*cursor)++;
    if ((IsKeyPressed(KEY_LEFT) || IsKeyPressedRepeat(KEY_LEFT)) && *cursor > 0) (*cursor)--;
    if (IsKeyPressed(KEY_HOME)) *cursor = 0;
    if (IsKeyPressed(KEY_END)) *cursor = total;
}

void lab_card(Rectangle panel, float x, float y, float w, const algo_info_t *info, const char *fallback) {
    Rectangle card = {panel.x + 1, y, panel.width - 2, panel.y + panel.height - y - 1};
    if (card.height < 60) return;

    DrawLine((int)x, (int)y - 4, (int)(x + w), (int)y - 4, UI_BORDER);
    ui_scissor_push(card);
    if (info) explain_draw_card(info, x, y + 4, w);
    else ui_text_wrapped(x, y + 4, w, 13, UI_TEXT_DIM, fallback);
    ui_scissor_pop();
}

static void draw_tab_bar(labs_t *labs, Rectangle r) {
    DrawRectangleRec(r, UI_PANEL);
    DrawLine((int)r.x, (int)(r.y + r.height - 1), (int)(r.x + r.width), (int)(r.y + r.height - 1), UI_BORDER);

    float x = r.x + 10;
    for (int i = 0; i < LAB_COUNT; i++) {
        float w = ui_text_width(LAB_NAMES[i], 15) + 28;
        if (ui_button_ex(LAB_ID(LAB_COUNT, i), (Rectangle){x, r.y + 5, w, r.height - 10}, LAB_NAMES[i], true, labs->active == i)) {
            labs->active = i;
        }
        x += w + 6;
    }
}

void labs_view(gui_t *g, Rectangle r) {
    labs_t *labs = g->labs;
    Rectangle bar = {r.x, r.y, r.width, TAB_BAR_H};
    Rectangle body = {r.x, r.y + TAB_BAR_H, r.width, r.height - TAB_BAR_H};
    ui_claim_rect(bar);
    draw_tab_bar(labs, bar);

    switch ((lab_kind_t)labs->active) {
        case LAB_REFSTRING: lab_refstring_draw(g, &labs->refstring, body); break;
        case LAB_DISK:      lab_disk_draw(g, &labs->disk, body); break;
        case LAB_COUNT:     break;
    }
}
