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
- [ ] GUI Development (Deferred to the very end after all testing is complete)

---

## Extensions (Ranked in order)
1. **Resource manager + deadlock detection** — resource types, request/release, wait-for graph, detection + recovery (kill/rollback). Natural fit: processes gain `acquire R` / `release R` bursts.
2. **Fault injection** — random process crash / page corruption / I/O failure with a seed (basic process kill & RAM shrink implemented).
3. **IPC + sync** — message queue and semaphore, producer-consumer workload.
4. **Checkpoint/restore** of `struct sim` (flat snapshot already built, disk serialization).
5. **GUI Frontend** (Web / graphical interface after core test validation).

## Weekly checkpoint
End of each week: milestone checkboxes ticked, `make test` green, one new demo-able thing you can show.
