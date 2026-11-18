# PRISM — Project Plan (6 weeks)

**Goal:** a polished, demo-able C simulator with a live dashboard. Required: CPU scheduling + virtual memory. Quality over quantity: a small system that is correct, tested, and shows the scheduler/VM interaction beats a big one that half-works.

Rule: finish milestones in order. Tick boxes as you go. Do not touch extensions until Milestone 6 is done.

---

## Milestone 1 — Core engine (Week 1)
Foundation: events, processes, workload parsing, first scheduler, headless output.
- [x] Flat pointer-free engine (`sim_t`), ticks, FNV-1a log hash, determinism — `tests/test_prism.c`
- [x] Workload loading: parse configuration, bursts, and page traces
- [x] Process lifecycle: burst advance, page references, snapshot & rewind (`sim_snapshot`, `sim_restore`)
- [x] Scheduler ops registry (`sched_ops_t`) + FCFS, Round Robin (`src/sched/schedulers.c`)
- [x] Main engine loop (`sim_step` / `sim_run`): arrivals, dispatches, bursts, I/O blocks/wakes, page fault service
- [x] CLI & metrics (`prism-cli`): `--workload`, `--sched`, `--repl`, `--frames`, `--csv`, Gantt chart
- [x] Hand-check baseline scheduling and determinism verification in automated test suite

**Status:** Completed.

## Milestone 2 — Full scheduler suite (Week 2)
- [x] `sched_fcfs.c` (FIFO queue)
- [x] `sched_rr.c` (Round Robin with configurable quantum)
- [x] `sched_sjf.c` (Shortest Job First, non-preemptive with shortest burst selection)
- [x] `sched_srtf.c` (Shortest Remaining Time First, preemption on arrival/wake)
- [x] `sched_mlfq.c` (3 levels: Q0=2, Q1=4, Q2=8, demotion on expiry, periodic priority boost)
- [x] `sched_adaptive.c` (starvation aging + thrashing/fault-rate demotion)
- [x] `sched_cfs.c` (Completely Fair Scheduler: virtual runtime `vruntime` tracking, min-vruntime selection, nice weighting)
- [x] `sched_bfs.c` (Brainfuck Scheduler / BFS: single-runqueue earliest effective virtual deadline with latency target)
- [x] Context-switch tracking + counter across all dispatches
- [x] Gantt trace history output (per-core timeline chart)
- [x] `--csv` export mode and multi-scheduler comparison benchmark (`--exp1` comparing all 8 schedulers)
- [x] Automated unit tests verifying textbook SJF, SRTF, CFS, and BFS behaviors (`test_sjf_srtf_schedulers`, `test_cfs_bfs_schedulers`)

**Status:** Completed.

## Milestone 3 — Virtual memory core (Week 3)
- [x] `vm`: per-process page tables (`page_table_entry_t`), physical frame table (`frame_entry_t`)
- [x] `repl_fifo.c`, `repl_lru.c` ops implementations
- [x] Engine integration: per-tick page access; fault blocks process with `disk_latency`; frame allocation/eviction; scheduler context-switches to ready processes
- [x] Metrics: total accesses, page faults, fault rate %, per-process fault stats
- [x] `tests/test_prism.c`: textbook page replacement verification (FIFO=15, LRU=12, OPT=9 on classic reference string)
- [x] `tests/test_prism.c`: Belady's anomaly verified (FIFO: 3 frames = 9 faults vs 4 frames = 10 faults)

**Status:** Completed.

