#include "labs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAB_BAR_H 34

static const char *LAB_NAMES[LAB_COUNT] = {
    [LAB_REFSTRING] = "Reference string",
};

static const char *LAB_KEYS[LAB_COUNT] = {
    [LAB_REFSTRING] = "refstring",
};

void labs_init(gui_t *g) {
    labs_t *labs = calloc(1, sizeof(labs_t));
    if (!labs) {
        fprintf(stderr, "prism-gui: out of memory\n");
        exit(1);
    }
    lab_refstring_init(&labs->refstring);
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
        case LAB_COUNT:     break;
    }
}
