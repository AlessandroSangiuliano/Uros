# Uros SMP IPC/FLIPC2 scaling benchmark — 1→12 CPU (mediana N=3)

**Host**: pavillion — AMD Ryzen 5 4600H (6 core / 12 thread SMT), KVM, QEMU 11.0, GCC 16.1.1.
**Kernel**: branch `feature/302-tsc-bringup-delay` (TSC bring-up delay + SHA-NI stack-align fix).
**Data**: 2026-06-20. `./scripts/run-qemu.sh --bench --ahci -nographic -serial mon:stdio --smp N`, 3 run per N, **mediana**.
**Unità**: µs/op salvo dove indicato `(ns)`; `(ns/RPC)` per concurrent. Lower = faster.

> **Note**
> - Mediana di 3 run (toglie gli outlier del single-run). Sopra i 6 vCPU = oversubscription (host 6c/12t).
> - L'outlier `Intra-task null RPC` ~15µs a smp1/2 è un warmup costante del primo sample di quella suite (resta anche in mediana).
> - Sotto KVM il win lock-free di #331 NON è visibile (no cache-coherence reale → vedi #332 bare-metal).

## IPC — tutte le misurazioni

| Sezione | Metrica | smp1 | smp2 | smp4 | smp6 | smp12 |
|---|---|--:|--:|--:|--:|--:|
| Raw syscall | mach_null (noop trap) | 0.11 | 0.13 | 0.14 | 0.15 | 0.17 |
| | mach_print("") trap | 0.13 | 0.64 | 4.73 | 9.14 | 10.91 |
| Intra-task | null RPC | 15.65 | 18.61 | 4.38 | 5.16 | 7.31 |
| | 128B | 2.29 | 2.28 | 2.91 | 5.54 | 9.19 |
| | 1024B | 2.36 | 2.34 | 3.53 | 5.28 | 7.64 |
| | 4096B | 2.42 | 2.47 | 3.97 | 5.99 | 7.71 |
| Slow-path receive | null RPC | 2.30 | 2.52 | 3.88 | 5.13 | 7.48 |
| | 128B | 2.31 | 2.46 | 3.89 | 5.30 | 7.72 |
| | 1024B | 2.40 | 2.65 | 4.00 | 5.70 | 8.01 |
| | 4096B | 2.93 | 3.09 | 4.01 | 5.96 | 7.48 |
| Inter-task | null RPC | 3.07 | 3.04 | 4.82 | 4.86 | 7.44 |
| | 128B | 3.11 | 3.06 | 4.24 | 4.89 | 6.11 |
| | 1024B | 3.26 | 3.31 | 3.96 | 4.91 | 6.55 |
| | 4096B | 3.33 | 3.44 | 3.90 | 5.03 | 7.22 |
| Hotpath SEND\|RCV | null RPC | 1.40 | 1.45 | 2.26 | 2.90 | 3.84 |
| | 128B | 1.41 | 1.44 | 2.29 | 2.86 | 3.58 |
| | 1024B | 1.45 | 1.54 | 2.37 | 2.87 | 5.79 |
| | 4096B | 1.51 | 1.53 | 2.31 | 2.97 | 5.35 |
| Concurrent same-space (ns/RPC) | x1 | 2260 | 2399 | 3582 | 4576 | 7160 |
| | x2 | 2544 | 2278 | 2294 | 2702 | 3887 |
| | x4 | 2204 | 2244 | 2286 | 2596 | 3087 |
| Port ops | port alloc+destroy | 1.24 | 1.33 | 1.57 | 1.66 | 2.03 |
| | mach_port_names() | 8.57 | 24.36 | 50.47 | 42.52 | 121.27 |
| PP intra | null (no PP) | 2.22 | 2.33 | 3.59 | 5.02 | 7.99 |
| | null (w/ PP) | 2.21 | 2.28 | 3.63 | 5.04 | 8.23 |
| | 128B (no PP) | 2.23 | 2.27 | 3.64 | 5.16 | 7.34 |
| | 128B (w/ PP) | 2.23 | 4.19 | 3.61 | 5.25 | 6.85 |
| | 1024B (no PP) | 2.27 | 2.30 | 3.82 | 5.25 | 9.01 |
| | 1024B (w/ PP) | 2.28 | 2.29 | 3.85 | 5.03 | 8.99 |
| | 4096B (no PP) | 2.35 | 2.35 | 3.39 | 5.57 | 9.76 |
| | 4096B (w/ PP) | 2.35 | 2.35 | 3.33 | 5.37 | 8.80 |
| PP inter | null (no PP) | 3.06 | 3.02 | 4.63 | 4.73 | 6.84 |
| | null (w/ PP) | 3.04 | 3.05 | 4.51 | 4.77 | 6.68 |
| | 128B (no PP) | 3.06 | 3.05 | 4.76 | 4.71 | 6.61 |
| | 128B (w/ PP) | 3.05 | 3.08 | 4.39 | 4.85 | 6.33 |
| | 1024B (no PP) | 3.23 | 3.23 | 4.68 | 5.03 | 7.52 |
| | 1024B (w/ PP) | 3.23 | 3.27 | 4.78 | 5.08 | 8.62 |
| | 4096B (no PP) | 3.32 | 3.37 | 4.62 | 5.40 | 8.35 |
| | 4096B (w/ PP) | 3.34 | 3.34 | 4.80 | 5.25 | 7.65 |
| OOL intra (PHYS_COPY) | 4 KB | 3.92 | 3.92 | 6.13 | 19.98 | 68.95 |
| | 16 KB | 3.91 | 3.94 | 5.71 | 20.69 | 66.82 |
| | 64 KB | 3.92 | 3.91 | 5.82 | 20.54 | 73.97 |
| OOL inter (PHYS_COPY) | 4 KB | 3.37 | 3.44 | 4.84 | 5.15 | 9.44 |
| | 16 KB | 3.35 | 3.39 | 4.85 | 5.13 | 7.72 |
| | 64 KB | 5.24 | 5.16 | 6.66 | 6.91 | 13.07 |
| Disk raw read | 1 KB | 235.62 | 280.76 | 189.67 | 254.49 | 630.30 |
| | 4 KB | 234.14 | 224.61 | 175.06 | 241.73 | 276.88 |
| | 64 KB | 319.31 | 252.68 | 218.11 | 279.71 | 359.33 |
| Disk raw write | 1 KB | 236.59 | 244.92 | 168.27 | 249.29 | 454.58 |
| | 4 KB | 267.04 | 256.25 | 182.44 | 282.49 | 505.37 |
| | 64 KB | 288.93 | 245.56 | 205.12 | 236.36 | 626.47 |
| Disk ext2 | open+close | 806.81 | 908.40 | 997.59 | 985.45 | 1914.41 |
| | open non-existent | 1110.52 | 1251.13 | 1312.96 | 1353.44 | 2751.19 |
| | open+read+close (3 RPC) | 1201.66 | 951.50 | 977.09 | 1035.55 | 1592.02 |
| | open_read+close (2 RPC) | 2859.69 | 1002.08 | 971.09 | 1061.26 | 1685.17 |
| | open_read+read_close (2) | 1758.99 | 5260.26 | 971.15 | 1036.83 | 1771.12 |
| | write+sync (durable) | 671.99 | 762.84 | 640.52 | 818.57 | 4227.19 |
| | write+sync (2 open,1 dirty) | 427.35 | 553.98 | 368.47 | 469.63 | 1279.66 |

