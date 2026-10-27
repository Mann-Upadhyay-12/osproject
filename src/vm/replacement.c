#include "prism/vm.h"
#include "prism/sim.h"
#include <limits.h>
#include <assert.h>

/* =========================================================================
 * 1. FIFO (First-In, First-Out)
 * Evicts the frame with the earliest load_tick.
 * ========================================================================= */
static void fifo_init(sim_t *s) {
    (void)s;
}

static void fifo_on_load(sim_t *s, sim_pid_t pid, uint16_t vpage, int frame_idx) {
    (void)s;
    (void)pid;
    (void)vpage;
    (void)frame_idx;
}

static void fifo_on_access(sim_t *s, sim_pid_t pid, uint16_t vpage, int frame_idx) {
    (void)s;
    (void)pid;
    (void)vpage;
    (void)frame_idx;
}

static int fifo_pick_victim(sim_t *s) {
    int victim = -1;
    tick_t min_tick = UINT64_MAX;

    for (int i = 0; i < s->total_frames; i++) {
        if (!s->frames[i].occupied) return i;
        if (s->frames[i].load_tick < min_tick) {
            min_tick = s->frames[i].load_tick;
            victim = i;
        }
    }
    return (victim == -1) ? 0 : victim;
}

static const repl_ops_t g_repl_fifo = {
    .policy      = REPL_FIFO,
    .name        = "FIFO",
    .init        = fifo_init,
    .on_load     = fifo_on_load,
    .on_access   = fifo_on_access,
    .pick_victim = fifo_pick_victim
};

/* =========================================================================
 * 2. LRU (Least Recently Used)
 * Evicts the frame with the earliest last_access_tick.
 * ========================================================================= */
static void lru_init(sim_t *s) {
    (void)s;
}

static void lru_on_load(sim_t *s, sim_pid_t pid, uint16_t vpage, int frame_idx) {
    (void)s;
    (void)pid;
    (void)vpage;
    (void)frame_idx;
}

static void lru_on_access(sim_t *s, sim_pid_t pid, uint16_t vpage, int frame_idx) {
    (void)s;
    (void)pid;
    (void)vpage;
    (void)frame_idx;
}

static int lru_pick_victim(sim_t *s) {
    int victim = -1;
    tick_t min_tick = UINT64_MAX;

    for (int i = 0; i < s->total_frames; i++) {
        if (!s->frames[i].occupied) return i;
        if (s->frames[i].last_access_tick < min_tick) {
            min_tick = s->frames[i].last_access_tick;
            victim = i;
        }
    }
    return (victim == -1) ? 0 : victim;
}

static const repl_ops_t g_repl_lru = {
    .policy      = REPL_LRU,
    .name        = "LRU",
    .init        = lru_init,
    .on_load     = lru_on_load,
    .on_access   = lru_on_access,
    .pick_victim = lru_pick_victim
};

/* =========================================================================
 * 3. Clock (Second-Chance)
 * Uses circular pointer (clock_hand) and reference bit.
 * ========================================================================= */
static void clock_init(sim_t *s) {
    s->clock_hand = 0;
}

static void clock_on_load(sim_t *s, sim_pid_t pid, uint16_t vpage, int frame_idx) {
    (void)s;
    (void)pid;
    (void)vpage;
    (void)frame_idx;
}

static void clock_on_access(sim_t *s, sim_pid_t pid, uint16_t vpage, int frame_idx) {
    (void)pid;
    (void)vpage;
    s->frames[frame_idx].ref_bit = true;
}

static int clock_pick_victim(sim_t *s) {
    if (s->total_frames <= 0) return 0;

    for (int attempts = 0; attempts < s->total_frames * 2 + 1; attempts++) {
        int idx = s->clock_hand % s->total_frames;
        s->clock_hand = (s->clock_hand + 1) % s->total_frames;

        if (!s->frames[idx].occupied) {
            return idx;
        }
        if (!s->frames[idx].ref_bit) {
            return idx;
        }
        /* Give second chance: clear ref bit and continue */
        s->frames[idx].ref_bit = false;
    }
    return 0;
}

static const repl_ops_t g_repl_clock = {
    .policy      = REPL_CLOCK,
    .name        = "Clock",
    .init        = clock_init,
    .on_load     = clock_on_load,
    .on_access   = clock_on_access,
    .pick_victim = clock_pick_victim
};

/* =========================================================================
 * 4. Optimal (Belady's OPT)
 * Looks forward into future references and evicts the frame that will
 * not be used for the longest period of time (or never again).
 * ========================================================================= */
static void opt_init(sim_t *s) {
    (void)s;
}

static void opt_on_load(sim_t *s, sim_pid_t pid, uint16_t vpage, int frame_idx) {
    (void)s;
    (void)pid;
    (void)vpage;
    (void)frame_idx;
}

static void opt_on_access(sim_t *s, sim_pid_t pid, uint16_t vpage, int frame_idx) {
    (void)s;
    (void)pid;
    (void)vpage;
    (void)frame_idx;
}

static int opt_pick_victim(sim_t *s) {
    int victim = -1;
    uint32_t max_dist = 0;

    for (int i = 0; i < s->total_frames; i++) {
        if (!s->frames[i].occupied) return i;

        sim_pid_t pid = s->frames[i].owner_pid;
        uint16_t vpage = s->frames[i].vpage;
        uint32_t dist = UINT32_MAX;

        if (pid >= 0 && pid < s->num_processes) {
            const pcb_t *p = &s->procs[pid];
            /* Look forward in the remaining page references */
            for (uint16_t r = p->curr_page_ref_idx; r < p->num_page_refs; r++) {
                if (p->page_refs[r] == vpage) {
                    dist = (uint32_t)(r - p->curr_page_ref_idx);
                    break;
                }
            }
        }

        if (dist > max_dist) {
            max_dist = dist;
            victim = i;
        }
    }

    return (victim == -1) ? 0 : victim;
}

static const repl_ops_t g_repl_opt = {
    .policy      = REPL_OPT,
    .name        = "Optimal",
    .init        = opt_init,
    .on_load     = opt_on_load,
    .on_access   = opt_on_access,
    .pick_victim = opt_pick_victim
};

/* =========================================================================
 * Page Replacement Ops Registry
 * ========================================================================= */
const repl_ops_t *repl_get_ops(repl_policy_t policy) {
    switch (policy) {
        case REPL_FIFO:  return &g_repl_fifo;
        case REPL_LRU:   return &g_repl_lru;
        case REPL_CLOCK: return &g_repl_clock;
        case REPL_OPT:   return &g_repl_opt;
        default:         return &g_repl_fifo;
    }
}
