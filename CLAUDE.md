# PRISM

Read this first, then `plan.md` (what to build, in order), `architecture.md` (original design) and `GUI_PLAN.md` (interactive GUI, Milestones 7–11).

## What this is
PRISM = Process, Resource & Interprocess Simulation Model. An **event-driven OS simulator** for an Operating Systems course project, with a live dashboard and (Milestone 8+) a "Packet Tracer for processes" GUI.

**Required scope (graded):** CPU scheduling + virtual memory management.
Resources/deadlock, IPC, checkpoint/restore and fault injection are extensions (already implemented).

## Hard constraints
- **Pure C (C11).** No C++, no Python in the build.
- **Simulator core stays library-free (libc + libm).** Frontends (`src/ui/tui.c`, `gui/`) may use ncurses / raylib. (Check with the teacher that a C GUI library is acceptable; if not, see GUI_PLAN §8.)
- **Deterministic.** Virtual clock (`tick_t`), never wall-clock time in simulation logic. All randomness is seeded (`prng.h`), so the same scenario + seed gives an identical trace (`log_hash`).
- The core (`src/engine`, `src/sched`, `src/vm`) has **zero UI dependency**. Frontends never mutate `sim_t` directly: they go through `sim_api.h` (edits, policies, stepping) and read state / subscribe with `sim_set_observer()`. The observer is a view of the trace and never feeds back.
- Headless mode (`prism-cli`) must always work. Tests depend on the core only.

## Build & test
```
make            # build/libprism.a + prism-cli + prism-tui
make test       # builds and runs every tests/test_*.c
make debug      # clean, -O0 -g with ASan + UBSan, runs all tests
make clean
./prism-cli --workload workloads/mixed_demo.txt
./prism-tui workloads/mixed_demo.txt
```
`make gui` (raylib, `prism-gui`) arrives in Milestone 8.

## Code layout (actual)
| Path | Responsibility |
|---|---|
| `include/prism/types.h`, `pcb.h`, `sim.h` | Flat, pointer-free `sim_t` (≈0.5 MB: keep instances off the stack in new code), `pcb_t`, events |
| `src/engine/sim.c` | Tick-driven main loop `sim_step()` (6 phases: arrivals, I/O, fault service, dispatch, wait accounting, execution), extensions |
| `src/engine/sim_api.c` / `sim_api.h` | Interactive API: `proc_spec_t`, spawn/remove/update, reset, live policy switch, `sim_set_frames`, `sim_run_until`, `sim_step_event`, observer |
| `src/engine/scenario.c` / `scenario.h` | Scenario save/load = workload format + `layout node=K x= y=` lines; `layout_t` |
| `src/engine/workload.c` / `workload.h` | Workload parser (`proc NAME arrival= priority= bursts=C5,I3 pages=...`), generators |
| `src/sched/schedulers.c` | 8 schedulers behind `sched_ops_t` (registry + `sched_lookup`) |
| `src/vm/replacement.c` | FIFO/LRU/Clock/OPT behind `repl_ops_t` (registry + `repl_lookup`) |
| `src/cli/main.c`, `src/ui/tui.c` | Frontends |
| `tests/test_prism.c`, `tests/test_api.c` | Core suite, interactive API suite |

### Where the code differs from architecture.md (it is the original design; the code wins)
- Engine is **tick-stepped** (`sim_step` = one tick), not a min-heap of events; there is no `EV_ARRIVAL` — arrivals are admitted in phase 1 when `arrival_tick <= tick`.
- Workload format is the one in `workload.h` (`cores/frames/disk_latency/sched/repl/quantum/seed/resource` lines + one-line `proc`), not `config … / proc … end`. Files live in `workloads/*.txt`. No `refs=seq|rand|local` / `ws=` (pages use `loop:`/`seq:`/`random:`/explicit lists).
- `fault_service` is called `disk_latency`; `ctx_switch` cost is not modelled (switches are counted only).
- Single files per subsystem (`schedulers.c`, `replacement.c`) instead of one file per policy; still ops-table based.

