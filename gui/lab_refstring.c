#include "labs.h"
#include <stdio.h>
#include <string.h>

#define ID(n)       LAB_ID(LAB_REFSTRING, n)
#define CELL_W      28.0f
#define ROW_H       20.0f
#define TITLE_H     24.0f
#define TABLE_GAP   14.0f
#define EXPLAIN_H   96.0f

typedef struct {
    const char *label;
    const char *refs;
    int         frames;
} refstring_preset_t;

static const refstring_preset_t PRESETS[] = {
    {"Textbook", "7 0 1 2 0 3 0 4 2 3 0 3 2 1 2 0 1 7 0 1", 3},
    {"Belady", "1 2 3 4 1 2 5 1 2 3 4 5", 3},
    {"Locality", "1 2 3 1 2 3 1 2 4 5 4 5 4 5 6 7 6 7 1 2 3 1 2 3", 3},
};
#define NUM_PRESETS ((int)(sizeof(PRESETS) / sizeof(PRESETS[0])))

static void set_message(refstring_lab_t *lab, bool bad, const char *text) {
    snprintf(lab->message, sizeof(lab->message), "%s", text);
    lab->message_bad = bad;
}

static void load_preset(refstring_lab_t *lab, const refstring_preset_t *preset) {
    snprintf(lab->input, sizeof(lab->input), "%s", preset->refs);
    lab->frames = preset->frames;
    lab->dirty = true;
}

void lab_refstring_init(refstring_lab_t *lab) {
    memset(lab, 0, sizeof(*lab));
    for (int p = 0; p < REPL_COUNT; p++) lab->enabled[p] = true;
    lab->page_size = 4096;
    lab->hover_policy = -1;
    load_preset(lab, &PRESETS[0]);
}

static void fill_random(refstring_lab_t *lab) {
    static uint32_t seed = 12345;
    size_t used = 0;
    lab->input[0] = '\0';
    for (int i = 0; i < 20; i++) {
        seed = seed * 1103515245u + 12345u;
        int page = (int)((seed >> 16) % 8);
        used += (size_t)snprintf(lab->input + used, sizeof(lab->input) - used, i ? " %d" : "%d", page);
    }
    lab->dirty = true;
}

static void write_refs_as_text(refstring_lab_t *lab) {
    size_t used = 0;
    lab->input[0] = '\0';
    for (int i = 0; i < lab->count && used + 4 < sizeof(lab->input); i++) {
        used += (size_t)snprintf(lab->input + used, sizeof(lab->input) - used, i ? " %d" : "%d", lab->refs[i]);
    }
}

bool lab_refstring_load_trace(refstring_lab_t *lab, const char *path) {
    trace_info_t info;
    uint16_t refs[REFSTR_MAX_REFS];
    refstr_status_t status = refstr_load_trace(path, (uint32_t)lab->page_size, refs, REFSTR_MAX_REFS, &info);
    if (status != REFSTR_OK) {
        set_message(lab, true, refstr_strerror(status));
        return false;
    }

    memcpy(lab->refs, refs, sizeof(refs));
    lab->count = info.count;
    write_refs_as_text(lab);
    lab->dirty = true;

    char text[200];
    snprintf(text, sizeof(text), "Imported %d references, %d distinct pages from %s%s", info.count, info.distinct_pages,
             GetFileName(path), info.truncated ? " (only the start fits)" : "");
    set_message(lab, false, text);
    return true;
}

static int count_distinct(const uint16_t *refs, int count) {
    bool seen[PRISM_MAX_VIRTUAL_PAGES] = {false};
    int distinct = 0;
    for (int i = 0; i < count; i++) {
        if (!seen[refs[i]]) distinct++;
        seen[refs[i]] = true;
    }
    return distinct;
}

