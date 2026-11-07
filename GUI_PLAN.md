# PRISM — Interactive GUI Addendum ("Packet Tracer for processes")

Adds **Milestone 7+** to `plan.md` and amends `CLAUDE.md`. Existing code (TUI, sim core, schedulers, VM) is the base.

> **Reconciled with the code (Milestone 7, done).** The real API is in `include/prism/sim_api.h` and `include/prism/scenario.h`; differences from the sketch in §3 are recorded in `CLAUDE.md`'s decision log. In short: `sim_add_process(proto)` → `sim_spawn_process(const proc_spec_t *)`; the observer delivers `sim_event_t` / `event_type_t` instead of a new `obs_kind` enum (no IDLE event); layout lives inline in the scenario file; `struct sim` is `sim_t`, `struct pcb` prototype is `proc_spec_t`.

## 1. Goal

Today: load a workload file, watch a TUI. Target: a desktop app where you **build an OS scenario visually**, like Cisco Packet Tracer builds networks:

| Packet Tracer | PRISM |
|---|---|
| Routers / switches / PCs on a canvas | **Process, Ready Queue, Scheduler, CPU, Memory (frames), Disk/I-O** nodes on a canvas |
| Cables between devices | **Links** showing the flow of a process through the system |
| Packets moving along cables | **Process tokens** animating along links (arrival → ready → CPU → I/O or fault → ready → exit) |
| Realtime / Simulation mode | **Edit mode** / **Run mode** |
| Event List panel | **Event List**: every dispatch, preempt, fault, evict, I/O, exit with tick |
| Inspect a device | Click a node → **Inspector**: PCB, page table, frame grid, queue contents |

## 2. Decisions (recorded in CLAUDE.md)

- **GUI library: raylib + raygui** (C, tiny, static-linkable, built-in drawing/text/widgets). Fallback: SDL2 + SDL_ttf if raylib is not allowed. **Ask the teacher first**: the course says "entirely in C"; the GUI is still C but uses a library. If libraries are forbidden, keep the TUI and make the TUI interactive (see §8).
- Constraint: "Simulator core stays library-free (libc + libm). Frontends (`tui.c`, `gui/`) may use ncurses / raylib."
- Core is the static library `build/libprism.a`; `prism-cli`, `prism-tui` and `prism-gui` link it. `make` builds core + CLI + TUI. `make gui` builds `prism-gui`.
- **GUI never mutates sim state directly.** It issues commands through `sim_api`. Same API serves the TUI.
- Determinism stays: same scenario + seed → same run. Animation is a *view* of the trace, never feeds back.

## 3. Core changes (Milestone 7) — implemented

See `include/prism/sim_api.h`:

```c
int  sim_spawn_process(sim_t *s, const proc_spec_t *spec);   /* arrival = max(spec.arrival, now) */
int  sim_remove_process(sim_t *s, sim_pid_t pid);            /* only if NEW or TERMINATED */
int  sim_update_process(sim_t *s, sim_pid_t pid, const proc_spec_t *spec); /* only before it first runs */
int  sim_get_process_spec(const sim_t *s, sim_pid_t pid, proc_spec_t *out);
void sim_reset(sim_t *s);                                    /* rewind to t=0, keep scenario */
int  sim_set_sched(sim_t *s, const char *name, int quantum); /* live: migrates ready queues */
int  sim_set_repl(sim_t *s, const char *name);               /* live */
int  sim_set_frames(sim_t *s, int nframes);                  /* resets */
bool sim_run_until(sim_t *s, tick_t t);
bool sim_step_event(sim_t *s);                               /* until next non-PAGE_ACCESS event */
void sim_set_observer(sim_t *s, sim_observer_fn fn, void *user); /* fn(const sim_event_t *, user) */

int  scenario_save(const sim_t *s, const layout_t *L, const char *path);
int  scenario_load(sim_t *s, layout_t *L, const char *path);
```

- Mid-run process creation: the new process arrives at the **current tick**; the UI should say "arrives now".
- Process ids stay stable; deleting leaves a hole (`PROC_UNUSED`); pids are never reused within a scenario.
- Scenario files gain optional layout lines that the workload parser ignores:
  ```
  layout node=cpu x=640 y=200
  layout node=P1 x=120 y=80
  ```
- `tests/test_api.c`: add-process-mid-run, reset-then-rerun gives identical metrics and hash, remove/update rules, observer order matches trace, live scheduler switch, frames, save/load round trip, old files still load.

## 4. GUI design

### Window layout (default 1280×800, resizable)
```
┌ Toolbar: [Edit|Run] ▶ ⏸ ⏭ step  speed▮▮▮  [Scheduler ▾][q] [Repl ▾][frames]  Save Load ┐
├ Palette ─┬──────────── Canvas (pan/zoom grid) ──────────────┬ Inspector ───────────┤
│ Process  │   [P1]──┐                                         │ selected node details │
│ Ready Q  │   [P2]──┼─►[Ready Queue]─►[Scheduler]─►[ CPU ]    │ (PCB / page table /   │
│ Scheduler│   [P3]──┘                     ▲   │        │      │  frame grid / queue)  │
│ CPU      │                    [Disk/I-O]◄─┘   └►[Memory]     │                      │
│ Memory   │                                                   │                      │
│ Disk/I-O │                                                   │                      │
├ Event List (tick | event | pid | detail) ─────┬ Gantt strip ─┬ Metrics ─────────────┤
└───────────────────────────────────────────────┴──────────────┴──────────────────────┘
```

