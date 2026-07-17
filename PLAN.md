# PLAN.md — `lockfree-spsc`: a lock-free message-passing library, end to end

A from-scratch, rigorously validated lock-free queue family for C++ on Linux,
built to the same bar as a serious systems portfolio project. This document is
the **complete end-state plan** — the full "five-pillar" target. All eight
milestones (M1–M8) in the Roadmap are now implemented; each carries a short
"Delivered" note where the implementation makes a pragmatic, honest trade-off.

---

## 0. Scope, ethics & compliance (READ FIRST)

This is a **personal project**, and it is kept scrupulously clean of anything
work-related. These rules are non-negotiable and apply to every milestone:

- **Personal time and personal equipment only.** No moonlighting; no work done
  on the clock or with employer resources.
- **No employer confidential information** of any kind —
  no internal source code, internal tools, internal designs, internal
  documents, or internal processes.
- **No data from any employer or product.** Specifically **none** of:
  customer data, consumer data, telemetry data, production data, logs,
  internal datasets, or anything derived from them.
- **All data in this project is synthetic** — generated in-process by the code
  itself (monotonic sequence numbers and wall-clock timestamps). There are no
  external data files, no downloads of proprietary data, and no PII.
- **Only public, open-source dependencies** and **only publicly published,
  citable academic papers/specs** are used as references.
- **No proprietary algorithms.** Everything implemented here is from public
  computer-science literature and standard C++.
- The project follows applicable **employer policies on personal / outside
  projects**; it is unrelated to the author's job responsibilities and does not
  build on or compete with any employer product.

If any milestone below cannot be done without touching any of the above, it is
dropped — the constraint wins, always.

---

## 1. What this project is

A small, dependency-free C++20 library of **lock-free queues** for
message passing between threads (and later, processes), with **first-class
measurement**: it does not just move messages fast, it explains *why* the
latency tail looks the way it does.

Target audience for the write-up: high-performance / core-systems engineering.
Everything is benchmarkable and reproducible with one command.

---

## 2. The five pillars (the quality bar)

This project is considered "done to caliber" only when all five hold:

| # | Pillar | Concretely means |
|---|--------|------------------|
| 1 | **Real, correct semantics** | SPSC, MPSC, and MPMC queues with proven no-loss / no-duplication / no-reordering guarantees and documented memory-ordering arguments. |
| 2 | **Hard performance numbers** | Throughput *and* full latency **distribution** (p50…p99.99, max), measured with a disciplined harness (pinned cores, warm-up, isolated CPUs, high-resolution timing). |
| 3 | **Algorithmic sophistication** | Wait-free SPSC; Vyukov-style MPMC bounded queue; cache-line isolation; cached indices; batched claim; optional huge-page backing. |
| 4 | **Rigorous testing** | 40+ unit + concurrency stress tests, edge cases, and clean runs under ThreadSanitizer, AddressSanitizer, and UBSan in CI. |
| 5 | **Ground-truth validation** | Correctness cross-checked against known-good reference queues on identical workloads, and published benchmark results reproduced within a stated tolerance. |

Pillar 5 is the differentiator most projects skip; it is a hard requirement here.

---

## 3. Architecture (end state)

```
include/lfq/
  spsc_queue.hpp     # single-producer / single-consumer (wait-free)   [M1: done]
  mpsc_queue.hpp     # multi-producer / single-consumer                [M3: done]
  mpmc_queue.hpp     # multi-producer / multi-consumer (Vyukov bounded)[M4: done]
  shm_transport.hpp  # cross-process transport over shared memory      [M5: done]
  timing.hpp         # portable nanosecond timing + overhead calibration[M2: done]
  histogram.hpp      # HDR-style latency histogram                     [M2: done]

tests/               # correctness + concurrency stress + sanitizers
bench/               # throughput + latency-distribution benchmarks
tools/
  latency_scope/     # the "latency microscope" (jitter attribution)   [M6: done]
docs/                # design notes, memory-ordering reasoning          [M8: done]
```

### Design principles
- **Header-only core**, standard C++20, no OS dependency in the data structures.
- **Mechanical sympathy**: producer/consumer state on separate cache lines;
  power-of-two capacity with bitmask indexing; cached opposite-end indices.
- **Portability**: builds with MSVC, GCC, and Clang. OS-specific bits (thread
  pinning, shared memory, huge pages) are isolated and feature-guarded.

---

## 4. Roadmap (milestones)

Milestones are ordered by value and by teachability. Each ends with: tests
green, sanitizers clean, benchmark numbers recorded in `docs/`, and a README
section written.

### M1 — Wait-free SPSC queue  ✅
- Bounded SPSC ring buffer; acquire/release publication; cache-line padding;
  cached indices.
- FIFO / full / empty / wraparound / capacity-rounding tests + a
  multi-million-message concurrency stress test.
- Throughput + one-way latency-distribution benchmark (low load).
- CMake build, MSVC build script, and CI (build/test + sanitizer matrix).