static void recompute(refstring_lab_t *lab) {
    lab->dirty = false;
    refstr_status_t status = refstr_parse(lab->input, lab->refs, REFSTR_MAX_REFS, &lab->count);
    if (status != REFSTR_OK) {
        lab->count = 0;
        set_message(lab, true, refstr_strerror(status));
        return;
    }

    for (int p = 0; p < REPL_COUNT; p++) {
        if (lab->enabled[p]) refstr_simulate((repl_policy_t)p, lab->frames, lab->refs, lab->count, &lab->results[p]);
    }
    lab->cursor = lab->count;

    char text[200];
    snprintf(text, sizeof(text), "%d references, %d distinct pages, %d frames", lab->count,
             count_distinct(lab->refs, lab->count), lab->frames);
    set_message(lab, false, text);
}

static float draw_input_section(refstring_lab_t *lab, float x, float y, float w) {
    lab_draw_section(x, &y, "1. REFERENCE STRING");
    if (ui_text_field(ID(1), (Rectangle){x, y, w, 26}, lab->input, sizeof(lab->input))) lab->dirty = true;
    y += 30;
    ui_text_clipped((Rectangle){x, y, w, 16}, 12, lab->message_bad ? UI_BAD : UI_TEXT_DIM, lab->message);
    y += 22;

    float button_w = (w - 4 * NUM_PRESETS) / (NUM_PRESETS + 1);
    for (int i = 0; i < NUM_PRESETS; i++) {
        Rectangle button = {x + i * (button_w + 4), y, button_w, 24};
        if (ui_button(ID(10 + i), button, PRESETS[i].label)) load_preset(lab, &PRESETS[i]);
    }
    if (ui_button(ID(19), (Rectangle){x + NUM_PRESETS * (button_w + 4), y, button_w, 24}, "Random")) fill_random(lab);
    return y + 36;
}

static float draw_frames_section(refstring_lab_t *lab, float x, float y) {
    lab_draw_section(x, &y, "2. FRAMES");
    if (ui_int_field(ID(20), (Rectangle){x, y, 130, 26}, &lab->frames, 1, REFSTR_MAX_FRAMES)) lab->dirty = true;
    return y + 38;
}

static float draw_policy_section(refstring_lab_t *lab, float x, float y, float w) {
    lab_draw_section(x, &y, "3. POLICIES");
    for (int p = 0; p < REPL_COUNT; p++) {
        const char *name = repl_get_ops((repl_policy_t)p)->name;
        if (lab_policy_toggle(ID(30 + p), x, &y, w, p, name, &lab->enabled[p], &lab->hover_policy)) lab->dirty = true;
    }
    return y + 10;
}

static float draw_trace_section(refstring_lab_t *lab, float x, float y, float w) {
    lab_draw_section(x, &y, "4. IMPORT A TRACE");
    ui_text(x, y + 5, 13, UI_TEXT_DIM, "page size (bytes)");
    ui_int_field(ID(40), (Rectangle){x + 130, y, 130, 26}, &lab->page_size, 64, 65536);
    y += 34;
    lab_draw_hint(x, &y, w,
                  "Drop a trace file on the window: valgrind --tool=lackey output, or one address per line. "
                  "Pages are renumbered 0, 1, 2... in order of first use (64 pages, 256 references max).");
    return y + 6;
}

static void draw_policy_card(refstring_lab_t *lab, Rectangle panel, float x, float y, float w) {
    const algo_info_t *info = lab->hover_policy >= 0 ? explain_repl_info((repl_policy_t)lab->hover_policy) : NULL;
    lab_card(panel, x, y, w, info,
             "Hover a policy to see how it chooses a victim. Each table is the classic textbook "
             "frame table: one column per reference, red = page fault, green outline = hit.");
}

static void draw_setup_panel(refstring_lab_t *lab, Rectangle r) {
    ui_claim_rect(r);
    ui_panel(r, NULL);
    float x = r.x + 12, y = r.y + 10, w = r.width - 24;

    y = draw_input_section(lab, x, y, w);
    y = draw_frames_section(lab, x, y);
    y = draw_policy_section(lab, x, y, w);
    y = draw_trace_section(lab, x, y, w);
    draw_policy_card(lab, r, x, y, w);
}

static int best_fault_count(const refstring_lab_t *lab) {
    int best = -1;
    for (int p = 0; p < REPL_COUNT; p++) {
        if (!lab->enabled[p]) continue;
        if (best < 0 || lab->results[p].faults < best) best = lab->results[p].faults;
    }
    return best;
}

