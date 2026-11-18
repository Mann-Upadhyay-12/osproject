#include "labs.h"
#include "compare.h"
#include <stdio.h>
#include <string.h>

#define ID(n)       LAB_ID(LAB_MEMORY, n)
#define BAR_H       34.0f
#define ROW_H       96.0f
#define TREND_H     26.0f
#define EXPLAIN_H   96.0f

typedef struct {
    const char *label;
    const char *script;
    int         memory_size;
} memory_preset_t;

static const memory_preset_t PRESETS[] = {
    {"Textbook",
     "h1=100 #1=10 h2=500 #2=10 h3=200 #3=10 h4=300 #4=10 h5=600 free h1 free h2 free h3 free h4 free h5 "
     "P1=212 P2=417 P3=112 P4=426",
     1740},
    {"Compaction", "A=20 B=10 C=20 D=10 E=20 F=10 free B free D free F G=25 compact G=25", 100},
    {"Churn", "A=300 B=150 C=200 D=100 free A E=120 F=90 free C G=180 H=60 free E I=250 J=140", 1000},
};
#define NUM_PRESETS ((int)(sizeof(PRESETS) / sizeof(PRESETS[0])))

static const algo_info_t MEMORY_INFO[CONTIG_COUNT] = {
    [CONTIG_FIRST_FIT] = {"First fit", "Use the first hole that is big enough.",
        "Scan memory from address 0 and place the request in the first hole whose size is at least the request; "
        "the rest of the hole stays free.",
        "Fast: stops at the first match. Usually as good as best fit in practice.",
        "Small leftover holes pile up near the start of memory.", "-"},
    [CONTIG_BEST_FIT] = {"Best fit", "Use the smallest hole that is big enough.",
        "Look at every hole and choose the one that leaves the smallest leftover (ties: lower address).",
        "Keeps big holes intact for big requests.",
        "Has to search every hole and leaves tiny, useless slivers behind.", "-"},
    [CONTIG_WORST_FIT] = {"Worst fit", "Use the largest hole.",
        "Always split the biggest hole, hoping the leftover is still large enough to be useful.",
        "Leftover holes stay usable for medium requests.",
        "Quickly destroys the large holes, so big requests fail first.", "-"},
    [CONTIG_NEXT_FIT] = {"Next fit", "First fit, but start where the last search stopped.",
        "Keep a roving pointer at the end of the last allocation and search forward from there, wrapping around "
        "to address 0 when the end of memory is reached.",
        "Spreads allocations across memory; searches are short on average.",
        "Breaks up the big hole at the end of memory sooner than first fit.", "-"},
};

static void load_preset(memory_lab_t *lab, const memory_preset_t *preset) {
    snprintf(lab->input, sizeof(lab->input), "%s", preset->script);
    lab->memory_size = preset->memory_size;
    lab->dirty = true;
}

void lab_memory_init(memory_lab_t *lab) {
    memset(lab, 0, sizeof(*lab));
    for (int p = 0; p < CONTIG_COUNT; p++) lab->enabled[p] = true;
    lab->hover_policy = -1;
    load_preset(lab, &PRESETS[0]);
}

static void fill_random(memory_lab_t *lab) {
    static uint32_t seed = 4242;
    char live[CONTIG_MAX_OPS][4];
    int num_live = 0, next_name = 0;
    size_t used = 0;
    lab->input[0] = '\0';
    lab->memory_size = 1000;

    for (int i = 0; i < 16; i++) {
        seed = seed * 1103515245u + 12345u;
        bool do_free = num_live > 2 && (seed >> 16) % 3 == 0;
        if (do_free) {
            int victim = (int)((seed >> 8) % (uint32_t)num_live);
            used += (size_t)snprintf(lab->input + used, sizeof(lab->input) - used, "%sfree %s", used ? " " : "", live[victim]);
            memcpy(live[victim], live[--num_live], sizeof(live[0]));
        } else {
            int size = 40 + (int)((seed >> 4) % 260);
            snprintf(live[num_live], sizeof(live[0]), "%c", 'A' + next_name++ % 26);
            used += (size_t)snprintf(lab->input + used, sizeof(lab->input) - used, "%s%s=%d", used ? " " : "", live[num_live], size);
            num_live++;
        }
    }
    lab->dirty = true;
}