## Code conventions
- C11, `-Wall -Wextra -Wpedantic` must stay clean. Fix warnings, never silence them.
- Naming: `module_verb_noun()`, types end in `_t`. Headers have include guards and document their contract.
- Pluggable policies use **function-pointer ops tables**. Adding a policy = one ops struct (with `.key`) + one line in the registry. No `switch` on policy *names* in core code — use `sched_lookup` / `repl_lookup`.
- Check every `malloc`/`fopen`. Out of memory: print to stderr and exit non-zero. Recoverable API errors return `SIM_ERR_*` (`sim_strerror`).
- Free everything; `make debug` must be leak-clean.
- Keep files focused (~400–500 lines).
- Comments explain *why*, not *what*.

## How to work
1. One milestone at a time, in the order in `plan.md`.
2. Write the test first (or alongside) for anything with a known right answer.
3. When a milestone is done: tests pass, `make debug` clean, tick its boxes in `plan.md`, log design choices below.
4. If a documented decision seems wrong, raise it instead of silently deviating.
5. Commit small, with messages like `sched: add SRTF with preemption on arrival`.

## Decision log
- Dashboard is a terminal UI (ncurses) so the whole project stays in C.
- Page faults block the faulting process for `disk_latency` ticks, which is how scheduling and virtual memory interact.
- **GUI library: raylib + raygui** (C, small, static-linkable); fallback SDL2 + SDL_ttf. Core stays library-free; only `gui/` links raylib. `make` builds core + CLI + TUI, `make gui` builds `prism-gui`. If GUI libraries are not allowed, the TUI gets a command line instead (GUI_PLAN §8).
- **`sim_api` reconciled with the existing code (M7):** GUI_PLAN's `sim_add_process(proto)` is `sim_spawn_process(const proc_spec_t *)` because `sim_add_process(name, arrival)` already exists and the tests use it. `sim_step_event` steps ticks until a *notable* event (anything except `PAGE_ACCESS`); plain `sim_step` is still one tick.
- **Observer delivers `sim_event_t` (the trace record), not a separate `obs_event` enum.** `event_type_t` already covers arrive/dispatch/preempt/block/wake/fault/evict/exit and more, and reusing it guarantees "observer order == trace order". It fires from `sim_log_event`, so it keeps working after `event_log` (8192 entries) is full. No IDLE event: idle is visible as `running_pid[c] == -1` / gantt `-1`, and logging it would fill the trace.
- Observer pointer lives in `sim_t` (so `sim_restore` copies it along with the snapshot); it is not hashed and does not affect determinism. `sim_reset` and `scenario_load` keep it.
- Mid-run spawn: `arrival = max(spec.arrival, tick)` and the arrival tick is stored, so `sim_reset` + rerun reproduces the same trace hash.
- Removal leaves a hole (`PROC_UNUSED`); pids are never reused; `sim_is_done` counts `completed + num_removed`. Removed processes are skipped by `scenario_save` (later pids shift on reload — matters only for IPC targets).
- `sim_reset` rebuilds via `sim_init` + re-adding specs, so new `sim_t` fields reset automatically. It restores `configured_frames` (undoing RAM-shrink injection) and each process's `base_priority` (MLFQ mutates `priority`).
- Live scheduler switch drains all ready queues (Q0..Q2) and re-enqueues via the new policy's `on_ready`, so nothing is stranded in an MLFQ level. Replacement switch is always safe (shared frame metadata). Frame count change always resets.
- **Layout is stored inline** as `layout node=KEY x=N y=N` lines in the scenario file (no sidecar). `workload_load_file` ignores unknown keywords, so scenario files still run in `prism-cli`/`prism-tui`.
- `scenario_load` uses `prism-cli`'s defaults (1 core, 4 frames, latency 3, RR, LRU, seed 42) so a file behaves the same in every frontend.
- Bug fixes found during M7: a process ending on an I/O burst now frees its frames (`sim_release_frames`); page numbers ≥ 64 in workload files are rejected instead of overrunning the page table; workload line/field buffers enlarged (256 page refs fit on one line); Makefile tracks header dependencies (`-MMD`) because a stale object after a `sim_t` change segfaults.
