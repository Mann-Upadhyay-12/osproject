#include "labs.h"
#include "compare.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ID(n)        LAB_ID(LAB_DISK, n)
#define CHART_GAP    10.0f
#define EXPLAIN_H    118.0f
#define MAX_COLUMNS  3

typedef struct {
    const char *label;
    const char *requests;
    int         head;
    int         cylinders;
    int         direction;
} disk_preset_t;

static const disk_preset_t PRESETS[] = {
    {"Textbook", "98 183 37 122 14 124 65 67", 53, 200, DISK_TOWARD_HIGH},
    {"Clusters", "10 12 14 15 180 182 185 11 13 181", 100, 200, DISK_TOWARD_HIGH},
    {"Edges", "0 199 1 198 2 197 3 196", 100, 200, DISK_TOWARD_LOW},
};
#define NUM_PRESETS ((int)(sizeof(PRESETS) / sizeof(PRESETS[0])))

static const algo_info_t DISK_INFO[DISK_COUNT] = {
    [DISK_FCFS] = {"FCFS (First Come, First Served)", "Serve requests in the order they arrived.",
        "The head visits each cylinder in arrival order, no matter how far apart consecutive requests are.",
        "Fair and simple; no request can starve.",
        "Wild back-and-forth swings give the largest total head movement.", "-"},
    [DISK_SSTF] = {"SSTF (Shortest Seek Time First)", "Always go to the closest pending request.",
        "From the current cylinder, pick the pending request with the shortest seek (ties: the lower cylinder).",
        "Far less head movement than FCFS.",
        "Requests far from a busy area can starve; it is greedy, not optimal.", "-"},
    [DISK_SCAN] = {"SCAN (Elevator)", "Sweep to the end of the disk, then reverse.",
        "The head serves every request in its direction, keeps going to the last cylinder, then turns around and "
        "serves the rest on the way back.",
        "No starvation; waits are bounded by two sweeps.",
        "Travels to the edge even when nothing is waiting there; requests just behind the head wait a round trip.", "-"},
    [DISK_CSCAN] = {"C-SCAN (Circular SCAN)", "Sweep one way only, then jump back to the start.",
        "Serves requests while moving toward the end, continues to the last cylinder, returns to the other end "
        "without serving anything, and sweeps again. The return seek is counted in the total.",
        "More uniform waiting times than SCAN: the disk is treated as a circle.",
        "The return trip is unproductive movement.", "-"},
    [DISK_LOOK] = {"LOOK", "SCAN that turns around at the last request.",
        "Like SCAN, but the head reverses at the last pending request in its direction instead of at the disk edge.",
        "Avoids SCAN's pointless trip to the edge.",
        "Middle cylinders are still visited more often than the edges.", "-"},
    [DISK_CLOOK] = {"C-LOOK", "C-SCAN that turns at the last request.",
        "Serves in one direction up to the last request, then jumps to the farthest pending request at the other "
        "side (the jump is counted) and sweeps again in the same direction.",
        "Uniform waits with less wasted movement than C-SCAN.",
        "The jump back is still unproductive movement.", "-"},
};

static void load_preset(disk_lab_t *lab, const disk_preset_t *preset) {
    snprintf(lab->input, sizeof(lab->input), "%s", preset->requests);
    lab->head = preset->head;
    lab->cylinders = preset->cylinders;
    lab->direction = preset->direction;
    lab->dirty = true;
}

void lab_disk_init(disk_lab_t *lab) {
    memset(lab, 0, sizeof(*lab));
    for (int p = 0; p < DISK_COUNT; p++) lab->enabled[p] = true;
    lab->hover_policy = -1;
    load_preset(lab, &PRESETS[0]);
}

static void fill_random(disk_lab_t *lab) {
    static uint32_t seed = 777;
    size_t used = 0;
    lab->input[0] = '\0';
    for (int i = 0; i < 10; i++) {
        seed = seed * 1103515245u + 12345u;
        int cylinder = (int)((seed >> 16) % (uint32_t)lab->cylinders);
        used += (size_t)snprintf(lab->input + used, sizeof(lab->input) - used, i ? " %d" : "%d", cylinder);
    }
    lab->dirty = true;
}