static void recompute(memory_lab_t *lab) {
    lab->dirty = false;
    lab->valid = false;

    char bad[64];
    contig_status_t status = contig_parse_script(lab->input, lab->ops, CONTIG_MAX_OPS, &lab->count, bad, sizeof(bad));
    if (status == CONTIG_OK) {
        for (int p = 0; p < CONTIG_COUNT && status == CONTIG_OK; p++) {
            status = contig_run((contig_policy_t)p, lab->memory_size, lab->ops, lab->count, &lab->results[p]);
        }
    }
    if (status != CONTIG_OK) {
        snprintf(lab->message, sizeof(lab->message), bad[0] ? "\"%s\": %s" : "%s%s", bad, contig_strerror(status));
        return;
    }

    lab->valid = true;
    lab->cursor = lab->count;
    snprintf(lab->message, sizeof(lab->message), "%d operations on %d units of memory", lab->count, lab->memory_size);
}

static float draw_script_section(memory_lab_t *lab, float x, float y, float w) {
    lab_draw_section(x, &y, "1. REQUESTS");
    if (ui_text_field(ID(1), (Rectangle){x, y, w, 26}, lab->input, sizeof(lab->input))) lab->dirty = true;
    y += 30;
    ui_text_clipped((Rectangle){x, y, w, 16}, 12, lab->valid ? UI_TEXT_DIM : UI_BAD, lab->message);
    y += 20;
    lab_draw_hint(x, &y, w, "A=212 allocates 212 units for A, free A (or -A) releases it, compact slides everything down. "
                            "Names starting with # are reserved (e.g. the OS) and drawn grey.");

    float button_w = (w - 4) / 2;
    for (int i = 0; i <= NUM_PRESETS; i++) {
        Rectangle button = {x + (i % 2) * (button_w + 4), y + (i / 2) * 28, button_w, 24};
        if (i == NUM_PRESETS) {
            if (ui_button(ID(19), button, "Random")) fill_random(lab);
        } else if (ui_button(ID(10 + i), button, PRESETS[i].label)) {
            load_preset(lab, &PRESETS[i]);
        }
    }
    return y + ((NUM_PRESETS + 2) / 2) * 28 + 8;
}

static float draw_size_section(memory_lab_t *lab, float x, float y) {
    lab_draw_section(x, &y, "2. MEMORY SIZE (units)");
    if (ui_int_field(ID(20), (Rectangle){x, y, 160, 26}, &lab->memory_size, 1, CONTIG_MAX_MEMORY)) lab->dirty = true;
    return y + 38;
}

static float draw_policy_section(memory_lab_t *lab, float x, float y, float w) {
    lab_draw_section(x, &y, "3. POLICIES");
    for (int p = 0; p < CONTIG_COUNT; p++) {
        lab_policy_toggle(ID(30 + p), x, &y, w, p, contig_get_ops((contig_policy_t)p)->name, &lab->enabled[p], &lab->hover_policy);
    }
    return y + 10;
}

static void draw_setup_panel(memory_lab_t *lab, Rectangle r) {
    ui_claim_rect(r);
    ui_panel(r, NULL);
    float x = r.x + 12, y = r.y + 10, w = r.width - 24;

    y = draw_script_section(lab, x, y, w);
    y = draw_size_section(lab, x, y);
    y = draw_policy_section(lab, x, y, w);

    const algo_info_t *info = lab->hover_policy >= 0 ? &MEMORY_INFO[lab->hover_policy] : NULL;
    lab_card(r, x, y, w, info,
             "Hover a policy to see how it picks a hole. Each bar is physical memory after the selected step: "
             "coloured blocks are allocated, dark gaps are holes.");
}

