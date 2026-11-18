#include "prism/disk.h"
#include "prism/text.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const disk_input_t *in;
    disk_result_t      *out;
    bool                pending[DISK_MAX_REQUESTS];
    int                 num_pending;
    int                 position;
} sweep_t;

static void start_sweep(sweep_t *sw, const disk_input_t *in, disk_result_t *out) {
    sw->in = in;
    sw->out = out;
    sw->num_pending = in->num_requests;
    sw->position = in->head;
    for (int i = 0; i < in->num_requests; i++) sw->pending[i] = true;
}

static void move_to(sweep_t *sw, int cylinder, disk_stop_kind_t kind, int request_index) {
    disk_result_t *out = sw->out;
    int distance = abs(cylinder - sw->position);
    out->total_movement += distance;
    if (kind == DISK_STOP_JUMP) out->return_movement += distance;

    out->stops[out->num_stops++] = (disk_stop_t){cylinder, kind, request_index};
    sw->position = cylinder;
}

static void serve(sweep_t *sw, int request_index) {
    move_to(sw, sw->in->requests[request_index], DISK_STOP_REQUEST, request_index);
    sw->pending[request_index] = false;
    sw->num_pending--;
}

static bool lies_ahead(int cylinder, int position, disk_direction_t direction) {
    return direction == DISK_TOWARD_HIGH ? cylinder >= position : cylinder <= position;
}

static int nearest_ahead(const sweep_t *sw, disk_direction_t direction) {
    int best = -1;
    for (int i = 0; i < sw->in->num_requests; i++) {
        if (!sw->pending[i]) continue;
        int cylinder = sw->in->requests[i];
        if (!lies_ahead(cylinder, sw->position, direction)) continue;
        if (best < 0 || abs(cylinder - sw->position) < abs(sw->in->requests[best] - sw->position)) best = i;
    }
    return best;
}

static void serve_everything_ahead(sweep_t *sw, disk_direction_t direction) {
    for (int next = nearest_ahead(sw, direction); next >= 0; next = nearest_ahead(sw, direction)) serve(sw, next);
}

static int far_end(const disk_input_t *in, disk_direction_t direction) {
    return direction == DISK_TOWARD_HIGH ? in->cylinders - 1 : 0;
}

static disk_direction_t opposite(disk_direction_t direction) {
    return direction == DISK_TOWARD_HIGH ? DISK_TOWARD_LOW : DISK_TOWARD_HIGH;
}

static int farthest_behind(const sweep_t *sw, disk_direction_t direction) {
    int best = -1;
    for (int i = 0; i < sw->in->num_requests; i++) {
        if (!sw->pending[i]) continue;
        int cylinder = sw->in->requests[i];
        bool farther = best < 0 || (direction == DISK_TOWARD_HIGH ? cylinder < sw->in->requests[best]
                                                                  : cylinder > sw->in->requests[best]);
        if (farther) best = i;
    }
    return best;
}

static void fcfs_schedule(const disk_input_t *in, disk_result_t *out) {
    sweep_t sw;
    start_sweep(&sw, in, out);
    for (int i = 0; i < in->num_requests; i++) serve(&sw, i);
}

static bool closer(const disk_input_t *in, int position, int candidate, int best) {
    int a = abs(in->requests[candidate] - position);
    int b = abs(in->requests[best] - position);
    if (a != b) return a < b;
    return in->requests[candidate] < in->requests[best];
}

static void sstf_schedule(const disk_input_t *in, disk_result_t *out) {
    sweep_t sw;
    start_sweep(&sw, in, out);
    while (sw.num_pending > 0) {
        int best = -1;
        for (int i = 0; i < in->num_requests; i++) {
            if (sw.pending[i] && (best < 0 || closer(in, sw.position, i, best))) best = i;
        }
        serve(&sw, best);
    }
}

static void scan_schedule(const disk_input_t *in, disk_result_t *out) {
    sweep_t sw;
    start_sweep(&sw, in, out);
    serve_everything_ahead(&sw, in->direction);
    if (sw.num_pending == 0) return;

    int edge = far_end(in, in->direction);
    if (sw.position != edge) move_to(&sw, edge, DISK_STOP_EDGE, -1);
    serve_everything_ahead(&sw, opposite(in->direction));
}

static void look_schedule(const disk_input_t *in, disk_result_t *out) {
    sweep_t sw;
    start_sweep(&sw, in, out);
    serve_everything_ahead(&sw, in->direction);
    serve_everything_ahead(&sw, opposite(in->direction));
}

static void cscan_schedule(const disk_input_t *in, disk_result_t *out) {
    sweep_t sw;
    start_sweep(&sw, in, out);
    serve_everything_ahead(&sw, in->direction);
    if (sw.num_pending == 0) return;

    int edge = far_end(in, in->direction);
    if (sw.position != edge) move_to(&sw, edge, DISK_STOP_EDGE, -1);
    move_to(&sw, far_end(in, opposite(in->direction)), DISK_STOP_JUMP, -1);
    serve_everything_ahead(&sw, in->direction);
}