static int faults_until(const refstr_result_t *result, int cursor) {
    int faults = 0;
    for (int i = 0; i < cursor && i < result->num_steps; i++) faults += result->steps[i].fault;
    return faults;
}

static void draw_summary(refstring_lab_t *lab, float x, float y) {
    int best = best_fault_count(lab);
    for (int p = 0; p < REPL_COUNT; p++) {
        if (!lab->enabled[p]) continue;
        const refstr_result_t *result = &lab->results[p];
        bool is_best = result->faults == best;
        double rate = lab->count ? 100.0 * result->faults / lab->count : 0.0;

        const char *text = TextFormat("%s  %d faults (%.0f%%)", repl_get_ops((repl_policy_t)p)->name, result->faults, rate);
        lab_legend_swatch(x, y + 3, p);
        ui_text(x + 18, y, 15, is_best ? UI_GOOD : UI_TEXT, "%s", text);
        x += ui_text_width(text, 15) + 40;
    }
}

static float table_height(const refstring_lab_t *lab) {
    return TITLE_H + ROW_H * (lab->frames + 2);
}

static void draw_frame_cell(const refstr_step_t *step, int frame, Rectangle cell) {
    bool loaded_here = step->fault && step->frame == frame;
    bool hit_here = !step->fault && step->frame == frame;

    DrawRectangleRec(cell, loaded_here ? ColorAlpha(UI_BAD, 0.55f) : (Color){30, 33, 40, 255});
    if (hit_here) DrawRectangleLinesEx(cell, 2, UI_GOOD);
    if (step->frames[frame] != REFSTR_EMPTY) {
        ui_text(cell.x + 6, cell.y + (cell.height - 13) / 2, 13, UI_TEXT, "%d", step->frames[frame]);
    }
}

static void draw_step_column(const refstring_lab_t *lab, const refstr_step_t *step, int index, float x, float top) {
    bool current = index == lab->cursor - 1;
    if (current) DrawRectangleRec((Rectangle){x, top, CELL_W, ROW_H * (lab->frames + 2)}, ColorAlpha(UI_ACCENT, 0.12f));

    ui_text(x + 7, top + 3, 13, current ? UI_ACCENT : UI_TEXT, "%d", step->page);
    for (int f = 0; f < lab->frames; f++) {
        Rectangle cell = {x + 1, top + (f + 1) * ROW_H + 1, CELL_W - 2, ROW_H - 2};
        draw_frame_cell(step, f, cell);
    }

    float fault_y = top + (lab->frames + 1) * ROW_H + 3;
    if (step->fault) ui_text(x + 9, fault_y, 13, UI_BAD, "F");
    else ui_text(x + 11, fault_y, 13, UI_TEXT_DIM, ".");

    if (CheckCollisionPointRec(GetMousePosition(), (Rectangle){x, top, CELL_W, ROW_H * (lab->frames + 2)})) {
        if (step->fault && step->victim != REFSTR_EMPTY) {
            ui_tooltip("reference %d: page %d\nFAULT: evicts page %d", index + 1, step->page, step->victim);
        } else if (step->fault) {
            ui_tooltip("reference %d: page %d\nFAULT: loaded into free frame %d", index + 1, step->page, step->frame);
        } else {
            ui_tooltip("reference %d: page %d\nhit in frame %d", index + 1, step->page, step->frame);
        }
    }
}