static Color block_color(const char *name) {
    if (contig_is_reserved(name)) return (Color){96, 100, 110, 255};
    unsigned hash = 5381;
    for (const char *c = name; *c; c++) hash = hash * 33u + (unsigned char)*c;
    return proc_color((int)(hash % 64));
}

static const contig_step_t *step_at(const memory_lab_t *lab, int policy) {
    return lab->cursor > 0 ? &lab->results[policy].steps[lab->cursor - 1] : NULL;
}

static bool is_changed_block(const contig_step_t *step, const contig_block_t *block) {
    if (!step || !step->ok) return false;
    if (step->op.kind == CONTIG_OP_ALLOC) return block->used && strcmp(block->name, step->op.name) == 0;
    if (step->op.kind == CONTIG_OP_FREE) return !block->used && block->start <= step->address && step->address < block->start + block->size;
    return false;
}

static void draw_block(const contig_block_t *block, Rectangle rect, bool changed) {
    if (block->used) {
        DrawRectangleRec(rect, ColorAlpha(block_color(block->name), 0.85f));
        float text_w = ui_text_width(block->name, 13);
        if (rect.width > text_w + 6) {
            ui_text(rect.x + (rect.width - text_w) / 2, rect.y + 3, 13, (Color){15, 15, 20, 255}, "%s", block->name);
        }
    } else {
        DrawRectangleRec(rect, (Color){24, 26, 31, 255});
    }

    const char *size = TextFormat("%d", block->size);
    float size_w = ui_text_width(size, 11);
    if (rect.width > size_w + 6) {
        ui_text(rect.x + (rect.width - size_w) / 2, rect.y + rect.height - 14, 11, block->used ? (Color){25, 25, 30, 255} : UI_TEXT_DIM, "%s", size);
    }
    DrawRectangleLinesEx(rect, 1, (Color){15, 15, 20, 255});
    if (changed) DrawRectangleLinesEx((Rectangle){rect.x - 1, rect.y - 1, rect.width + 2, rect.height + 2}, 2, UI_TEXT);

    if (CheckCollisionPointRec(GetMousePosition(), rect)) {
        if (block->used) ui_tooltip("%s: %d units at %d..%d", block->name, block->size, block->start, block->start + block->size - 1);
        else ui_tooltip("hole: %d units at %d..%d", block->size, block->start, block->start + block->size - 1);
    }
}

static void draw_memory_bar(const memory_lab_t *lab, const contig_step_t *step, Rectangle bar) {
    DrawRectangleRec(bar, (Color){24, 26, 31, 255});
    if (!step) {
        ui_text(bar.x + 8, bar.y + 9, 13, UI_TEXT_DIM, "empty memory (%d units)", lab->memory_size);
        return;
    }

    float scale = bar.width / (float)lab->memory_size;
    for (int i = 0; i < step->memory.num_blocks; i++) {
        const contig_block_t *block = &step->memory.blocks[i];
        Rectangle rect = {bar.x + block->start * scale, bar.y, block->size * scale, bar.height};
        draw_block(block, rect, is_changed_block(step, block));
    }
    ui_text(bar.x - 2, bar.y + bar.height + 2, 10, UI_TEXT_DIM, "0");
    const char *end = TextFormat("%d", lab->memory_size);
    ui_text(bar.x + bar.width - ui_text_width(end, 10), bar.y + bar.height + 2, 10, UI_TEXT_DIM, "%s", end);
}

static void draw_fragmentation_trend(const memory_lab_t *lab, int policy, Rectangle area) {
    const contig_result_t *result = &lab->results[policy];
    DrawRectangleRec(area, (Color){28, 31, 38, 255});
    if (result->num_steps < 2) return;

    float step_w = area.width / (float)(result->num_steps - 1);
    Color color = cmp_alg_color(policy);
    for (int i = 1; i < result->num_steps && i < lab->cursor; i++) {
        float y0 = area.y + area.height * (1.0f - (float)result->steps[i - 1].stats.external_fragmentation);
        float y1 = area.y + area.height * (1.0f - (float)result->steps[i].stats.external_fragmentation);
        DrawLineEx((Vector2){area.x + (i - 1) * step_w, y0}, (Vector2){area.x + i * step_w, y1}, 1.5f, color);
    }
    for (int i = 0; i < result->num_steps && i < lab->cursor; i++) {
        if (result->steps[i].op.kind == CONTIG_OP_ALLOC && !result->steps[i].ok) {
            DrawCircleV((Vector2){area.x + i * step_w, area.y + area.height / 2}, 3, UI_BAD);
        }
    }
}