static void clook_schedule(const disk_input_t *in, disk_result_t *out) {
    sweep_t sw;
    start_sweep(&sw, in, out);
    serve_everything_ahead(&sw, in->direction);
    if (sw.num_pending == 0) return;

    int restart = farthest_behind(&sw, in->direction);
    move_to(&sw, in->requests[restart], DISK_STOP_JUMP, -1);
    serve_everything_ahead(&sw, in->direction);
}

static const disk_ops_t DISK_POLICIES[DISK_COUNT] = {
    [DISK_FCFS]  = {DISK_FCFS, "FCFS", "fcfs", fcfs_schedule},
    [DISK_SSTF]  = {DISK_SSTF, "SSTF", "sstf", sstf_schedule},
    [DISK_SCAN]  = {DISK_SCAN, "SCAN", "scan", scan_schedule},
    [DISK_CSCAN] = {DISK_CSCAN, "C-SCAN", "cscan", cscan_schedule},
    [DISK_LOOK]  = {DISK_LOOK, "LOOK", "look", look_schedule},
    [DISK_CLOOK] = {DISK_CLOOK, "C-LOOK", "clook", clook_schedule},
};

const disk_ops_t *disk_get_ops(disk_policy_t policy) {
    bool known = policy >= 0 && policy < DISK_COUNT;
    return &DISK_POLICIES[known ? policy : DISK_FCFS];
}

bool disk_lookup(const char *key, disk_policy_t *out) {
    for (int i = 0; i < DISK_COUNT; i++) {
        if (text_equals_ignore_case(key, DISK_POLICIES[i].key) || text_equals_ignore_case(key, DISK_POLICIES[i].name)) {
            *out = (disk_policy_t)i;
            return true;
        }
    }
    return false;
}

const char *disk_strerror(disk_status_t status) {
    switch (status) {
        case DISK_OK:            return "ok";
        case DISK_ERR_EMPTY:     return "the request queue is empty";
        case DISK_ERR_TOO_MANY:  return "too many requests (max 64)";
        case DISK_ERR_SYNTAX:    return "use cylinder numbers separated by spaces or commas";
        case DISK_ERR_RANGE:     return "every request must be inside the disk (0 .. cylinders-1)";
        case DISK_ERR_HEAD:      return "the head must be inside the disk";
        case DISK_ERR_CYLINDERS: return "the disk needs between 2 and 100000 cylinders";
    }
    return "unknown error";
}

disk_status_t disk_parse_requests(const char *text, disk_input_t *in) {
    in->num_requests = 0;
    for (const char *p = text; *p;) {
        while (*p && (isspace((unsigned char)*p) || *p == ',' || *p == ';')) p++;
        if (!*p) break;
        if (!isdigit((unsigned char)*p)) return DISK_ERR_SYNTAX;
        if (in->num_requests >= DISK_MAX_REQUESTS) return DISK_ERR_TOO_MANY;

        char *end;
        long cylinder = strtol(p, &end, 10);
        if (*end && !isspace((unsigned char)*end) && *end != ',' && *end != ';') return DISK_ERR_SYNTAX;
        in->requests[in->num_requests++] = cylinder > DISK_MAX_CYLINDERS ? DISK_MAX_CYLINDERS : (int)cylinder;
        p = end;
    }
    return in->num_requests > 0 ? DISK_OK : DISK_ERR_EMPTY;
}

disk_status_t disk_validate(const disk_input_t *in) {
    if (in->cylinders < 2 || in->cylinders > DISK_MAX_CYLINDERS) return DISK_ERR_CYLINDERS;
    if (in->head < 0 || in->head >= in->cylinders) return DISK_ERR_HEAD;
    if (in->num_requests <= 0) return DISK_ERR_EMPTY;
    if (in->num_requests > DISK_MAX_REQUESTS) return DISK_ERR_TOO_MANY;
    for (int i = 0; i < in->num_requests; i++) {
        if (in->requests[i] < 0 || in->requests[i] >= in->cylinders) return DISK_ERR_RANGE;
    }
    return DISK_OK;
}

disk_status_t disk_schedule(disk_policy_t policy, const disk_input_t *in, disk_result_t *out) {
    memset(out, 0, sizeof(*out));
    out->policy = policy;
    out->start = in->head;

    disk_status_t status = disk_validate(in);
    if (status != DISK_OK) return status;

    disk_get_ops(policy)->schedule(in, out);
    return DISK_OK;
}

int disk_distance_moved(const disk_result_t *result, int stop_count) {
    int position = result->start, moved = 0;
    for (int i = 0; i < stop_count && i < result->num_stops; i++) {
        moved += abs(result->stops[i].cylinder - position);
        position = result->stops[i].cylinder;
    }
    return moved;
}
