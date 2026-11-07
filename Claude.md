# PRISM

Read this first, then `plan.md` (what to build, in order) and `ARCHITECTURE.md` (how it fits together).

## What this is
PRISM = Process, Resource & Interprocess Simulation Model. An **event-driven OS simulator** for an Operating Systems course project, with a live dashboard.

**Required scope (graded):** CPU scheduling + virtual memory management.
**Everything else** (resources/deadlock, IPC, checkpoint/restore, fault injection) is an **extension**. Do NOT start extensions until every core milestone in `plan.md` is checked off.

## Hard constraints
- **Pure C (C11).** No C++, no Python in the build, no third-party libraries. libc + math only. The only allowed extra is `ncurses` for the dashboard (Milestone 5), and only if the course allows it.
- **Deterministic.** The simulation uses a virtual clock (`tick_t`). Never read wall-clock time inside simulation logic. All randomness goes through a seeded PRNG in `workload`, so the same input + seed gives identical output.
- Simulator core (`sim`, `sched`, `vm`, `event`, `metrics`) must have **zero UI dependency**. The UI only reads state and calls `sim_step()` / `sim_run()`.
- Headless mode (`--no-ui`) must always work. Tests depend on it.

## Build & test
```
make            # build ./prism
make debug      # -O0 -g with ASan + UBSan
make test       # builds and runs every tests/test_*.c
make clean
./prism --workload workloads/basic.wl --sched rr --quantum 4 --repl lru --frames 8 --no-ui
```

## Code conventions
- C11, `-Wall -Wextra -Wpedantic` must stay clean. Fix warnings, never silence them.
- Naming: `module_verb_noun()`, types end in `_t` or are `struct name`. Headers have include guards.
- Pluggable policies use **function-pointer ops tables** (`struct sched_ops`, `struct repl_ops`). Adding a policy = one new `.c` file + one line in the registry. Do not add `switch` on policy name in core code.
- Check every `malloc`/`fopen`. On failure print to stderr and exit non-zero.
- Free everything; `make debug` runs must be leak-clean (`ASAN_OPTIONS=detect_leaks=1`).
- Keep files focused; if a `.c` passes ~400 lines, split it.
- Comments explain *why*, not *what*. Each module header documents its contract.

## How to work
1. Do **one milestone at a time**, in the order in `plan.md`.
2. Write the test first (or alongside) for anything with a known right answer (textbook scheduling examples, Belady's anomaly).
3. When a milestone is done: tests pass, `make debug` clean, tick its boxes in `plan.md`, add a line to the decision log below if you made a design choice.
4. If a decision in `ARCHITECTURE.md` seems wrong, stop and raise it instead of silently deviating.
5. Commit small, with messages like `sched: add SRTF with preemption on arrival`.

## Decision log
- Dashboard is a terminal UI (ncurses) so the whole project stays in C. An optional HTML trace viewer is an extension only if the teacher accepts a non-C viewer.
- Page faults block the faulting process for `fault_service` ticks, which is how scheduling and virtual memory interact.