static int failed_until(const contig_result_t *result, int cursor) {
    int failed = 0;
    for (int i = 0; i < cursor && i < result->num_steps; i++) {
        failed += result->steps[i].op.kind == CONTIG_OP_ALLOC && !result->steps[i].ok;
    }
    return failed;
}

static void draw_policy_row(memory_lab_t *lab, int policy, Rectangle row) {
    const contig_step_t *step = step_at(lab, policy);
    if (CheckCollisionPointRec(GetMousePosition(), row)) lab->hover_policy = policy;

    lab_legend_swatch(row.x, row.y + 4, policy);
    ui_text(row.x + 18, row.y, 15, UI_TEXT, "%s", contig_get_ops((contig_policy_t)policy)->name);

    if (step) {
        const contig_stats_t *st = &step->stats;
        const char *stats = TextFormat("free %d in %d hole%s, largest %d, external fragmentation %.0f%%, %d failed",
                                       st->free_total, st->num_holes, st->num_holes == 1 ? "" : "s", st->largest_hole,
                                       st->external_fragmentation * 100.0, failed_until(&lab->results[policy], lab->cursor));
        ui_text(row.x + 120, row.y + 2, 13, UI_TEXT_DIM, "%s", stats);
    }

    Rectangle bar = {row.x, row.y + 22, row.width - 140, BAR_H};
    draw_memory_bar(lab, step, bar);
    Rectangle trend = {row.x + row.width - 128, row.y + 22, 128, TREND_H};
    draw_fragmentation_trend(lab, policy, trend);
    ui_text(trend.x, trend.y + trend.height + 2, 10, UI_TEXT_DIM, "fragmentation trend");
}

static int rover_before(const contig_result_t *result, int step) {
    int rover = 0;
    for (int i = 0; i < step; i++) {
        const contig_step_t *s = &result->steps[i];
        if (s->op.kind == CONTIG_OP_COMPACT) rover = 0;
        else if (s->op.kind == CONTIG_OP_ALLOC && s->ok) rover = s->address + s->op.size;
    }
    return rover;
}

static void describe_alloc(const memory_lab_t *lab, int policy, int index, char *out, size_t size) {
    const contig_result_t *result = &lab->results[policy];
    const contig_step_t *step = &result->steps[index];
    const contig_op_t *op = &step->op;

    if (!step->ok) {
        const contig_stats_t *before = index > 0 ? &result->steps[index - 1].stats : &step->stats;
        if (before->free_total >= op->size) {
            snprintf(out, size, "%s (%d) does not fit: %d units are free in total, but the largest hole is only %d "
                     "(external fragmentation)", op->name, op->size, before->free_total, before->largest_hole);
        } else {
            snprintf(out, size, "%s (%d) does not fit: only %d units are free", op->name, op->size, before->free_total);
        }
        return;
    }

    const char *why;
    switch ((contig_policy_t)policy) {
        case CONTIG_FIRST_FIT: why = "the first hole that fits"; break;
        case CONTIG_BEST_FIT:  why = "the smallest hole that fits"; break;
        case CONTIG_WORST_FIT: why = "the largest hole"; break;
        case CONTIG_NEXT_FIT:  why = TextFormat("the first hole that fits after the rover at %d", rover_before(result, index)); break;
        default:               why = "chosen by the policy"; break;
    }
    snprintf(out, size, "%s (%d) goes into the hole of %d at address %d, %s; %d units stay free there", op->name,
             op->size, step->hole_size, step->address, why, step->hole_size - op->size);
}

