# ipc_bench — Risultati KVM

Hardware: QEMU/KVM i686, single core, 128 MB RAM.
Ogni test: 10000 iterazioni, 100 warmup.
Nota: KVM timing è rumoroso (±0.3–0.5 µs tra run); le colonne "run A/B" sono due run pulite.

---

## feature/55 — Port Lookup Cache (2026-03-08)

### Intra-task (thread-to-thread, stesso task)

| Test          | run A   | run B   | #54 baseline |
|---------------|---------|---------|--------------|
| null RPC      | 1.30 µs | 1.22 µs | 1.45–1.57 µs |
| 128B inline   | 1.65 µs | 1.21 µs | 1.46–1.98 µs |
| 1024B inline  | 1.70 µs | 1.50 µs | 1.35–1.67 µs |
| 4096B inline  | 1.64 µs | 1.62 µs | 1.37–1.47 µs |

### Slow-path receive (DTS — receiver già bloccato su mach_msg_receive_continue)

| Test          | run A   | run B   | #54 baseline |
|---------------|---------|---------|--------------|
| null RPC      | 1.56 µs | 1.34 µs | 1.71–1.82 µs |
| 128B inline   | 1.41 µs | 1.39 µs | 1.34–1.50 µs |
| 1024B inline  | 1.50 µs | 1.38 µs | 1.36–1.53 µs |
| 4096B inline  | 2.11 µs | 1.46 µs | 1.45–1.48 µs |

### Inter-task (task-to-task)

| Test          | run A   | run B   | #54 baseline |
|---------------|---------|---------|--------------|
| null RPC      | 1.80 µs | 1.54 µs | 1.54 µs      |
| 128B inline   | 1.74 µs | 1.59 µs | 1.54–1.59 µs |
| 1024B inline  | 1.78 µs | 1.64 µs | 1.64–1.69 µs |
| 4096B inline  | 2.05 µs | 1.75 µs | 2.34–17 µs * |

\* kalloc zone expansion causa outlier occasionali

### Port operations

| Test               | run A    | run B    | #54 baseline  |
|--------------------|----------|----------|---------------|
| port alloc+destroy | 0.69 µs  | 0.62 µs  | 0.65–0.90 µs  |
| mach_port_names()  | 3.63 µs  | 3.44 µs  | 3.30–4.33 µs  |

### Port cache hit rate

| Test                  | tries  | hits   | misses | hit% |
|-----------------------|--------|--------|--------|------|
| intra null RPC        | 20003  | 20003  | 0      | 100% |
| intra 128B            | 20003  | 20003  | 0      | 100% |
| intra 1024B           | 10003  | 10003  | 0      | 100% |
| intra 4096B           | 10003  | 10003  | 0      | 100% |
| inter null RPC        | 20003  | 20003  | 0      | 100% |
| inter 128B            | 20003  | 20003  | 0      | 100% |
| inter 1024B           | 10003  | 10003  | 0      | 100% |
| inter 4096B           | 10003  | 10003  | 0      | 100% |

---

## Storico per issue

| Issue | Descrizione                          | Intra null RPC | Inter null RPC | Slow null (DTS) |
|-------|--------------------------------------|----------------|----------------|-----------------|
| #42   | baseline (nessuna ottimizzazione)    | ~3.5–4.0 µs    | ~3.5–4.0 µs    | ~3.5–4.0 µs     |
| #52   | kmsg pool LIFO per-CPU               | ~2.0–2.5 µs    | ~2.0–2.5 µs    | ~2.0–2.5 µs     |
| #53   | IPC continuations riabilitate        | ~1.7 µs        | ~1.7 µs        | ~1.7 µs         |
| #54   | Direct Thread Switch                 | 1.45–1.57 µs   | 1.54 µs        | 1.71–1.82 µs    |
| #55   | Per-thread port lookup cache         | 1.22–1.30 µs   | 1.54–1.80 µs   | 1.34–1.56 µs    |

Nota: i valori pre-#52 sono stime conservative; il benchmark non esisteva prima di #42.