static void draw_policy_table(refstring_lab_t *lab, int policy, Rectangle area, float top, float label_w) {
    const refstr_result_t *result = &lab->results[policy];
    float x0 = area.x + 8;

    lab_legend_swatch(x0, top + 5, policy);
    ui_text(x0 + 18, top + 2, 15, UI_TEXT, "%s", repl_get_ops((repl_policy_t)policy)->name);
    ui_text(x0 + 130, top + 4, 13, UI_TEXT_DIM, "%d faults so far", faults_until(result, lab->cursor));
    top += TITLE_H;

    ui_text(x0, top + 3, 12, UI_TEXT_DIM, "ref");
    for (int f = 0; f < lab->frames; f++) ui_text(x0, top + (f + 1) * ROW_H + 3, 12, UI_TEXT_DIM, "frame %d", f);
    ui_text(x0, top + (lab->frames + 1) * ROW_H + 3, 12, UI_TEXT_DIM, "fault?");

    Rectangle columns = {x0 + label_w, top, area.width - label_w - 16, ROW_H * (lab->frames + 2)};
    ui_scissor_push(columns);
    for (int i = 0; i < lab->cursor && i < result->num_steps; i++) {
        float x = columns.x + i * CELL_W - lab->scroll_x;
        if (x + CELL_W < columns.x || x > columns.x + columns.width) continue;
        draw_step_column(lab, &result->steps[i], i, x, top);
    }
    ui_scissor_pop();

    Rectangle hover = {area.x, top - TITLE_H, area.width, TITLE_H + columns.height};
    if (CheckCollisionPointRec(GetMousePosition(), hover)) lab->hover_policy = policy;
}

static int last_use_before(const refstr_result_t *result, int step, int page) {
    for (int j = step - 1; j >= 0; j--) {
        if (result->steps[j].page == page) return j;
    }
    return -1;
}

static int last_load_before(const refstr_result_t *result, int step, int page) {
    for (int j = step - 1; j >= 0; j--) {
        if (result->steps[j].page == page && result->steps[j].fault) return j;
    }
    return -1;
}

static int next_use_after(const refstr_result_t *result, int step, int page) {
    for (int j = step + 1; j < result->num_steps; j++) {
        if (result->steps[j].page == page) return j;
    }
    return -1;
}

static void eviction_reason(const refstr_result_t *result, int policy, int step, char *out, size_t size) {
    int victim = result->steps[step].victim;
    switch ((repl_policy_t)policy) {
        case REPL_FIFO:
            snprintf(out, size, "evicts page %d, the one loaded longest ago (at reference %d)", victim,
                     last_load_before(result, step, victim) + 1);
            break;
        case REPL_LRU:
            snprintf(out, size, "evicts page %d, the least recently used (last used at reference %d)", victim,
                     last_use_before(result, step, victim) + 1);
            break;
        case REPL_CLOCK:
            snprintf(out, size, "evicts page %d: the clock hand found its reference bit at 0", victim);
            break;
        case REPL_OPT: {
            int next = next_use_after(result, step, victim);
            if (next < 0) snprintf(out, size, "evicts page %d, which is never used again", victim);
            else snprintf(out, size, "evicts page %d, not needed until reference %d (farthest away)", victim, next + 1);
            break;
        }
        default:
            snprintf(out, size, "evicts page %d", victim);
            break;
    }
}

static void describe_step(const refstr_result_t *result, int policy, int step, char *out, size_t size) {
    const refstr_step_t *s = &result->steps[step];
    if (!s->fault) {
        snprintf(out, size, "page %d is already in frame %d: hit", s->page, s->frame);
        return;
    }
    if (s->victim == REFSTR_EMPTY) {
        snprintf(out, size, "page %d faults and goes into free frame %d", s->page, s->frame);
        return;
    }
    char reason[140];
    eviction_reason(result, policy, step, reason, sizeof(reason));
    snprintf(out, size, "page %d faults; %s", s->page, reason);
}

static void draw_explanation(refstring_lab_t *lab, Rectangle r) {
    DrawRectangleRec(r, UI_PANEL_HI);
    int step = lab->cursor - 1;
    if (step < 0) {
        ui_text(r.x + 10, r.y + 8, 14, UI_TEXT_DIM, "Press > (or the Right key) to play the references one at a time.");
        return;
    }

    ui_text(r.x + 10, r.y + 6, 13, UI_ACCENT, "REFERENCE %d: PAGE %d", step + 1, lab->refs[step]);
    float y = r.y + 26;
    for (int p = 0; p < REPL_COUNT && y < r.y + r.height - 14; p++) {
        if (!lab->enabled[p]) continue;
        char line[220];
        describe_step(&lab->results[p], p, step, line, sizeof(line));
        lab_legend_swatch(r.x + 10, y + 2, p);
        ui_text(r.x + 26, y, 13, UI_TEXT_DIM, "%s:", repl_get_ops((repl_policy_t)p)->name);
        ui_text_clipped((Rectangle){r.x + 90, y, r.width - 100, 16}, 13, lab->results[p].steps[step].fault ? UI_TEXT : UI_GOOD, line);
        y += 17;
    }
}

