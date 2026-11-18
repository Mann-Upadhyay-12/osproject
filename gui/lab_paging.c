#include "labs.h"
#include "compare.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ID(n)        LAB_ID(LAB_PAGING, n)
#define FIELD_X      150.0f
#define FIELD_W      126.0f
#define ROW_STEP     30.0f
#define BIT_BAR_H    30.0f
#define WALK_H       112.0f
#define TABLE_ROW_H  19.0f
#define OFFSET_COLOR (Color){150, 156, 170, 255}

typedef struct {
    const char     *label;
    const char     *addresses;
    paging_config_t config;
} paging_preset_t;

static const paging_preset_t PRESETS[] = {
    {"32-bit, 2 levels", "0x00000000 0x00001004 0x00001ABC 0xFFFFF000 0x00400123 0x00000010 0x00402FFF 0xFFFFF7F0",
     {32, 12, 2, {10, 10, 0, 0}, 4, 32, 4}},
    {"32-bit, flat", "0x00000000 0x00001004 0x00001ABC 0xFFFFF000 0x00400123 0x00000010 0x00402FFF 0xFFFFF7F0",
     {32, 12, 1, {20, 0, 0, 0}, 4, 32, 4}},
    {"x86-64, 4 levels", "0x7FFFFFFFE000 0x7FFFFFFFD123 0x400000 0x401ABC 0x600000 0x7FFFFFFFE010",
     {48, 12, 4, {9, 9, 9, 9}, 8, 52, 4}},
    {"16-bit toy", "0x1234 0x12FF 0xABCD 0x1200 0x0042 0xAB00", {16, 8, 2, {4, 4, 0, 0}, 2, 16, 2}},
};
#define NUM_PRESETS ((int)(sizeof(PRESETS) / sizeof(PRESETS[0])))

static void write_split_text(paging_lab_t *lab) {
    size_t used = 0;
    lab->split[0] = '\0';
    for (int l = 0; l < lab->config.levels; l++) {
        used += (size_t)snprintf(lab->split + used, sizeof(lab->split) - used, l ? " %d" : "%d", lab->config.level_bits[l]);
    }
}

static bool read_split_text(paging_lab_t *lab) {
    int bits[PAGING_MAX_LEVELS] = {0};
    int count = 0;
    const char *p = lab->split;
    while (*p) {
        char *end;
        long value = strtol(p, &end, 10);
        if (end == p) {
            p++;
            continue;
        }
        if (count == PAGING_MAX_LEVELS) return false;
        bits[count++] = (int)value;
        p = end;
    }
    if (count != lab->config.levels) return false;
    memcpy(lab->config.level_bits, bits, sizeof(bits));
    return true;
}

static void load_preset(paging_lab_t *lab, const paging_preset_t *preset) {
    snprintf(lab->input, sizeof(lab->input), "%s", preset->addresses);
    lab->config = preset->config;
    write_split_text(lab);
    lab->dirty = true;
}

void lab_paging_init(paging_lab_t *lab) {
    memset(lab, 0, sizeof(*lab));
    lab->process_bytes = 72766;
    lab->tlb_ns = 2;
    lab->memory_ns = 100;
    load_preset(lab, &PRESETS[0]);
}

static void recompute(paging_lab_t *lab) {
    lab->dirty = false;
    lab->valid = false;

    char bad[64] = "";
    if (!read_split_text(lab)) {
        snprintf(lab->message, sizeof(lab->message), "bits per level: give %d number%s, e.g. \"10 10\"", lab->config.levels,
                 lab->config.levels == 1 ? "" : "s");
        return;
    }
    paging_status_t status = paging_validate(&lab->config);
    if (status == PAGING_OK) {
        status = paging_parse_addresses(lab->input, &lab->config, lab->addresses, PAGING_MAX_ADDRESSES, &lab->count, bad,
                                        sizeof(bad));
    }
    if (status == PAGING_OK) status = paging_translate_all(&lab->config, lab->addresses, lab->count, &lab->result);
    if (status != PAGING_OK) {
        snprintf(lab->message, sizeof(lab->message), bad[0] ? "\"%s\": %s" : "%s%s", bad, paging_strerror(status));
        return;
    }

    lab->valid = true;
    if (lab->cursor < 1 || lab->cursor > lab->count) lab->cursor = 1;
    snprintf(lab->message, sizeof(lab->message), "%d virtual addresses", lab->count);
}