static void recompute(disk_lab_t *lab) {
    lab->dirty = false;
    lab->valid = false;

    disk_status_t status = disk_parse_requests(lab->input, &lab->queue);
    lab->queue.cylinders = lab->cylinders;
    lab->queue.head = lab->head;
    lab->queue.direction = (disk_direction_t)lab->direction;
    if (status == DISK_OK) status = disk_validate(&lab->queue);
    if (status != DISK_OK) {
        snprintf(lab->message, sizeof(lab->message), "%s", disk_strerror(status));
        return;
    }

    for (int p = 0; p < DISK_COUNT; p++) disk_schedule((disk_policy_t)p, &lab->queue, &lab->results[p]);
    lab->valid = true;
    lab->cursor = lab->queue.num_requests;
    snprintf(lab->message, sizeof(lab->message), "%d requests, head at %d, cylinders 0..%d", lab->queue.num_requests,
             lab->head, lab->cylinders - 1);
}

static float draw_queue_section(disk_lab_t *lab, float x, float y, float w) {
    lab_draw_section(x, &y, "1. REQUEST QUEUE (arrival order)");
    if (ui_text_field(ID(1), (Rectangle){x, y, w, 26}, lab->input, sizeof(lab->input))) lab->dirty = true;
    y += 30;
    ui_text_clipped((Rectangle){x, y, w, 16}, 12, lab->valid ? UI_TEXT_DIM : UI_BAD, lab->message);
    y += 22;

    float button_w = (w - 4 * NUM_PRESETS) / (NUM_PRESETS + 1);
    for (int i = 0; i < NUM_PRESETS; i++) {
        if (ui_button(ID(10 + i), (Rectangle){x + i * (button_w + 4), y, button_w, 24}, PRESETS[i].label)) {
            load_preset(lab, &PRESETS[i]);
        }
    }
    if (ui_button(ID(19), (Rectangle){x + NUM_PRESETS * (button_w + 4), y, button_w, 24}, "Random")) fill_random(lab);
    return y + 36;
}

static float draw_disk_section(disk_lab_t *lab, float x, float y, float w) {
    lab_draw_section(x, &y, "2. DISK AND HEAD");
    ui_text(x, y + 5, 13, UI_TEXT_DIM, "cylinders");
    if (ui_int_field(ID(20), (Rectangle){x + 110, y, 140, 26}, &lab->cylinders, 2, DISK_MAX_CYLINDERS)) lab->dirty = true;
    y += 30;
    ui_text(x, y + 5, 13, UI_TEXT_DIM, "head starts at");
    if (ui_int_field(ID(21), (Rectangle){x + 110, y, 140, 26}, &lab->head, 0, DISK_MAX_CYLINDERS - 1)) lab->dirty = true;
    y += 32;

    float half = (w - 6) / 2;
    bool toward_low = lab->direction == DISK_TOWARD_LOW;
    if (ui_button_ex(ID(22), (Rectangle){x, y, half, 24}, "moving toward 0", true, toward_low) && !toward_low) {
        lab->direction = DISK_TOWARD_LOW;
        lab->dirty = true;
    }
    if (ui_button_ex(ID(23), (Rectangle){x + half + 6, y, half, 24}, "toward the end", true, !toward_low) && toward_low) {
        lab->direction = DISK_TOWARD_HIGH;
        lab->dirty = true;
    }
    return y + 36;
}

static float draw_policy_section(disk_lab_t *lab, float x, float y, float w) {
    lab_draw_section(x, &y, "3. POLICIES");
    for (int p = 0; p < DISK_COUNT; p++) {
        lab_policy_toggle(ID(30 + p), x, &y, w, p, disk_get_ops((disk_policy_t)p)->name, &lab->enabled[p], &lab->hover_policy);
    }
    return y + 10;
}

