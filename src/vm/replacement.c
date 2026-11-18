#include "prism/vm.h"
#include "prism/sim.h"
#include "prism/text.h"

#define NEVER_USED_AGAIN UINT32_MAX

static int first_free_frame(const sim_t *s) {
    for (int f = 0; f < s->total_frames; f++) {
        if (!s->frames[f].occupied) return f;
    }
    return -1;
}

static int fifo_pick_victim(sim_t *s) {
    int free_frame = first_free_frame(s);
    if (free_frame >= 0) return free_frame;

    int oldest = 0;
    for (int f = 1; f < s->total_frames; f++) {
        if (s->frames[f].load_tick < s->frames[oldest].load_tick) oldest = f;
    }
    return oldest;
}

static int lru_pick_victim(sim_t *s) {
    int free_frame = first_free_frame(s);
    if (free_frame >= 0) return free_frame;

    int least_recent = 0;
    for (int f = 1; f < s->total_frames; f++) {
        if (s->frames[f].last_access_tick < s->frames[least_recent].last_access_tick) least_recent = f;
    }
    return least_recent;
}

static void clock_init(sim_t *s) {
    s->clock_hand = 0;
}

static void clock_on_access(sim_t *s, sim_pid_t pid, uint16_t vpage, int frame_idx) {
    (void)pid;
    (void)vpage;
    s->frames[frame_idx].ref_bit = true;
}

static int clock_advance_hand(sim_t *s) {
    int frame = s->clock_hand % s->total_frames;
    s->clock_hand = (s->clock_hand + 1) % s->total_frames;
    return frame;
}

static int clock_pick_victim(sim_t *s) {
    if (s->total_frames <= 0) return 0;

    int max_steps = s->total_frames * 2 + 1;
    for (int step = 0; step < max_steps; step++) {
        int f = clock_advance_hand(s);
        frame_entry_t *frame = &s->frames[f];

        if (!frame->occupied || !frame->ref_bit) return f;
        frame->ref_bit = false;
    }
    return 0;
}

static uint32_t distance_to_next_use(const sim_t *s, const frame_entry_t *frame) {
    sim_pid_t pid = frame->owner_pid;
    if (pid < 0 || pid >= s->num_processes) return NEVER_USED_AGAIN;

    const pcb_t *p = &s->procs[pid];
    for (uint16_t r = p->curr_page_ref_idx; r < p->num_page_refs; r++) {
        if (p->page_refs[r] == frame->vpage) return (uint32_t)(r - p->curr_page_ref_idx);
    }
    return NEVER_USED_AGAIN;
}

static int opt_pick_victim(sim_t *s) {
    int free_frame = first_free_frame(s);
    if (free_frame >= 0) return free_frame;

    int victim = 0;
    uint32_t farthest = 0;
    for (int f = 0; f < s->total_frames; f++) {
        uint32_t distance = distance_to_next_use(s, &s->frames[f]);
        if (distance > farthest) {
            farthest = distance;
            victim = f;
        }
    }
    return victim;
}

static const repl_ops_t REPLACERS[REPL_COUNT] = {
    [REPL_FIFO]  = { .policy = REPL_FIFO,  .name = "FIFO",    .key = "fifo",
                     .pick_victim = fifo_pick_victim },
    [REPL_LRU]   = { .policy = REPL_LRU,   .name = "LRU",     .key = "lru",
                     .pick_victim = lru_pick_victim },
    [REPL_CLOCK] = { .policy = REPL_CLOCK, .name = "Clock",   .key = "clock",
                     .init = clock_init, .on_access = clock_on_access,
                     .pick_victim = clock_pick_victim },
    [REPL_OPT]   = { .policy = REPL_OPT,   .name = "Optimal", .key = "opt",
                     .pick_victim = opt_pick_victim },
};

const repl_ops_t *repl_get_ops(repl_policy_t policy) {
    bool known = policy >= 0 && policy < REPL_COUNT;
    return &REPLACERS[known ? policy : REPL_FIFO];
}

bool repl_lookup(const char *name, repl_policy_t *out) {
    for (int i = 0; i < REPL_COUNT; i++) {
        const repl_ops_t *ops = &REPLACERS[i];
        if (text_equals_ignore_case(name, ops->key) || text_equals_ignore_case(name, ops->name)) {
            if (out) *out = ops->policy;
            return true;
        }
    }
    return false;
}
