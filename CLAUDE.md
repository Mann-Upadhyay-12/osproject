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
make gui        # prism-gui (raylib via pkg-config, or RAYLIB_DIR=<raylib checkout>)
./prism-gui workloads/mixed_demo.txt
./prism-gui workloads/mixed_demo.txt --ticks 20 --select memory --shot out.png   # headless smoke test (Xvfb)
```


## Code layout (actual)
| Path | Responsibility |
|---|---|
| `include/prism/types.h`, `pcb.h`, `sim.h` | Flat, pointer-free `sim_t` (≈0.5 MB: keep instances off the stack in new code), `pcb_t`, events |
| `src/engine/sim.c` | Tick-driven main loop `sim_step()`: one named function per phase (`admit_arrivals`, `advance_io`, `advance_fault_service`, `dispatch_idle_cores`, `account_waiting`, `execute_core`), frame helpers, logging |
| `src/engine/ready_queue.c` | Intrusive ready-queue push/pop/remove |
| `src/engine/extensions.c` | Fault injection, resources + deadlock detection/recovery, IPC, checkpoints |
| `src/engine/semaphore.c` | Counting semaphores: `sim_add_semaphore`, `sim_sem_wait` / `sim_sem_signal` (FIFO waiters), `sim_sem_forget` |
| `src/labs/` + `include/prism/{refstring,disk,contig,paging}.h` | Pure lab cores (no `sim_t` except refstring): reference strings + trace import, disk scheduling, contiguous allocation, address translation |
| `src/engine/sim_api.c` / `sim_api.h` | Interactive API: `proc_spec_t`, spawn/remove/update, reset, live policy switch, `sim_set_frames`, `sim_run_until`, `sim_step_event`, observer |
| `src/engine/scenario.c` / `scenario.h` | Scenario save/load = workload format + `layout node=K x= y=` lines; `layout_t` |
| `src/engine/metrics.c` / `metrics.h` | `sim_metrics_compute` (avg wait/turnaround/response, max wait, util, throughput, faults), `sim_prepare_variant` (rewound copy with other policies), `sim_run_bounded` |
| `src/engine/workload.c` / `workload.h` | Workload parser (`proc NAME arrival= priority= bursts=C5,I3 pages=...`), generators |
| `src/sched/schedulers.c` | 8 schedulers behind `sched_ops_t` (registry + `sched_lookup`) |
| `src/vm/replacement.c` | FIFO/LRU/Clock/OPT behind `repl_ops_t` (registry + `repl_lookup`) |
| `src/cli/main.c`, `cli.h`, `report.c`, `experiments.c` | CLI: option parsing + shell / snapshot printing / experiments and demos |
| `src/ui/tui.c` | ncurses dashboard (`tui_t` state, one `draw_*` per panel) |
| `include/prism/text.h` | `text_equals_ignore_case` (shared by policy lookups and parsers) |
| `tests/test_prism.c`, `tests/test_api.c`, `tests/test_metrics.c`, `tests/test_labs.c`, `tests/test_sync.c` | Core suite, interactive API suite, metrics/comparison suite, lab cores, semaphores + classic problems (textbook answers) |
| `gui/` | raylib GUI: `main_gui.c` (window, frame layout, keys, startup flags), `actions.c` (run control, seek/rewind, load/save, node and process edits), `canvas.c` (layout, links, pan/zoom, hit-test), `nodes.c` (node geometry and drawing, colours), `anim.c` (observer → tokens), `explain.c` (narration + algorithm cards), `gantt.c` (Gantt chart, state timeline, results table), `panels.c` (toolbar, palette, tabbed bottom panes, metrics, context menu), `inspector.c`, `builder.c` (process builder), `compare.c` / `compare_view.c` / `compare_details.c` / `compare.h` (Compare view: runner; setup panel, bar chart, results table; Gantt stack, fault sweep, frame table), `widgets.c` (own immediate-mode widgets), `labs.c` + `lab_*.c` (Labs view: one tab per lab, shared step/policy widgets), `sync.c` (Sync tab, semaphore narration, deadlock text) |

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
- No comments in source files: intent comes from names, named constants and small functions. Rationale (the *why*) goes in the decision log below.

## How to work
1. One milestone at a time, in the order in `plan.md`.
2. Write the test first (or alongside) for anything with a known right answer.
3. When a milestone is done: tests pass, `make debug` clean, tick its boxes in `plan.md`, log design choices below.
4. If a documented decision seems wrong, raise it instead of silently deviating.
5. Commit small, with messages like `sched: add SRTF with preemption on arrival`.

## Decision log
- Dashboard is a terminal UI (ncurses) so the whole project stays in C.
- Page faults block the faulting process for `disk_latency` ticks, which is how scheduling and virtual memory interact.
- **GUI library: raylib** (C, small, static-linkable; raygui dropped, see below); fallback SDL2 + SDL_ttf. Core stays library-free; only `gui/` links raylib. `make` builds core + CLI + TUI, `make gui` builds `prism-gui`. If GUI libraries are not allowed, the TUI gets a command line instead (GUI_PLAN §8).
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
- **GUI (M8–M10):** own small immediate-mode widget set (`gui/widgets.c`) instead of raygui, so the only GUI dependency is raylib. raylib's scissor is not nestable, so use `ui_scissor_push/pop`, and `ui_text_clipped` truncates rather than scissoring (it also works inside `BeginMode2D`).
- GUI system nodes exist once each and are "present" when their key is in `layout_t` (`ready`, `sched`, `cpu`, `memory`, `disk`). Process nodes are keyed by process name, so the GUI keeps names unique. Links use the fixed topology from GUI_PLAN §4; a missing node shows a validation banner and blocks Run.
- Edit mode = tick 0. Pressing Run/Step enters Run mode; Reset (`sim_reset`) returns to Edit. In Run mode the builder *injects* (`sim_spawn_process`, arrives now). Frames and cores need a reset, so their controls are disabled while running; scheduler, replacement and disk latency apply live.
- The GUI steps one tick at a time (its "Step" mirrors `sim_step_event`), so it can record per-tick process states for the timeline. Tokens are queued per process so same-tick transitions play back to back; above 30 ticks/s animation is off and only the views update.
- Added `sim_set_cores` (resets) and `sim_set_disk_latency` (live) to `sim_api` for the CPU and Disk inspectors.
- **Metrics live in the core (`metrics.h`)**, not in each frontend, so the GUI's metrics panel, its Compare view and the tests use one definition. Averages are over *completed* processes; waiting = ticks in READY (equals turnaround − burst only when there is no I/O or fault time).
- **Comparison = `sim_prepare_variant` on scratch `sim_t` copies** (copy, detach observer, `sim_reset`, apply policy/quantum/frames). Runs are synchronous and recomputed only when a setting changes (a few ms for all presets). A run that has not finished by 50 000 ticks is shown as "did not finish". The GUI keeps ≤ 4096 Gantt ticks per run.
- **Rewind (Back / click a timeline) = reset + replay to the target tick**, not snapshots (a `sim_t` is 0.5 MB). Exact because the engine is deterministic and injected processes keep their arrival tick; live policy switches made during the run are not recorded, so the replay uses the current policies (the status line says so).
- **Narration is generated inside the observer**, because only then is the state that justified a decision still visible (other ready processes and their SJF/SRTF/CFS/BFS keys; the victim frame's load/last-use tick before it is overwritten). It reads `sim_t` and never changes it.
- **Bundled presets are read-only from the GUI:** loading one points Save at `my_<name>.txt` (they are reference inputs for Compare and for `tests/`).
- **CFS `min_vruntime` is the minimum over runnable tasks (monotonic)**, not the largest vruntime seen. The old code placed every waking task level with the most-run task, so sleepers went to the back. Wakers (tasks that have run before) also keep one slice of credit, as Linux's fair sleepers do; new arrivals do not.
- **`prism-tui` switches policies through `sim_set_sched` / `sim_set_repl`.** `sim_set_policy` is for tick 0 only: it does not migrate ready queues, and using it mid-run stranded processes in MLFQ levels 1–2. The TUI's rewind history is a 500-entry ring, and its Compare panel uses `sim_prepare_variant` like the GUI.
- **`prism-cli` flags override the workload file** (applied through `sim_api` after loading); unknown options and policy names are errors.
- `make sanitize` ends with `make clean` so instrumented objects are never mistaken for an up-to-date release build.
- **Readability refactor (2026-11-18):** source comments removed at the owner's request; long functions split into named steps (`sim_step` phases, per-keyword workload parsers, per-panel TUI/GUI draw functions). `sim.c` was split into `ready_queue.c` + `extensions.c`, the CLI into `report.c` + `experiments.c`, and the GUI gained `actions.c`, `nodes.c` and `compare_details.c`. Behaviour unchanged: identical CLI output and trace hashes.
- **Labs are standalone cores (`src/labs/`)**, not engine features, so adding them cannot change any existing trace. Only the reference-string lab drives the real engine (single process, FCFS, latency 1), so its frame tables are exactly what `prism-cli` would produce. Each lab is pure input → result data (every step recorded) and the GUI only draws a cursor over it.
- **Control bursts run back to back (M17).** After a CPU burst, every following non-CPU burst (acquire/release/send/recv/checkpoint/wait/signal) runs in the same tick until a CPU burst, a block, or the end of the process. Before this, only the first one ran and the next was consumed as a 1-tick CPU burst (so `K` never saved a checkpoint and a trailing control burst crashed). Workload traces are unchanged; only `--deadlock-demo` differs.
- **A process releases its resources when it terminates or is killed**, like a real OS. The deadlock demo used to leave the survivor waiting on a resource held by a finished process.
- **Stall = done.** `sim_is_stalled` is true when no process is NEW/READY/RUNNING/in I/O or fault service but some are blocked (semaphore, resource or IPC). `sim_is_done` includes it, so every run loop stops instead of spinning; `EVENT_STALLED` is logged once. It is computed, not latched, so a deadlock recovery makes the run continue.
- **Semaphores are separate from resources:** a semaphore has no owner (any process may signal), so it cannot reuse the resource code that requires the holder to release. Signal with waiters hands the permit straight to the first waiter (value unchanged), so FIFO order is guaranteed.