static void clamp_scroll(refstring_lab_t *lab, float visible_w, float content_h, float visible_h) {
    float content_w = lab->cursor * CELL_W;
    float max_x = content_w > visible_w ? content_w - visible_w : 0;
    float max_y = content_h > visible_h ? content_h - visible_h : 0;
    if (lab->scroll_x > max_x) lab->scroll_x = max_x;
    if (lab->scroll_x < 0) lab->scroll_x = 0;
    if (lab->scroll_y > max_y) lab->scroll_y = max_y;
    if (lab->scroll_y < 0) lab->scroll_y = 0;
}

static void follow_cursor(refstring_lab_t *lab, float visible_w) {
    float cursor_x = lab->cursor * CELL_W;
    if (cursor_x - lab->scroll_x > visible_w) lab->scroll_x = cursor_x - visible_w;
    if (cursor_x - CELL_W < lab->scroll_x) lab->scroll_x = cursor_x - CELL_W;
}

static void draw_tables(refstring_lab_t *lab, Rectangle area) {
    const float label_w = 58;
    float visible_w = area.width - label_w - 24;
    int previous_cursor = lab->cursor;

    float content_h = 0;
    for (int p = 0; p < REPL_COUNT; p++) {
        if (lab->enabled[p]) content_h += table_height(lab) + TABLE_GAP;
    }

    if (CheckCollisionPointRec(GetMousePosition(), area)) {
        float wheel = GetMouseWheelMove();
        if (IsKeyDown(KEY_LEFT_SHIFT)) lab->scroll_x -= wheel * 3 * CELL_W;
        else lab->scroll_y -= wheel * 40;
    }

    lab_step_keys(&lab->cursor, lab->count);
    if (lab->cursor != previous_cursor) follow_cursor(lab, visible_w);
    clamp_scroll(lab, visible_w, content_h, area.height);

    ui_scissor_push(area);
    float top = area.y - lab->scroll_y;
    for (int p = 0; p < REPL_COUNT; p++) {
        if (!lab->enabled[p]) continue;
        if (top + table_height(lab) > area.y && top < area.y + area.height) draw_policy_table(lab, p, area, top, label_w);
        top += table_height(lab) + TABLE_GAP;
    }
    ui_scissor_pop();
}

static void draw_results(refstring_lab_t *lab, Rectangle r) {
    ui_claim_rect(r);
    ui_panel(r, NULL);
    if (lab->count == 0) {
        ui_text(r.x + 14, r.y + 14, 15, UI_TEXT_DIM, "Type a reference string on the left, e.g. 7 0 1 2 0 3 0 4");
        return;
    }

    draw_summary(lab, r.x + 14, r.y + 10);
    lab_step_controls(ID(50), r.x + 14, r.y + 36, &lab->cursor, lab->count, "references");

    Rectangle tables = {r.x, r.y + 70, r.width, r.height - 70 - EXPLAIN_H - 24};
    draw_tables(lab, tables);
    draw_explanation(lab, (Rectangle){r.x + 8, r.y + r.height - EXPLAIN_H - 22, r.width - 16, EXPLAIN_H});
    ui_text(r.x + 12, r.y + r.height - 18, 11, UI_TEXT_DIM,
            "red = page loaded on a fault  |  green box = hit  |  wheel scrolls policies, Shift+wheel scrolls references");
}

void lab_refstring_draw(gui_t *g, refstring_lab_t *lab, Rectangle r) {
    (void)g;
    if (lab->dirty) recompute(lab);
    lab->hover_policy = -1;

    Rectangle setup = {r.x, r.y, LAB_SETUP_W, r.height};
    Rectangle results = {r.x + LAB_SETUP_W, r.y, r.width - LAB_SETUP_W, r.height};
    draw_results(lab, results);
    draw_setup_panel(lab, setup);
}