static void draw_setup_panel(disk_lab_t *lab, Rectangle r) {
    ui_claim_rect(r);
    ui_panel(r, NULL);
    float x = r.x + 12, y = r.y + 10, w = r.width - 24;

    y = draw_queue_section(lab, x, y, w);
    y = draw_disk_section(lab, x, y, w);
    y = draw_policy_section(lab, x, y, w);

    const algo_info_t *info = lab->hover_policy >= 0 ? &DISK_INFO[lab->hover_policy] : NULL;
    lab_card(r, x, y, w, info,
             "Hover a policy to see how it moves the head. Each chart is the classic textbook seek diagram: "
             "cylinders left to right, time downwards.");
}

static int stops_for_cursor(const disk_result_t *result, int cursor) {
    int served = 0;
    for (int i = 0; i < result->num_stops; i++) {
        if (result->stops[i].kind != DISK_STOP_REQUEST) continue;
        if (++served == cursor) return i + 1;
    }
    return cursor <= 0 ? 0 : result->num_stops;
}

static bool request_served(const disk_result_t *result, int shown_stops, int request_index) {
    for (int i = 0; i < shown_stops; i++) {
        if (result->stops[i].request_index == request_index) return true;
    }
    return false;
}

static int best_total(const disk_lab_t *lab) {
    int best = -1;
    for (int p = 0; p < DISK_COUNT; p++) {
        if (lab->enabled[p] && (best < 0 || lab->results[p].total_movement < best)) best = lab->results[p].total_movement;
    }
    return best;
}

static void draw_summary(disk_lab_t *lab, float x, float y) {
    int best = best_total(lab);
    for (int p = 0; p < DISK_COUNT; p++) {
        if (!lab->enabled[p]) continue;
        const char *text = TextFormat("%s %d", disk_get_ops((disk_policy_t)p)->name, lab->results[p].total_movement);
        lab_legend_swatch(x, y + 3, p);
        ui_text(x + 18, y, 15, lab->results[p].total_movement == best ? UI_GOOD : UI_TEXT, "%s", text);
        x += ui_text_width(text, 15) + 36;
    }
    ui_text(x, y + 2, 13, UI_TEXT_DIM, "cylinders of head movement (lower is better)");
}

static float cylinder_x(const disk_lab_t *lab, Rectangle plot, int cylinder) {
    return plot.x + plot.width * (float)cylinder / (float)(lab->cylinders - 1);
}

static void draw_cylinder_axis(const disk_lab_t *lab, const disk_result_t *result, Rectangle plot, int shown) {
    DrawLine((int)plot.x, (int)plot.y, (int)(plot.x + plot.width), (int)plot.y, UI_BORDER);
    ui_text(plot.x - 2, plot.y - 15, 11, UI_TEXT_DIM, "0");
    const char *last = TextFormat("%d", lab->cylinders - 1);
    ui_text(plot.x + plot.width - ui_text_width(last, 11), plot.y - 15, 11, UI_TEXT_DIM, "%s", last);

    for (int i = 0; i < lab->queue.num_requests; i++) {
        float x = cylinder_x(lab, plot, lab->queue.requests[i]);
        bool served = request_served(result, shown, i);
        Vector2 a = {x - 4, plot.y - 7}, b = {x, plot.y - 1}, c = {x + 4, plot.y - 7};
        if (served) DrawTriangle(a, b, c, UI_TEXT_DIM);
        else DrawTriangleLines(a, b, c, UI_ACCENT);
    }
}

static void draw_dashed(Vector2 from, Vector2 to, Color color) {
    const int dashes = 12;
    for (int k = 0; k < dashes; k += 2) {
        float t0 = (float)k / dashes, t1 = (float)(k + 1) / dashes;
        Vector2 a = {from.x + (to.x - from.x) * t0, from.y + (to.y - from.y) * t0};
        Vector2 b = {from.x + (to.x - from.x) * t1, from.y + (to.y - from.y) * t1};
        DrawLineEx(a, b, 1.5f, color);
    }
}

