# Design notes

This document explains how each queue works and, more importantly, *why* it is
correct. It is written to be the study guide for the code — read it alongside
the headers in `include/lfq/`.

## 1. The shared vocabulary

All four queues are **bounded ring buffers**. A fixed-size array of slots is
indexed by a monotonically increasing counter; the slot for a given index is
`index & (capacity - 1)`. Capacity is rounded up to a power of two so that
`& mask` replaces an expensive modulo.

Two properties matter throughout:

- **Lock-free**: at least one thread always makes progress; no mutex can put a
  thread to sleep holding up everyone else.
- **Wait-free** (SPSC only here): *every* operation finishes in a bounded number
  of steps regardless of what other threads do — no retry loops at all.

### Memory ordering in one paragraph

The C++ memory model lets us say "these writes happen-before those reads" without
a lock. A **release** store publishes everything the thread wrote before it; an
**acquire** load that reads that value sees all of those writes. Every queue here
uses exactly one release/acquire pair per hand-off:

```
producer: construct element   --(release store of the index)-->
consumer: (acquire load of the index)  then read the element
```

Because the element is constructed *before* the release store, and read *after*
the acquire load, the consumer can never observe a half-constructed element.

## 2. SPSC — `spsc_queue.hpp`

One producer owns `tail_`, one consumer owns `head_`. Each side only ever writes
its own index, so there is no CAS anywhere — this is what makes it wait-free.

- `try_push` constructs the element, then does a **release** store of `tail+1`.
- `try_pop` does an **acquire** load of `tail`, reads the element, then a
  **release** store of `head+1` to free the slot.

Two performance details:

- **Cache-line isolation.** `tail_` and `head_` are `alignas(cache line)` so the
  producer's writes and the consumer's writes never land on the same line. Sharing
  a line would cause *false sharing*: each store invalidates the other core's copy
  and the line ping-pongs between cores.
- **Cached opposite index.** The producer keeps `cached_head_`, a stale copy of
  the consumer's index, and only re-reads the real (shared) `head_` when the cache
  says the queue might be full. On the common path it never touches the consumer's
  cache line at all. The consumer does the symmetric thing with `cached_tail_`.

## 3. MPSC — `mpsc_queue.hpp`

Now many producers share one enqueue position, so a plain store is not enough —
two producers could grab the same slot. Each slot carries its own
`std::atomic<size_t> seq` (a sequence counter), following Dmitry Vyukov's cell
scheme.

Producer:

1. Read the enqueue index `pos`.
2. Look at `slot[pos].seq`. If `seq == pos`, the slot is free for this round:
   CAS the enqueue index from `pos` to `pos+1` to claim it. If the CAS fails,
   another producer won; retry with the new `pos`.
3. Write the element and **release**-store `seq = pos + 1`, which signals "ready
   to read".

Consumer (single, so no CAS needed on the dequeue index):

1. Look at `slot[pos].seq`. If `seq == pos + 1`, the element is ready.
2. Read it, then **release**-store `seq = pos + capacity`, marking the slot free
   for the *next* lap through the ring. Advance the dequeue index.

The sequence numbers double as full/empty detection and eliminate the ABA
problem: a slot is only reusable when its `seq` has advanced a full lap.

## 4. MPMC — `mpmc_queue.hpp`

Identical to MPSC on the producer side, but now consumers also contend, so the
consumer CAS-advances the dequeue index the same way producers advance the
enqueue index. This is the full Vyukov bounded MPMC queue. With multiple
consumers there is no single global order to observe, so correctness is defined
as **conservation**: every value pushed is popped exactly once — no loss, no
duplication. The N×M test in `tests/test_mpmc.cpp` verifies exactly that.

## 5. Cross-process transport — `shm_transport.hpp`

The same SPSC ring, but the header (head/tail indices, capacity) and the slot
array live in a **named shared-memory region** mapped into two processes. Because
everything is index-based and the element type must be trivially copyable, the
layout is valid no matter where each process maps it. Backends:

- POSIX: `shm_open` + `ftruncate` + `mmap` (link `-lrt` on Linux).
- Windows: `CreateFileMapping` + `MapViewOfFile`.

The atomics live in shared memory, so the same acquire/release reasoning as the
in-process SPSC queue carries over across the process boundary.

## 6. Measuring latency — `timing.hpp`, `histogram.hpp`

- **Timing** defaults to `std::chrono::steady_clock` (monotonic, portable). An
  rdtsc-style counter would be faster but needs per-CPU calibration and behaves
  differently under emulation, so it is deliberately avoided on the hot path.
- **Histogram** buckets values on a log2 scale with a configurable number of
  sub-buckets per octave (an HdrHistogram-style layout). This gives bounded
  relative error and O(1) `record`, so percentiles can be read without storing
  every sample.

Averages hide tail spikes; the whole point is to look at p99 / p99.9 and the max.

## 7. Attributing the tail — `tools/latency_scope`

The microscope runs the paced SPSC latency loop and, whenever a sample lands in
the tail, snapshots the OS involuntary-context-switch counter
(`getrusage(RUSAGE_THREAD)` on Linux). Tail spikes that line up with a context
switch are **scheduler preemption**, not queue overhead. On a shared, non-tuned
machine the tail is dominated by these; on an isolated Linux box with pinned,
`isolcpus` cores it shrinks dramatically. Windows does not expose the same
counter, so there the tool reports the worst outliers and defers counter-level
attribution to Linux.

## 8. Validation strategy

Three layers:

1. **Unit + concurrency tests** per queue (FIFO, full/empty, wraparound,
   multi-threaded conservation).
2. **Differential testing** (`tests/test_reference.cpp`): millions of randomized
   push/pop operations run against a trivially-correct `std::queue` oracle; any
   divergence in accept/reject or value is a failure. Matching a reference on
   tens of millions of operations is strong correctness evidence.
3. **Sanitizers in CI**: ThreadSanitizer catches data races the tests might miss;
   ASan/UBSan catch memory and undefined-behavior bugs.