## FLIPC v2 — tutte le misurazioni

| Sezione | Metrica | smp1 | smp2 | smp4 | smp6 | smp12 |
|---|---|--:|--:|--:|--:|--:|
| futex vs sem | futex WAKE_WAIT ping-pong | 0.43 | 1.23 | 1.48 | 2.20 | 6.67 |
| | semaphore ping-pong | 2.67 | 3.37 | 4.23 | 5.84 | 9.53 |
| Throughput no-kernel | null desc batch=1 (ns) | 29 | 34 | 34 | 36 | 40 |
| | null desc batch=16 (ns) | 5 | 6 | 6 | 6 | 9 |
| | null desc batch=64 (ns) | 4 | 5 | 5 | 5 | 8 |
| Throughput con dati | 128B prod+cons (ns) | 32 | 38 | 39 | 40 | 42 |
| | 1024B prod+cons (ns) | 46 | 50 | 52 | 55 | 68 |
| | 4096B prod+cons (ns) | 94 | 110 | 110 | 115 | 520 |
| Intra RPC (semaphore) | null | 1.55 | 1.76 | 2.67 | 4.35 | 6.50 |
| | 128B | 1.88 | 2.31 | 3.05 | 5.22 | 6.59 |
| | 1024B | 1.89 | 2.69 | 3.46 | 5.64 | 6.89 |
| | 4096B | 1.99 | 2.71 | 3.45 | 6.06 | 7.27 |
| Inter RPC (vm_remap) | null | 2.53 | 3.19 | 3.78 | 4.84 | 5.37 |
| | 128B | 2.89 | 3.52 | 4.17 | 5.14 | 5.73 |
| | 1024B | 2.92 | 3.54 | 4.55 | 5.38 | 6.66 |
| | 4096B | 3.04 | 3.68 | 4.70 | 5.60 | 9.19 |
| Inter RPC (futex) | null | 2.33 | 2.84 | 3.14 | 4.34 | 10.12 |
| | 128B | 2.74 | 3.30 | 3.49 | 4.78 | 4.81 |
| | 1024B | 2.76 | 3.25 | 3.58 | 4.80 | 5.76 |
| | 4096B | 2.87 | 3.46 | 3.77 | 4.96 | 6.01 |
| Inter BATCH (vm_remap) | batch=1 | 2.46 | 3.18 | 3.81 | 4.58 | 4.60 |
| | batch=16 (ns) | 182 | 221 | 282 | 335 | 397 |
| | batch=64 (ns) | 67 | 78 | 87 | 108 | 129 |
| Game sim | per draw cmd (ns) | 5 | 6 | 6 | 6 | 8 |
| | texture 16KB (ns) | 293 | 344 | 345 | 350 | 434 |
| | audio 4KB PCM (ns) | 94 | 109 | 108 | 111 | 141 |
| Isolated channel | null (isolated intra) | 1.52 | 1.81 | 2.68 | 4.17 | 5.47 |
| | 128B (isolated intra) | 1.86 | 2.65 | 3.15 | 5.01 | 4.80 |
| | null (isolated inter) | 2.50 | 3.16 | 4.02 | 4.21 | 4.35 |
| | 128B (isolated inter) | 2.88 | 3.54 | 4.39 | 5.01 | 5.01 |
| Endpoint | create+destroy | 18.49 | 21.69 | 23.15 | 24.45 | 66.78 |
| | null (endpoint) | 2.46 | 3.15 | 3.76 | 4.66 | 4.78 |
| | 128B (endpoint) | 2.89 | 3.52 | 4.38 | 5.14 | 5.28 |
| Buffer group | alloc+free (ns) | 12 | 13 | 15 | 14 | 14 |
| | 256B (bufgroup) | 1.54 | 2.34 | 3.04 | 4.62 | 5.94 |
| | 256B (bufgroup inter) | 2.49 | 3.21 | 4.02 | 1.51 | 1.88 |

## Lettura

- **Più CPU = più lento per-op**, monotòno su quasi tutto (oversubscription host >6 vCPU + contesa stato condiviso kernel).
- **Peggiore in assoluto: OOL PHYSICAL_COPY intra** (4KB 3.92→68.95µs, ~18×) → segnale per **#338** (split page-table locks, pmap writers).
- `mach_port_names()` ~14× e `concurrent same-space` degradano: path già reso lock-free da **#331**, ma KVM non mostra il win → **#332** (bare-metal).
- **FLIPC2 throughput user-space (no kernel) quasi piatto** (4→8 ns) + **futex hand-off** che scala meglio dei path kernel-mediati → prova empirica della direzione **AMP/multikernel** (meno stato condiviso = scala meglio).
- Misura di scaling pulita (no oversubscription) = **OMEGA (i9 32-core)**.