### M2 — Measurement you can trust (Pillar 2)  ✅
> Delivered with a portable `steady_clock` timer plus measured per-call overhead
> subtraction, and an HdrHistogram-style bucketed histogram. A calibrated TSC
> path is noted as a portable-risk optimization and intentionally left out of the
> hot path (differs under emulation).
- `timing.hpp`: TSC (`rdtsc`/`cntvct`) reads calibrated to nanoseconds against a
  steady clock; fall back to `steady_clock` where unavailable.
- `histogram.hpp`: HDR-style histogram so percentiles are cheap and accurate.
- Harness upgrades: CPU isolation guidance (`isolcpus`, `taskset`), warm-up,
  multiple runs with variance reported.
- Deliverable: a latency methodology doc explaining *how* the numbers are taken.

### M3 — MPSC queue  ✅
- Multiple producers, single consumer (e.g. intrusive Michael-Scott-style or
  Vyukov MPSC). New stress tests with N producer threads.
- Benchmark: throughput vs. producer count; contention behavior.

### M4 — MPMC bounded queue  ✅
- Vyukov bounded MPMC queue (per-slot sequence numbers).
- Stress tests: N producers × M consumers, verify conservation (every item
  consumed exactly once). ThreadSanitizer clean.

### M5 — Cross-process transport (Pillar 1, extended)  ✅
> Both backends implemented and `#ifdef`-guarded. Verified at runtime on Windows;
> the POSIX `shm_open`/`mmap` path is exercised by CI on Linux.
- `shm_transport.hpp`: the ring buffer placed in **named shared memory**
  (`shm_open`/`mmap` on Linux; `CreateFileMapping`/`MapViewOfFile` on Windows),
  so two separate processes exchange messages with zero copies.
- Robustness: layout/versioning header, alignment, graceful detach.

### M6 — The "latency microscope" (the signature feature)  ✅
> Delivered with tail-sample capture and context-switch attribution via
> `getrusage(RUSAGE_THREAD)` on Linux (no privileges required). Deeper counter
> sources (`perf_event_open`/eBPF, ETW) and a jitter flamegraph remain as future
> extensions; on Windows the tool reports the worst outliers and defers
> counter-level attribution to Linux.
- `tools/latency_scope`: run the queue under **saturated** load and attribute
  every tail spike to a *cause* — page fault, TLB miss, context switch,
  hardware interrupt, frequency change — using OS counters
  (`perf_event_open` / eBPF on Linux; ETW on Windows).
- Output: a **jitter flamegraph** and a table linking p99.9 events to causes.
- This is what turns "here is a fast queue" into "here is *why* the tail is what
  it is" — the actual senior-systems conversation.

### M7 — Ground-truth validation (Pillar 5)  ✅
> Delivered as differential testing against a `std::queue` oracle (22M+ randomized
> ops, zero divergences) plus a lock-free-vs-mutex throughput comparison. Keeping
> the oracle in-repo avoids external dependencies in CI; reproducing a specific
> published third-party benchmark on documented hardware remains a future add-on.
- Correctness: replay identical synthetic workloads through this library and
  through reference open-source queues; assert byte-for-byte identical output
  ordering/conservation.
- Performance: **reproduce a published lock-free-queue benchmark** (e.g. a
  well-known SPSC/MPMC result) on documented hardware and report agreement
  within a stated tolerance, with an honest analysis of any gap.

### M8 — Write-up & portfolio polish  ✅
> `docs/design.md` covers the algorithms and memory-ordering reasoning; the README
> carries results tables and honest tail caveats. Charts/badges are optional polish.
- `docs/`: design notes, memory-ordering proofs, roofline / mechanical-sympathy
  reasoning, and a short blog-style post with charts.
- README: results tables, build matrix badges, and a "what I learned / what
  surprised me" section (the interview script).

---

## 5. Benchmark methodology (target)

- Pin producer/consumer to fixed, ideally **isolated** cores.
- Discard a warm-up phase; run multiple trials; report median **and** spread.
- Report the **distribution**, never just the average: p50, p90, p99, p99.9,
  p99.99, and max — plus a histogram.
- Two regimes, reported separately and never conflated:
  1. **Low load** (paced producer): hand-off latency.
  2. **Saturated** (producer flat out): throughput and backlog/tail behavior.
- Record exact CPU model, core layout, compiler, flags, and OS in every result.

---

## 6. Correctness strategy (target)

- **Unit tests** for FIFO order, full/empty boundaries, wraparound, capacity
  rounding, and (later) move-only element types.
- **Concurrency stress tests**: producers emit a known sequence; consumers
  assert exactly-once, in-order delivery over many millions of messages.
- **Sanitizers in CI**: ThreadSanitizer (data races), AddressSanitizer (memory
  errors), UBSan (undefined behavior) — all must pass.
- **Reference cross-check** (M7): identical results vs. known-good queues.

---

## 7. Explicit non-goals

- Not a general concurrency framework; queues only.
- No unbounded/allocating queues in the core hot path (bounded ring buffers).
- No networking/RDMA (shared-memory IPC is the transport boundary).
- No dependency on any proprietary, internal, or employer-specific technology,
  data, or process — see Section 0.

---

## 8. How to build & run (current)

```powershell
# Windows (MSVC), from the repo root:
.\build.ps1 -Run
```

```bash
# Linux / macOS with CMake:
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
./build/bench_spsc
```