### Nodes
| Node | Shows | Editable |
|---|---|---|
| **Process** (many) | name, state colour, burst progress bar | arrival, priority, page refs (list / `loop:` / `seq:` / `random:`), **burst list** (add/remove/reorder `cpu N` / `io N`) |
| **Ready Queue** | tokens in order | — |
| **Scheduler** | policy name, quantum | policy dropdown, quantum |
| **CPU** | running process (per core), quantum bar | cores |
| **Memory** | frame grid (`pid:page`, colour per process, evicted cell flashes red), fault counter | frames, replacement policy dropdown |
| **Disk/I-O** | I/O wait queue + page-fault service queue | `disk_latency` ticks |

Process state colours: NEW grey, READY blue, RUNNING green, BLOCKED_IO amber, BLOCKED_FAULT red, TERMINATED dim.

### Links
Fixed topology to start: Process → Ready Queue → Scheduler → CPU; CPU → Disk/I-O → Ready Queue; CPU ↔ Memory. Drawn automatically when nodes exist, with dashed "ghost" links if a required node is missing and a validation message ("Add a CPU node"). Free-form wiring is an extension; do not build it first.

### Interaction
- Drag nodes, pan (middle mouse / space+drag), zoom (wheel), snap-to-grid toggle.
- Drag from Palette to canvas to create a node; **Process** opens the **Process Builder** dialog.
- Right-click node: Edit, Duplicate, Delete.
- Keyboard: `Space` run/pause, `N` step one event, `R` reset, `Ctrl+S` save, `Del` delete, `Ctrl+Z` undo (stretch).
- **Edit mode**: scenario editable, sim at tick 0. **Run mode**: structure locked; you can still **inject a new process** ("Add process now") and change scheduler/replacement live (frames require reset; label which).

### Process Builder dialog
Fields: name, arrival, priority, page refs, burst list editor with live total CPU/IO, validation via `proc_spec_validate` (first burst must be `cpu`, pages < 64, ≤ 64 bursts, ≤ 256 refs). Buttons: **Add**, **Add & keep open**, **Random** (seeded generator: N processes with mix of CPU-bound/IO-bound), **Templates** (CPU-bound, I/O-bound, interactive, memory hog).

### Animation (Simulation mode feel)
- Consume observer events into a queue; animate each as a token move along the matching link (≈300 ms at 1× speed, scaled by speed slider). Never block the sim on animation: sim runs ahead, GUI replays from the event queue at a bounded rate.
- Step button advances exactly one notable event (`sim_step_event`); token animates once.
- Event List rows are clickable → pause and highlight the relevant node/process; "scrub back" later via `sim_snapshot`.

### Panels
- **Event List**: scrolling log, filter by kind/pid.
- **Gantt strip**: per-process rows over time, coloured by state; current-time cursor.
- **Metrics**: live avg wait/turnaround/response, CPU util, faults & fault rate, ctx switches, thrashing flag.

## 5. Code layout

```
gui/
  main_gui.c        window loop, mode switching
  gui_state.h       struct gui { sim, layout, selection, mode, anim queue, ui flags }
  canvas.c          nodes, links, pan/zoom, hit-testing, drawing
  nodes.c           per-node draw + inspector content
  builder.c         process builder dialog
  panels.c          event list, gantt, metrics, toolbar
  anim.c            observer → animation queue, token paths
include/prism/sim_api.h, src/engine/sim_api.c          (done)
include/prism/scenario.h, src/engine/scenario.c        (done; save/load with layout)
```
Keep each file < ~500 lines. No sim logic in `gui/`.

Makefile addition (M8):
```
gui: $(STATIC_LIB) $(GUI_OBJ)
	$(CC) $^ -o prism-gui $(shell pkg-config --libs raylib) -lm
```

## 6. Milestones

Tracked with checkboxes in `plan.md` (M7–M11).

## 7. Cut list (drop in this order)
1. Undo/redo  2. Free-form wiring (keep fixed topology)  3. Scrub-back  4. Random/Templates  5. Live scheduler switch (require reset)  6. Zoom  7. Event List filters

**Never cut:** custom process creation, run/pause/step, memory frame view, event list, save/load.

## 8. If a GUI library is not allowed
Keep the TUI and make it interactive: command line at the bottom (`add P5 arrival=3 priority=2 bursts=C8,I3,C4 pages=loop:0:5:2`, `sched rr 4`, `repl lru`, `frames 10`, `step`, `run`, `reset`, `save f.wl`), and arrow-key navigation of panels. The `sim_api` is needed either way, so M7 is never wasted.