static void draw_seek_path(const disk_lab_t *lab, const disk_result_t *result, int policy, Rectangle plot, int shown) {
    float row_h = plot.height / (float)(result->num_stops > 0 ? result->num_stops : 1);
    Color color = cmp_alg_color(policy);
    Vector2 previous = {cylinder_x(lab, plot, result->start), plot.y + 4};
    DrawCircleV(previous, 4, UI_TEXT);

    for (int i = 0; i < shown; i++) {
        const disk_stop_t *stop = &result->stops[i];
        Vector2 point = {cylinder_x(lab, plot, stop->cylinder), plot.y + 4 + (i + 1) * row_h};

        if (stop->kind == DISK_STOP_JUMP) draw_dashed(previous, point, UI_TEXT_DIM);
        else DrawLineEx(previous, point, 2, color);

        if (stop->kind == DISK_STOP_REQUEST) {
            DrawCircleV(point, 4, color);
            if (row_h >= 13) ui_text(point.x + 6, point.y - 7, 11, UI_TEXT, "%d", stop->cylinder);
        } else {
            DrawRectangle((int)point.x - 3, (int)point.y - 3, 6, 6, UI_TEXT_DIM);
        }

        if (CheckCollisionPointCircle(GetMousePosition(), point, 7)) {
            const char *what = stop->kind == DISK_STOP_REQUEST ? "serve request" : stop->kind == DISK_STOP_EDGE ? "reach the edge" : "jump back";
            ui_tooltip("step %d: %s at cylinder %d\nhead moved %d so far", i + 1, what, stop->cylinder,
                       disk_distance_moved(result, i + 1));
        }
        previous = point;
    }
}

static void draw_chart(disk_lab_t *lab, int policy, Rectangle cell) {
    const disk_result_t *result = &lab->results[policy];
    DrawRectangleRec(cell, (Color){28, 31, 38, 255});
    DrawRectangleLinesEx(cell, 1, UI_BORDER);
    if (CheckCollisionPointRec(GetMousePosition(), cell)) lab->hover_policy = policy;

    int shown = stops_for_cursor(result, lab->cursor);
    lab_legend_swatch(cell.x + 8, cell.y + 9, policy);
    ui_text(cell.x + 26, cell.y + 6, 15, UI_TEXT, "%s", disk_get_ops((disk_policy_t)policy)->name);
    const char *moved = result->return_movement > 0
        ? TextFormat("moved %d of %d  (return %d)", disk_distance_moved(result, shown), result->total_movement, result->return_movement)
        : TextFormat("moved %d of %d", disk_distance_moved(result, shown), result->total_movement);
    ui_text(cell.x + cell.width - ui_text_width(moved, 12) - 8, cell.y + 8, 12, UI_TEXT_DIM, "%s", moved);

    Rectangle plot = {cell.x + 16, cell.y + 46, cell.width - 32, cell.height - 58};
    draw_cylinder_axis(lab, result, plot, shown);
    draw_seek_path(lab, result, policy, plot, shown);
}

static int enabled_count(const disk_lab_t *lab) {
    int count = 0;
    for (int p = 0; p < DISK_COUNT; p++) count += lab->enabled[p];
    return count;
}

static void draw_chart_grid(disk_lab_t *lab, Rectangle area) {
    int count = enabled_count(lab);
    if (count == 0) {
        ui_text(area.x + 14, area.y + 10, 15, UI_TEXT_DIM, "Tick at least one policy.");
        return;
    }
    int columns = count < MAX_COLUMNS ? count : MAX_COLUMNS;
    int rows = (count + columns - 1) / columns;
    float cell_w = (area.width - CHART_GAP * (columns + 1)) / columns;
    float cell_h = (area.height - CHART_GAP * (rows + 1)) / rows;

    int slot = 0;
    for (int p = 0; p < DISK_COUNT; p++) {
        if (!lab->enabled[p]) continue;
        int column = slot % columns, row = slot / columns;
        Rectangle cell = {area.x + CHART_GAP + column * (cell_w + CHART_GAP), area.y + CHART_GAP + row * (cell_h + CHART_GAP),
                          cell_w, cell_h};
        draw_chart(lab, p, cell);
        slot++;
    }
}

static int request_stop_index(const disk_result_t *result, int nth) {
    int served = 0;
    for (int i = 0; i < result->num_stops; i++) {
        if (result->stops[i].kind == DISK_STOP_REQUEST && ++served == nth) return i;
    }
    return -1;
}