## Milestone 4 — Replacement policies + experiments (Week 4)
- [x] `repl_clock.c` (Second-chance algorithm with reference bits and circular clock hand)
- [x] `repl_opt.c` (Belady's Optimal page replacement looking ahead in reference strings)
- [x] Thrashing detection & simulation under constrained memory footprints
- [x] Experiment runners: `--exp2` (Belady's anomaly & policy comparison), `--exp3` (RAM shrinking & thrashing)

**Status:** Completed.

## Milestone 5 — Dashboard (Week 5)
- [x] ncurses terminal dashboard (`prism-tui`)
- [x] Live panels: CPU cores status, real-time Gantt timeline, process state table, memory frame allocation grid, aggregate metrics
- [x] Interactive controls: Step (`S`), Run/Pause (`SPACE`), Rewind (`B`), Cycle Scheduler (`M`), Cycle Replacement (`P`), Kill Process (`K`), Shrink RAM (`D`), Quit (`Q`)
- [x] Headless mode (`prism-cli`) for headless execution, tests, benchmarks, and CI

**Status:** Completed.

## Milestone 6 — Polish, report, buffer (Week 6)
- [x] `make debug` clean (AddressSanitizer + UndefinedBehaviorSanitizer leak-free)
- [x] Determinism verified via FNV-1a event hash invariance
- [x] Flat memory snapshot & rewind verified
- [x] Extended test coverage for new schedulers (SJF, SRTF, CFS, BFS)
- [x] Documentation & workload guide updates in README.md
- [x] GUI Development → Milestones 7–10 and 12 done (`prism-gui`), M11 polish mostly done

---

## Milestone 7 — Interactive core API (see `GUI_PLAN.md` §3)
- [x] `sim_api.[ch]`: `proc_spec_t`, spawn (arrives now) / remove (holes, pids never reused) / update, `sim_reset`, live `sim_set_sched` / `sim_set_repl`, `sim_set_frames`, `sim_run_until`, `sim_step_event`
- [x] `libprism.a` holds the whole core; `make test` builds and runs every `tests/test_*.c`; header dependency tracking in Makefile
- [x] Observer (`sim_set_observer`) fires for every trace event in trace order, survives reset/load, never affects the hash
- [x] Scenario save/load with inline `layout` lines (`scenario.[ch]`); old workload files still load; saved files still run in `prism-cli`
- [x] `tests/test_api.c` (11 tests) green; `make debug` clean
- [ ] (optional) TUI reworked to use the observer

**Status:** Completed.

## Milestone 8 — GUI shell  (done)
- [x] raylib window, toolbar, palette, canvas with pan/zoom/grid
- [x] Place/drag/delete nodes; auto links; validation messages
- [x] Inspector shows selected node fields (read-only first)

## Milestone 9 — Process builder & editing  (done)
- [x] Builder dialog with burst editor + validation; edit existing process
- [x] Scheduler/Memory/CPU/Disk node controls wired to `sim_set_*`
- [x] Save/Load buttons; Random + Templates

## Milestone 10 — Run mode & animation  (done)
- [x] Run/pause/step/speed; reset
- [x] Token animation along links from observer events; memory grid updates + evict flash
- [x] Event List, Gantt strip, Metrics panels
- [x] **Inject process mid-run**

## Milestone 11 — Polish & demo
- [x] Clean `make debug` run, no leaks (core + GUI under ASan/UBSan); determinism test through the API (`tests/test_metrics.c`, GUI `--back`)
- [x] Preset scenarios menu (lists `workloads/*.txt`) — [x] dedicated presets: `convoy`, `starvation`, `interactive_mix`, `thrashing`, `locality`
- [x] Screenshots for report (`docs/prism-gui.png`, `docs/prism-compare*.png`, `--shot`); [ ] GIF; demo script: build scenario live → run → inject process → switch FIFO↔LRU → show thrashing

## Milestone 12 — Compare & learn (GUI overhaul)
- [x] Core: `metrics.[ch]` — `sim_metrics_compute` (one definition of every metric), `sim_prepare_variant` (rewound, observer-free copy with other policies / quantum / frames), `sim_run_bounded`; `tests/test_metrics.c` checks textbook answers (FCFS 8.75, SJF 7.75, SRTF 6.5, RR 11.75; FIFO 15 / LRU 12 / OPT 9; Belady 9 -> 10)
- [x] Compare view: algorithms x test cases, metric bar chart with "best on most cases", results table, per-algorithm Gantt charts on one time axis
- [x] Page replacement compare: faults-vs-frames curve with Belady detection, textbook frame table per policy
- [x] Explain: per-event narration with the algorithm's reason (who else was waiting and their key), caption over the canvas, algorithm cards in the inspector
- [x] Step back / click a timeline to rewind (deterministic replay); textbook CPU Gantt chart; per-process results table
- [x] Fixes: Save no longer overwrites bundled presets; failed startup load no longer leaves an uninitialised sim; clicks inside bottom panels (event rows, timelines) work again

## Milestone 13 — Labs view + reference-string lab
- [x] Core `refstring.[ch]`: runs a reference string through the real engine and records per-step frames, hits, victims; textbook test (FIFO 15 / LRU 12 / Clock 14 / OPT 9)
- [x] Trace import: valgrind lackey records or one address per line, pages renumbered by first use (`refstr_load_trace`)
- [x] GUI: third top-level view **Labs** (Tab cycles Simulate → Compare → Labs); reference-string tab with presets, side-by-side frame tables, step cursor with per-policy eviction reasons, drop a trace file

## Milestone 14 — Disk scheduling lab
- [x] Core `disk.[ch]`: FCFS, SSTF, SCAN, C-SCAN, LOOK, C-LOOK behind an ops table; textbook totals as tests (640 / 236 / 236 / 382 / 299 / 322)
- [x] GUI tab: request queue, head, direction; one seek diagram per policy, request-by-request cursor with the reason for each move

## Milestone 15 — Contiguous memory allocation lab
- [x] Core `contig.[ch]`: first / best / worst / next fit, free with hole merging, compaction, external fragmentation stats; tests
- [x] GUI tab: request script, memory bar per step, failed requests, fragmentation numbers

## Milestone 16 — Address translation lab
- [x] Core `paging.[ch]`: split an address into level indexes + offset, table sizes, internal fragmentation, multi-level walk; tests
- [x] GUI tab: address bits / page size / levels; bit-split bar, translation walk diagram, TLB with LRU and EAT, table memory vs flat table

## Milestone 17 — Semaphores + classic sync problems
- [x] Engine counting semaphores (`semaphore NAME VALUE`, bursts `wait:NAME` / `signal:NAME`, aliases `P:` / `V:`), FIFO waiter queue, `PROC_BLOCKED_SEM`, stall detection (`sim_is_stalled`, `EVENT_STALLED`)
- [x] Presets `producer_consumer`, `dining_philosophers` (deadlocks), `dining_philosophers_fixed`, `readers_writers`; tests in `tests/test_sync.c`
- [x] GUI: Sync tab (values, waiter queues, resources, who waits for what, deadlock banner), narration for wait/signal/wake/stall

## Milestone 18 — Real-time scheduling
- [ ] Periodic tasks (period, exec, jobs), deadline misses, Rate Monotonic + EDF schedulers

## Milestone 19 — Multi-core scheduling
- [ ] Per-core ready queues, CPU affinity, load balancing (pull migration), migration counts

## Milestone 20 — Process tree
- [ ] fork / wait bursts, parent-child links, zombies and orphans (re-parented to init), tree view

---

## Extensions (Ranked in order)
- [x] **Extension 1: Resource manager + deadlock detection** — multi-resource tracking, mutual exclusion, Wait-For Graph (WFG) cycle detection, automatic Checkpoint Rollback recovery (`sim_add_resource`, `sim_acquire_resource`, `sim_release_resource`, `sim_detect_deadlock`, `sim_recover_deadlock_rollback`).
- [x] **Extension 2: Fault injection** — dynamic process crash/kill (`sim_kill_process`), dynamic physical memory shrink fault (`sim_shrink_frames`).
- [x] **Extension 3: IPC + sync** — deterministic mailbox message queues (`sim_ipc_send`, `sim_ipc_recv`), blocking wait-for-message states (`PROC_BLOCKED_IPC`), synchronous wakeups on message arrival.
- [x] **Extension 4: Checkpoint / Restore** — per-process snapshot slots (`sim_checkpoint_create`, `sim_checkpoint_restore`) + full simulation state serialization & rewind.
- [x] **Extension 5: GUI Frontend** — `prism-gui` (Milestones 8–10, 12 compare & explain; M11 polish: GIF + demo script open).

## Weekly checkpoint
End of each week: milestone checkboxes ticked, `make test` green, one new demo-able thing you can show.