static void describe_step(const memory_lab_t *lab, int policy, int index, char *out, size_t size) {
    const contig_step_t *step = &lab->results[policy].steps[index];
    switch (step->op.kind) {
        case CONTIG_OP_ALLOC:
            describe_alloc(lab, policy, index, out, size);
            break;
        case CONTIG_OP_FREE:
            if (step->ok) snprintf(out, size, "%s is freed at address %d; touching holes merge into one", step->op.name, step->address);
            else snprintf(out, size, "%s is not in memory, nothing to free", step->op.name);
            break;
        case CONTIG_OP_COMPACT:
            snprintf(out, size, "compaction moves %d units so that all free memory forms one hole of %d", step->moved,
                     step->stats.largest_hole);
            break;
    }
}

static void draw_explanation(memory_lab_t *lab, Rectangle r) {
    DrawRectangleRec(r, UI_PANEL_HI);
    if (lab->cursor <= 0) {
        ui_text(r.x + 10, r.y + 8, 14, UI_TEXT_DIM, "Press > (or the Right key) to apply the requests one at a time.");
        return;
    }

    ui_text(r.x + 10, r.y + 6, 13, UI_ACCENT, "STEP %d", lab->cursor);
    float y = r.y + 26;
    for (int p = 0; p < CONTIG_COUNT && y < r.y + r.height - 14; p++) {
        if (!lab->enabled[p]) continue;
        char line[260];
        describe_step(lab, p, lab->cursor - 1, line, sizeof(line));
        const contig_step_t *step = &lab->results[p].steps[lab->cursor - 1];
        lab_legend_swatch(r.x + 10, y + 2, p);
        ui_text(r.x + 26, y, 13, UI_TEXT_DIM, "%s:", contig_get_ops((contig_policy_t)p)->name);
        ui_text_clipped((Rectangle){r.x + 104, y, r.width - 114, 16}, 13, step->ok ? UI_TEXT : UI_BAD, line);
        y += 17;
    }
}

static void draw_results(memory_lab_t *lab, Rectangle r) {
    ui_claim_rect(r);
    ui_panel(r, NULL);
    if (!lab->valid) {
        ui_text(r.x + 14, r.y + 14, 15, UI_BAD, "%s", lab->message);
        return;
    }

    lab_step_controls(ID(50), r.x + 14, r.y + 10, &lab->cursor, lab->count, "steps");
    lab_step_keys(&lab->cursor, lab->count);
    if (lab->cursor > 0) {
        const contig_op_t *op = &lab->ops[lab->cursor - 1];
        const char *text = op->kind == CONTIG_OP_ALLOC ? TextFormat("%s = %d", op->name, op->size)
                         : op->kind == CONTIG_OP_FREE  ? TextFormat("free %s", op->name) : "compact";
        ui_text(r.x + r.width - ui_text_width(text, 16) - 16, r.y + 12, 16, UI_ACCENT, "%s", text);
    }

    float y = r.y + 50;
    for (int p = 0; p < CONTIG_COUNT; p++) {
        if (!lab->enabled[p]) continue;
        draw_policy_row(lab, p, (Rectangle){r.x + 14, y, r.width - 28, ROW_H});
        y += ROW_H;
    }

    draw_explanation(lab, (Rectangle){r.x + 8, r.y + r.height - EXPLAIN_H - 22, r.width - 16, EXPLAIN_H});
    ui_text(r.x + 12, r.y + r.height - 18, 11, UI_TEXT_DIM,
            "white outline = block changed by this step  |  red dot = failed request  |  hover a block for its address range");
}

void lab_memory_draw(gui_t *g, memory_lab_t *lab, Rectangle r) {
    (void)g;
    if (lab->dirty) recompute(lab);
    lab->hover_policy = -1;

    draw_results(lab, (Rectangle){r.x + LAB_SETUP_W, r.y, r.width - LAB_SETUP_W, r.height});
    draw_setup_panel(lab, (Rectangle){r.x, r.y, LAB_SETUP_W, r.height});
}