static const char *sweep_word(int from, int to) {
    return to >= from ? "upward" : "downward";
}

static void describe_move(const disk_lab_t *lab, int policy, char *out, size_t size) {
    const disk_result_t *result = &lab->results[policy];
    int index = request_stop_index(result, lab->cursor);
    if (index < 0) {
        out[0] = '\0';
        return;
    }

    int previous_request = request_stop_index(result, lab->cursor - 1);
    int from = previous_request >= 0 ? result->stops[previous_request].cylinder : result->start;
    int to = result->stops[index].cylinder;
    int moved = disk_distance_moved(result, index + 1) - disk_distance_moved(result, previous_request + 1);

    char detour[80] = "";
    for (int i = previous_request + 1; i < index; i++) {
        const disk_stop_t *stop = &result->stops[i];
        size_t used = strlen(detour);
        snprintf(detour + used, sizeof(detour) - used, stop->kind == DISK_STOP_EDGE ? "to the edge %d, " : "jumps to %d, ", stop->cylinder);
    }

    const char *reason;
    switch ((disk_policy_t)policy) {
        case DISK_FCFS: reason = "next in arrival order"; break;
        case DISK_SSTF: reason = "closest pending request"; break;
        default:        reason = TextFormat("next request on the %s sweep", sweep_word(index > 0 ? result->stops[index - 1].cylinder : from, to)); break;
    }
    snprintf(out, size, "%d -> %s%d: %s (%d cylinders)", from, detour, to, reason, moved);
}

static void draw_explanation(disk_lab_t *lab, Rectangle r) {
    DrawRectangleRec(r, UI_PANEL_HI);
    if (lab->cursor <= 0) {
        ui_text(r.x + 10, r.y + 8, 14, UI_TEXT_DIM, "Press > (or the Right key) to serve the requests one at a time.");
        return;
    }

    ui_text(r.x + 10, r.y + 6, 13, UI_ACCENT, "REQUEST %d SERVED BY EACH POLICY", lab->cursor);
    float y = r.y + 26;
    for (int p = 0; p < DISK_COUNT && y < r.y + r.height - 14; p++) {
        if (!lab->enabled[p]) continue;
        char line[200];
        describe_move(lab, p, line, sizeof(line));
        lab_legend_swatch(r.x + 10, y + 2, p);
        ui_text(r.x + 26, y, 13, UI_TEXT_DIM, "%s:", disk_get_ops((disk_policy_t)p)->name);
        ui_text_clipped((Rectangle){r.x + 90, y, r.width - 100, 16}, 13, UI_TEXT, line);
        y += 15;
    }
}

static void draw_results(disk_lab_t *lab, Rectangle r) {
    ui_claim_rect(r);
    ui_panel(r, NULL);
    if (!lab->valid) {
        ui_text(r.x + 14, r.y + 14, 15, UI_BAD, "%s", lab->message);
        return;
    }

    draw_summary(lab, r.x + 14, r.y + 10);
    lab_step_controls(ID(50), r.x + 14, r.y + 36, &lab->cursor, lab->queue.num_requests, "requests");
    lab_step_keys(&lab->cursor, lab->queue.num_requests);

    draw_chart_grid(lab, (Rectangle){r.x, r.y + 64, r.width, r.height - 64 - EXPLAIN_H - 24});
    draw_explanation(lab, (Rectangle){r.x + 8, r.y + r.height - EXPLAIN_H - 22, r.width - 16, EXPLAIN_H});
    ui_text(r.x + 12, r.y + r.height - 18, 11, UI_TEXT_DIM,
            "hollow triangle = still waiting  |  square = edge of the disk  |  dashed = return jump (no requests served)");
}

void lab_disk_draw(gui_t *g, disk_lab_t *lab, Rectangle r) {
    (void)g;
    if (lab->dirty) recompute(lab);
    lab->hover_policy = -1;

    draw_results(lab, (Rectangle){r.x + LAB_SETUP_W, r.y, r.width - LAB_SETUP_W, r.height});
    draw_setup_panel(lab, (Rectangle){r.x, r.y, LAB_SETUP_W, r.height});
}