static const char *bytes_text(uint64_t bytes) {
    static const char *UNITS[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    double value = (double)bytes;
    int unit = 0;
    while (value >= 1024.0 && unit < 5) {
        value /= 1024.0;
        unit++;
    }
    bool whole = value == (double)(uint64_t)value;
    return whole ? TextFormat("%.0f %s", value, UNITS[unit]) : TextFormat("%.1f %s", value, UNITS[unit]);
}

static const char *hex_address(uint64_t value, int bits) {
    return TextFormat("0x%0*llX", (bits + 3) / 4, (unsigned long long)value);
}

static Color level_color(int level) {
    return cmp_alg_color(level);
}

static bool config_row(int id, float x, float *y, const char *label, int *value, int min, int max) {
    ui_text(x, *y + 5, 13, UI_TEXT_DIM, "%s", label);
    bool changed = ui_int_field(id, (Rectangle){x + FIELD_X, *y, FIELD_W, 26}, value, min, max);
    *y += ROW_STEP;
    return changed;
}

static float draw_address_section(paging_lab_t *lab, float x, float y, float w) {
    lab_draw_section(x, &y, "1. VIRTUAL ADDRESSES (decimal or 0x hex)");
    if (ui_text_field(ID(1), (Rectangle){x, y, w, 26}, lab->input, sizeof(lab->input))) lab->dirty = true;
    y += 30;
    ui_text_clipped((Rectangle){x, y, w, 16}, 12, lab->valid ? UI_TEXT_DIM : UI_BAD, lab->message);
    y += 22;

    float button_w = (w - 4) / 2;
    for (int i = 0; i < NUM_PRESETS; i++) {
        Rectangle button = {x + (i % 2) * (button_w + 4), y + (i / 2) * 28, button_w, 24};
        if (ui_button(ID(10 + i), button, PRESETS[i].label)) load_preset(lab, &PRESETS[i]);
    }
    return y + ((NUM_PRESETS + 1) / 2) * 28 + 10;
}

static float draw_split_row(paging_lab_t *lab, float x, float y, float w) {
    ui_text(x, y + 5, 13, UI_TEXT_DIM, "bits per level");
    float field_w = w - FIELD_X - 54;
    if (ui_text_field(ID(40), (Rectangle){x + FIELD_X, y, field_w, 26}, lab->split, sizeof(lab->split))) lab->dirty = true;
    if (ui_button(ID(41), (Rectangle){x + FIELD_X + field_w + 4, y, 50, 26}, "even")) {
        paging_split_evenly(&lab->config);
        write_split_text(lab);
        lab->dirty = true;
    }
    return y + ROW_STEP + 4;
}

static float draw_config_section(paging_lab_t *lab, float x, float y, float w) {
    lab_draw_section(x, &y, "2. ADDRESS SPACE");
    paging_config_t *c = &lab->config;
    bool reshaped = false;
    reshaped |= config_row(ID(20), x, &y, "virtual address bits", &c->address_bits, PAGING_MIN_ADDR_BITS, PAGING_MAX_ADDR_BITS);
    reshaped |= config_row(ID(21), x, &y, TextFormat("offset bits (%s)", bytes_text(1ull << c->offset_bits)), &c->offset_bits,
                           1, PAGING_MAX_ADDR_BITS - 1);
    reshaped |= config_row(ID(22), x, &y, "page-table levels", &c->levels, 1, PAGING_MAX_LEVELS);
    if (reshaped) {
        paging_split_evenly(c);
        write_split_text(lab);
        lab->dirty = true;
    }
    y = draw_split_row(lab, x, y, w);

    lab_draw_section(x, &y, "3. HARDWARE");
    bool changed = false;
    changed |= config_row(ID(23), x, &y, "bytes per table entry", &c->pte_bytes, 1, 16);
    changed |= config_row(ID(24), x, &y, "physical address bits", &c->physical_bits, 8, PAGING_MAX_PHYS_BITS);
    changed |= config_row(ID(25), x, &y, "TLB entries (0 = none)", &c->tlb_entries, 0, PAGING_MAX_TLB);
    config_row(ID(26), x, &y, "TLB lookup (ns)", &lab->tlb_ns, 0, 1000);
    config_row(ID(27), x, &y, "memory access (ns)", &lab->memory_ns, 1, 10000);
    if (changed) lab->dirty = true;
    y += 6;

    lab_draw_section(x, &y, "4. PROCESS SIZE (bytes)");
    ui_int_field(ID(28), (Rectangle){x, y, FIELD_W + FIELD_X - 24, 26}, &lab->process_bytes, 1, 2000000000);
    return y + 36;
}

static void draw_setup_panel(paging_lab_t *lab, Rectangle r) {
    ui_claim_rect(r);
    ui_panel(r, NULL);
    float x = r.x + 12, y = r.y + 10, w = r.width - 24;
    y = draw_address_section(lab, x, y, w);
    y = draw_config_section(lab, x, y, w);
    lab_card(r, x, y, w, NULL,
             "A virtual address is cut into one index per page-table level plus the offset inside the page. "
             "Tables are created only when a page in their range is first touched.");
}

static const paging_step_t *selected_step(const paging_lab_t *lab) {
    return lab->cursor > 0 ? &lab->result.steps[lab->cursor - 1] : NULL;
}

static void draw_bit_segment(Rectangle cell_area, int first_bit, int bits, float bit_w, uint64_t vaddr, int address_bits,
                             Color color, const char *title, const char *value) {
    float x = cell_area.x + first_bit * bit_w;
    Rectangle segment = {x, cell_area.y, bits * bit_w, cell_area.height};
    DrawRectangleRec(segment, ColorAlpha(color, 0.25f));

    for (int b = 0; b < bits; b++) {
        int bit_index = address_bits - 1 - (first_bit + b);
        int bit = (int)((vaddr >> bit_index) & 1u);
        float bx = x + b * bit_w;
        if (bit_w >= 9) ui_text(bx + bit_w / 2 - 3, segment.y + 8, 13, bit ? UI_TEXT : UI_TEXT_DIM, "%d", bit);
        if (b > 0) DrawLine((int)bx, (int)segment.y + 6, (int)bx, (int)(segment.y + segment.height - 6), ColorAlpha(color, 0.3f));
    }
    DrawRectangleLinesEx(segment, 1.5f, color);
    ui_text_clipped((Rectangle){x + 2, segment.y - 17, segment.width - 4, 16}, 12, color, title);
    ui_text_clipped((Rectangle){x + 2, segment.y + segment.height + 3, segment.width - 4, 16}, 12, UI_TEXT, value);
}

static float draw_bit_split(const paging_lab_t *lab, const paging_step_t *step, float x, float y, float w) {
    const paging_config_t *c = &lab->result.config;
    ui_text(x, y, 13, UI_ACCENT, "ADDRESS SPLIT  %s", hex_address(step->vaddr, c->address_bits));
    y += 38;

    float bit_w = w / (float)c->address_bits;
    Rectangle area = {x, y, w, BIT_BAR_H};
    int first_bit = 0;
    for (int l = 0; l < c->levels; l++) {
        const char *title = TextFormat("%s (%d bits)", c->levels == 1 ? "page number" : TextFormat("p%d", l + 1), c->level_bits[l]);
        const char *value = TextFormat("index %llu (0x%llX)", (unsigned long long)step->index[l], (unsigned long long)step->index[l]);
        draw_bit_segment(area, first_bit, c->level_bits[l], bit_w, step->vaddr, c->address_bits, level_color(l), title, value);
        first_bit += c->level_bits[l];
    }
    draw_bit_segment(area, first_bit, c->offset_bits, bit_w, step->vaddr, c->address_bits, OFFSET_COLOR,
                     TextFormat("offset (%d bits)", c->offset_bits), TextFormat("%llu (0x%llX)", (unsigned long long)step->offset,
                     (unsigned long long)step->offset));
    return y + BIT_BAR_H + 30;
}

static void draw_arrow(float x0, float x1, float y, Color color) {
    DrawLineEx((Vector2){x0, y}, (Vector2){x1 - 6, y}, 2, color);
    DrawTriangle((Vector2){x1, y}, (Vector2){x1 - 8, y - 5}, (Vector2){x1 - 8, y + 5}, color);
}

static void draw_walk_box(Rectangle box, Color edge, bool dimmed, const char *title, const char *line1, const char *line2,
                          Color line2_color) {
    DrawRectangleRec(box, dimmed ? (Color){30, 33, 40, 255} : UI_PANEL_HI);
    DrawRectangleLinesEx(box, 1.5f, dimmed ? UI_BORDER : edge);
    Rectangle inner = {box.x + 8, box.y, box.width - 16, 18};
    inner.y = box.y + 6;
    ui_text_clipped(inner, 13, dimmed ? UI_TEXT_DIM : edge, title);
    inner.y += 20;
    ui_text_clipped(inner, 13, dimmed ? UI_TEXT_DIM : UI_TEXT, line1);
    inner.y += 18;
    ui_text_clipped(inner, 12, dimmed ? UI_TEXT_DIM : line2_color, line2);
}

static const char *table_status(const paging_step_t *step, int level) {
    if (step->tlb_hit) return "skipped (TLB hit)";
    if (level > 0 && step->table_created[level]) return "table created now";
    return "already allocated";
}

static float draw_walk(const paging_lab_t *lab, const paging_step_t *step, float x, float y, float w) {
    const paging_config_t *c = &lab->result.config;
    if (c->tlb_entries == 0) {
        ui_text(x, y, 13, UI_ACCENT, "PAGE-TABLE WALK  (no TLB: every reference walks the tables)");
    } else if (step->tlb_hit) {
        ui_text(x, y, 13, UI_GOOD, "TLB HIT  page 0x%llX is cached: frame %llu, 1 memory access", (unsigned long long)step->vpn,
                (unsigned long long)step->frame);
    } else if (step->tlb_victim_slot >= 0) {
        ui_text(x, y, 13, UI_WARN, "TLB MISS  walk %d table%s, then cache page 0x%llX (evicts least recently used page 0x%llX)",
                c->levels, c->levels == 1 ? "" : "s", (unsigned long long)step->vpn, (unsigned long long)step->tlb_victim_vpn);
    } else {
        ui_text(x, y, 13, UI_WARN, "TLB MISS  walk %d table%s, then cache page 0x%llX", c->levels, c->levels == 1 ? "" : "s",
                (unsigned long long)step->vpn);
    }
    y += 24;

    int boxes = c->levels + 2;
    float gap = 28, box_w = (w - gap * (boxes - 1)) / boxes;
    float mid = y + WALK_H / 2 - 20;
    Rectangle box = {x, y, box_w, 66};

    draw_walk_box(box, UI_ACCENT, step->tlb_hit, "base register", "points to the", "level-1 table #0", UI_TEXT_DIM);
    for (int l = 0; l < c->levels; l++) {
        draw_arrow(box.x + box.width, box.x + box.width + gap, mid, step->tlb_hit ? UI_BORDER : UI_TEXT_DIM);
        box.x += box_w + gap;
        bool last = l == c->levels - 1;
        const char *entry = last ? TextFormat("entry %llu -> frame %llu", (unsigned long long)step->index[l], (unsigned long long)step->frame)
                                 : TextFormat("entry %llu -> table #%d", (unsigned long long)step->index[l], step->tlb_hit ? 0 : step->table_id[l + 1]);
        if (step->tlb_hit && !last) entry = TextFormat("entry %llu", (unsigned long long)step->index[l]);
        bool created = !step->tlb_hit && l > 0 && step->table_created[l];
        draw_walk_box(box, level_color(l), step->tlb_hit, TextFormat("level %d table #%d", l + 1, step->tlb_hit ? 0 : step->table_id[l]), entry,
                      table_status(step, l), created ? UI_WARN : UI_TEXT_DIM);
    }
    draw_arrow(box.x + box.width, box.x + box.width + gap, mid, step->tlb_hit ? UI_GOOD : UI_TEXT_DIM);
    box.x += box_w + gap;
    const char *frame_note = step->page_created ? "page first touched: frame assigned" : "frame number + offset";
    draw_walk_box(box, UI_GOOD, false, "physical address", hex_address(step->paddr, c->physical_bits), frame_note,
                  step->page_created ? UI_WARN : UI_TEXT_DIM);
    return y + 66 + 22;
}

static void size_line(float x, float *y, float w, Color color, const char *text) {
    ui_text_clipped((Rectangle){x, *y, w, 16}, 13, color, text);
    *y += 18;
}

static void draw_sizes(const paging_lab_t *lab, Rectangle r) {
    const paging_result_t *res = &lab->result;
    const paging_config_t *c = &res->config;
    const paging_sizes_t *s = &res->sizes;
    float x = r.x + 10, y = r.y + 6, w = r.width - 20;
    DrawRectangleRec(r, UI_PANEL_HI);
    ui_text(x, y, 13, UI_ACCENT, "SIZES");
    y += 22;

    size_line(x, &y, w, UI_TEXT, TextFormat("page size %s, %llu virtual pages, %llu physical frames", bytes_text(s->page_size),
              (unsigned long long)s->num_pages, (unsigned long long)s->num_frames));
    for (int l = 0; l < c->levels; l++) {
        bool fits = s->table_bytes[l] <= s->page_size;
        const char *pages = fits ? "fits in one page" : TextFormat("spans %llu pages", (unsigned long long)(s->table_bytes[l] / s->page_size));
        size_line(x, &y, w, level_color(l), TextFormat("level %d: %llu entries x %d B = %s per table (%s)", l + 1,
                  (unsigned long long)s->entries[l], c->pte_bytes, bytes_text(s->table_bytes[l]), pages));
    }
    size_line(x, &y, w, UI_TEXT, TextFormat("one flat table for the whole space: %s per process", bytes_text(s->flat_table_bytes)));
    if (c->levels > 1) {
        size_line(x, &y, w, UI_TEXT_DIM, TextFormat("all tables if every page were mapped: %s", bytes_text(s->full_tree_bytes)));
    }
    int tables = 0;
    for (int l = 0; l < c->levels; l++) tables += res->tables_per_level[l];
    size_line(x, &y, w, UI_GOOD, TextFormat("these %d addresses touch %d pages: %d table%s, %s in use", res->num_steps, res->pages_mapped,
              tables, tables == 1 ? "" : "s", bytes_text(res->table_bytes_used)));
    y += 6;

    uint64_t pages = paging_pages_needed(c, (uint64_t)lab->process_bytes);
    uint64_t waste = paging_internal_fragmentation(c, (uint64_t)lab->process_bytes);
    size_line(x, &y, w, UI_TEXT, TextFormat("a %d-byte process needs %llu pages; internal fragmentation %llu bytes", lab->process_bytes,
              (unsigned long long)pages, (unsigned long long)waste));

    if (c->tlb_entries > 0) {
        double hit_ratio = (double)res->tlb_hits / res->num_steps;
        double measured = paging_effective_access_time(c->levels, hit_ratio, lab->tlb_ns, lab->memory_ns);
        size_line(x, &y, w, UI_TEXT, TextFormat("TLB hits %d of %d (%.0f%%); memory accesses %d", res->tlb_hits, res->num_steps,
                  hit_ratio * 100.0, res->memory_accesses));
        size_line(x, &y, w, UI_TEXT, TextFormat("effective access time = %d + %d + (1 - %.2f) x %d x %d = %.1f ns", lab->tlb_ns,
                  lab->memory_ns, hit_ratio, c->levels, lab->memory_ns, measured));
    } else {
        size_line(x, &y, w, UI_TEXT, TextFormat("no TLB: every access costs %d memory accesses = %d ns", c->levels + 1,
                  (c->levels + 1) * lab->memory_ns));
    }
}

static void draw_translation_row(paging_lab_t *lab, int i, Rectangle row) {
    const paging_config_t *c = &lab->result.config;
    const paging_step_t *step = &lab->result.steps[i];
    bool selected = i == lab->cursor - 1;
    bool hovered = CheckCollisionPointRec(GetMousePosition(), row);
    if (selected) DrawRectangleRec(row, ColorAlpha(UI_ACCENT, 0.18f));
    else if (hovered) DrawRectangleRec(row, ColorAlpha(UI_TEXT, 0.05f));
    if (hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && ui_content_input()) lab->cursor = i + 1;

    float x = row.x + 6;
    ui_text(x, row.y + 2, 12, UI_TEXT_DIM, "%d", i + 1);
    ui_text(x + 26, row.y + 2, 12, UI_TEXT, "%s", hex_address(step->vaddr, c->address_bits));
    char indexes[64] = "";
    for (int l = 0; l < c->levels; l++) {
        size_t used = strlen(indexes);
        snprintf(indexes + used, sizeof(indexes) - used, l ? " / %llu" : "%llu", (unsigned long long)step->index[l]);
    }
    ui_text_clipped((Rectangle){x + 140, row.y + 2, 110, 16}, 12, UI_TEXT_DIM, indexes);
    const char *tlb = c->tlb_entries == 0 ? "-" : step->tlb_hit ? "hit" : "miss";
    ui_text(x + 256, row.y + 2, 12, step->tlb_hit ? UI_GOOD : UI_TEXT_DIM, "%s", tlb);
    ui_text(x + 300, row.y + 2, 12, UI_TEXT, "%llu", (unsigned long long)step->frame);
    ui_text(x + 350, row.y + 2, 12, UI_TEXT, "%s", hex_address(step->paddr, c->physical_bits));
}

static void draw_translations(paging_lab_t *lab, Rectangle r) {
    DrawRectangleRec(r, UI_PANEL_HI);
    float x = r.x + 6, y = r.y + 6;
    ui_text(x, y, 13, UI_ACCENT, "TRANSLATIONS (click a row)");
    y += 22;
    const char *headers[] = {"#", "virtual", "indexes", "TLB", "frame", "physical"};
    const float columns[] = {0, 26, 140, 256, 300, 350};
    for (int i = 0; i < 6; i++) ui_text(x + 6 + columns[i], y, 11, UI_TEXT_DIM, "%s", headers[i]);
    y += 16;

    int visible = (int)((r.y + r.height - y - 4) / TABLE_ROW_H);
    int first = lab->cursor - visible;
    if (first < 0) first = 0;
    for (int i = first; i < lab->result.num_steps && i < first + visible; i++) {
        draw_translation_row(lab, i, (Rectangle){r.x + 2, y, r.width - 4, TABLE_ROW_H});
        y += TABLE_ROW_H;
    }
}

static void draw_results(paging_lab_t *lab, Rectangle r) {
    ui_claim_rect(r);
    ui_panel(r, NULL);
    if (!lab->valid) {
        ui_text(r.x + 14, r.y + 14, 15, UI_BAD, "%s", lab->message);
        return;
    }

    lab_step_controls(ID(50), r.x + 14, r.y + 10, &lab->cursor, lab->count, "addresses");
    lab_step_keys(&lab->cursor, lab->count);
    if (lab->cursor < 1) lab->cursor = 1;

    float x = r.x + 18, w = r.width - 36;
    const paging_step_t *step = selected_step(lab);
    float y = draw_bit_split(lab, step, x, r.y + 52, w);
    y = draw_walk(lab, step, x, y, w);

    float bottom_h = r.y + r.height - y - 10;
    float table_w = 470;
    draw_sizes(lab, (Rectangle){r.x + 8, y, r.width - table_w - 24, bottom_h});
    draw_translations(lab, (Rectangle){r.x + r.width - table_w - 8, y, table_w, bottom_h});
}

void lab_paging_draw(gui_t *g, paging_lab_t *lab, Rectangle r) {
    (void)g;
    if (lab->dirty) recompute(lab);
    draw_results(lab, (Rectangle){r.x + LAB_SETUP_W, r.y, r.width - LAB_SETUP_W, r.height});
    draw_setup_panel(lab, (Rectangle){r.x, r.y, LAB_SETUP_W, r.height});
}
