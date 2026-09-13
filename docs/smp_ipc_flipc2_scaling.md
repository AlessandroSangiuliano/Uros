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


---

# 2026-09-13 — x86-64, smp1: l'early-out di `spl_replay()` (#454, trovato da #392)

**Host**: pavillion — **la stessa macchina della tabella sopra**, AMD Ryzen 5 4600H (6 core / 12 thread), KVM, QEMU 11.0.
**Kernel**: ramo `feature/454-spl-replay-early-out` = epic `v0.3.0-x86-64` + una guardia in `spl_replay()`.
**Data**: 2026-09-13. `~/uros-tests/431-mediana.sh x64 3 1` — 3 boot per braccio, **mediana**, **entrambe le braccia nella stessa sessione**.
**Condizione**: governor `performance` + `boost=1`, verificato **prima e dopo** ogni campagna; frequenza campionata durante (412 e 408 campioni). A batteria.
**Bundle**: **bench DA SOLO** (`bootstrap.conf` = `name_server` + `ipc_bench`), come le baseline di giugno.
**Unità**: µs/op. Lower = faster.

🔴 **LE TABELLE DELLE FASI SONO IN CICLI DI CPU, NON IN µs.** Il resto di questo
documento e' in µs/op, che e' quello che stampa il bench; la scomposizione per
fase la prende `rdtsc` e sono cicli. A 3.993 MHz, **1000 cicli = 0,25 µs**.

⚠️ E il quanto del TSC in questo guest e' **30 cicli**: ogni cifra qui e' un suo
multiplo, e una colonna a 60 e' *due quanti*, cioe' al pavimento della
risoluzione, non «misurata a 60».


> **Perche' questa tabella esiste**
> Le misure di stamattina dicevano che su `intra`, `inter` e `slow` x86-64 era
> 1,38-1,48× piu' lento di i386. La scomposizione per fase di #392 ha trovato
> il motivo, e non era il messaggio: `splx()` scandiva **240 vettori** a ogni
> abbassamento di livello per scoprire che quattro parole di bit pendenti erano
> zero, e una hand-off lo paga **due volte** (il secondo dentro il controllo
> `THREAD_SWAPPER` del ricevente). La guardia e' quattro load e tre OR.
>
> 🔴 **Le due braccia sono nella stessa sessione**, non «oggi contro stamattina»:
> confrontare due campagne prese in due momenti e' l'errore che ha prodotto,
> oggi stesso, una tabella con ogni numero triplicato perche' il portatile era
> passato al profilo batteria.
>
> ⚠️ **La mediana della frequenza campionata e' 2,43 GHz nel braccio ablato e
> 1,92 nel braccio con la guardia** — il campionatore gira anche nelle pause fra
> un boot e l'altro, quando la macchina e' ferma, e le tira giu' entrambe. Quel
> che conta e' il **verso**: il braccio piu' LENTO e' quello che ha girato al
> clock piu' ALTO, quindi il guadagno non puo' essere un artefatto del clock.

## I controlli, prima dei risultati

Le righe che **non** bloccano e non abbassano il livello di interrupt non si
muovono. Se si fossero mosse, la misura sarebbe stata di qualcos'altro.

| controllo | prima | dopo |
|---|--:|--:|
| `mach_null` (trap nudo) | 0.03 | 0.03 |
| `mach_print("")` | 0.05 | 0.05 |
| `port alloc + destroy` | 0.36 | 0.36 |
| FLIPC2 throughput no-kernel (batch=1) | 0.03 | 0.03 |
| FLIPC2 128B produce+consume | 0.03 | 0.03 |

E il controllo di correttezza, che non e' «le suite sono verdi»: l'autotest dei
differiti del #522 e' l'unico che esercita il replay **con bit pendenti**, ed e'
verde a `-smp 4` — *«lowering replayed 10 of the 10 owed, handler ran 10 more
times»*, tick `99 99 99 99`. Senza quella riga, tutto il resto verde sarebbe
stato compatibile con «il replay non avviene piu'».

## Tutte le misurazioni

| Sezione | Metrica | prima | dopo | Δ |
|---|---|--:|--:|--:|
| #324 futex vs Mach semaphore ping-pong (block+wake round-trip) | futex WAKE_WAIT ping-pong | 1.34 | 0.58 | -57% |
|  | semaphore ping-pong | 4.87 | 1.95 | -60% |
| Combined SEND\|RCV intra-task (hotpath) | 1024B inline RPC | 1.70 | 0.91 | -46% |
|  | 128B inline RPC | 1.67 | 0.91 | -46% |
|  | 4096B inline RPC | 1.71 | 0.99 | -42% |
|  | null RPC | 1.65 | 0.87 | -47% |
| FLIPC2 buffer group benchmarks | 256B RPC (bufgroup inter) | 2.76 | 2.06 | -25% |
|  | 256B RPC (bufgroup) | 1.84 | 0.90 | -51% |
|  | bufgroup alloc+free | 0.01 | 0.01 | +0% |
| FLIPC2 endpoint benchmarks | 128B RPC (endpoint) | 2.80 | 1.95 | -30% |
|  | endpoint create+destroy | 13.62 | 12.40 | -9% |
|  | null RPC (endpoint) | 2.81 | 1.98 | -30% |
| FLIPC2 game simulation (intra-task throughput) | audio 4KB PCM frame | 0.10 | 0.10 | +0% |
|  | per draw command | 0.00 | 0.00 | sotto risoluzione |
|  | texture 16KB chunk | 0.29 | 0.29 | +0% |
| FLIPC2 inter-task BATCH (vm_remap, amortized) | batch=1 (inter) | 2.82 | 1.87 | -34% |
|  | batch=16 (inter) | 0.22 | 0.14 | -36% |
|  | batch=64 (inter) | 0.07 | 0.06 | -14% |
| FLIPC2 inter-task RPC (urmach_futex hand-off) | 1024B RPC (inter futex) | 2.49 | 1.75 | -30% |
|  | 128B RPC (inter futex) | 2.50 | 1.71 | -32% |
|  | 4096B RPC (inter futex) | 2.73 | 1.89 | -31% |
|  | null RPC (inter futex) | 2.38 | 1.90 | -20% |
| FLIPC2 inter-task RPC (vm_remap shared memory) | 1024B RPC (inter) | 2.83 | 1.93 | -32% |
|  | 128B RPC (inter) | 2.80 | 1.90 | -32% |
|  | 4096B RPC (inter) | 2.97 | 2.10 | -29% |
|  | null RPC (inter) | 2.81 | 1.96 | -30% |
| FLIPC2 intra-task RPC (semaphore path) | 1024B RPC (intra) | 1.90 | 0.92 | -52% |
|  | 128B RPC (intra) | 1.85 | 0.91 | -51% |
|  | 4096B RPC (intra) | 2.00 | 1.04 | -48% |
|  | null RPC (intra) | 1.92 | 0.93 | -52% |
| FLIPC2 isolated channel RPC | 128B RPC (isolated inter) | 2.86 | 1.94 | -32% |
|  | 128B RPC (isolated intra) | 1.85 | 0.89 | -52% |
|  | null RPC (isolated inter) | 2.83 | 1.89 | -33% |
|  | null RPC (isolated intra) | 1.83 | 0.89 | -51% |
| FLIPC2 throughput (single-thread, no kernel) | null desc (batch=1) | 0.03 | 0.03 | +0% |
|  | null desc (batch=16) | 0.00 | 0.00 | sotto risoluzione |
|  | null desc (batch=64) | 0.00 | 0.00 | sotto risoluzione |
| FLIPC2 throughput with data | 1024B produce+consume | 0.04 | 0.04 | +0% |
|  | 128B produce+consume | 0.03 | 0.03 | +0% |
|  | 4096B produce+consume | 0.10 | 0.09 | -10% |
| Inter-task (task-to-task) | 1024B inline RPC | 4.43 | 2.54 | -43% |
|  | 128B inline RPC | 4.25 | 2.39 | -44% |
|  | 4096B inline RPC | 4.51 | 2.65 | -41% |
|  | null RPC | 4.23 | 2.36 | -44% |
| Intra-task (thread-to-thread) | 1024B inline RPC | 3.63 | 1.50 | -59% |
|  | 128B inline RPC | 3.41 | 1.51 | -56% |
|  | 4096B inline RPC | 3.49 | 1.53 | -56% |
|  | null RPC | 3.38 | 1.44 | -57% |
| Kernel RPC (where the MIG checks are) | mach_port_type (kernel RPC) | 0.28 | 0.31 | +11% |
| OOL data (inter-task, PHYSICAL_COPY) | 16 KB OOL inter | 4.01 | 2.71 | -32% |
|  | 4 KB OOL inter | 4.19 | 2.75 | -34% |
|  | 64 KB OOL inter | 9.29 | 8.43 | -9% |
| OOL data (intra-task, PHYSICAL_COPY) | 16 KB OOL | 3.25 | 1.92 | -41% |
|  | 4 KB OOL | 3.23 | 1.85 | -43% |
|  | 64 KB OOL | 3.69 | 2.07 | -44% |
| PP inter-task | 1024B (no PP) | 3.87 | 2.55 | -34% |
|  | 1024B (w/ PP) | 4.16 | 2.50 | -40% |
|  | 128B (no PP) | 3.73 | 2.34 | -37% |
|  | 128B (w/ PP) | 3.84 | 2.36 | -39% |
|  | 4096B (no PP) | 3.90 | 2.59 | -34% |
|  | 4096B (w/ PP) | 3.95 | 2.60 | -34% |
|  | null (no PP) | 3.66 | 2.34 | -36% |
|  | null (w/ PP) | 3.67 | 2.43 | -34% |
| PP intra-task | 1024B (no PP) | 2.84 | 1.44 | -49% |
|  | 1024B (w/ PP) | 2.83 | 1.43 | -49% |
|  | 128B (no PP) | 2.97 | 1.42 | -52% |
|  | 128B (w/ PP) | 2.97 | 1.42 | -52% |
|  | 4096B (no PP) | 2.98 | 1.52 | -49% |
|  | 4096B (w/ PP) | 3.05 | 1.49 | -51% |
|  | null (no PP) | 2.77 | 1.40 | -49% |
|  | null (w/ PP) | 2.76 | 1.38 | -50% |
| Port operations | mach_port_names() | 3.37 | 3.53 | +5% |
|  | port alloc + destroy | 0.36 | 0.36 | +0% |
| Raw syscall (no IPC) | mach_null (noop trap) | 0.03 | 0.03 | +0% |
|  | mach_print("") trap | 0.05 | 0.05 | +0% |
| Slow-path receive (continuation path) | 1024B inline RPC (receiver blocked) | 3.42 | 1.46 | -57% |
|  | 128B inline RPC (receiver blocked) | 3.39 | 1.44 | -58% |
|  | 4096B inline RPC (receiver blocked) | 3.55 | 1.50 | -58% |
|  | null RPC (receiver blocked) | 3.34 | 1.44 | -57% |

⚠️ `mach_port_names()` +5% e `mach_port_type` +11% sono le due righe che salgono:
nessuna delle due blocca, entrambe sono dentro il rumore fra boot di questa
suite, e nessuna ha una spiegazione misurata. Restano scritte cosi' invece di
essere arrotondate a zero.

## Concurrent same-space (#327) — etichette a parte

Il bench stampa il tempo grezzo dentro l'etichetta, quindi ogni boot ha una
riga diversa e l'estrattore non le appaia. Mediane sui tre boot, calcolate a
mano dai log:

| | prima | dopo | Δ |
|---|--:|--:|--:|
| x1, 10000 RPC | 33.53 ms | 14.18 ms | −58% |
| x2, 20000 RPC | 68.50 ms | 27.50 ms | −60% |
| x4, 40000 RPC | 136.49 ms | 55.12 ms | −60% |

## Contro la baseline i386 del 20/06 (la tabella in cima a questo file)

Stessa macchina, stesso acceleratore, stesso bundle, stessa metodologia
(mediana di 3). Le quattro righe su cui stamattina x86-64 perdeva:

| riga | i386 20/06 | x86-64 prima | x86-64 dopo | dopo vs i386 |
|---|--:|--:|--:|--:|
| Hotpath SEND\|RCV null | 1.40 | 1.65 | **0.87** | **1,6× piu' veloce** |
| Intra-task 128B | 2.29 | 3.41 | **1.51** | **1,5× piu' veloce** |
| Slow-path null | 2.30 | 3.34 | **1.44** | **1,6× piu' veloce** |
| Inter-task null | 3.07 | 4.23 | **2.36** | **1,3× piu' veloce** |
| futex WAKE_WAIT ping-pong | 0.43 | 1.34 | **0.58** | 1,35× piu' lento (era 3,1×) |
| port alloc + destroy | 1.24 | 0.36 | 0.36 | 3,4× piu' veloce |
| mach_null | 0.11 | 0.03 | 0.03 | 3,7× piu' veloce |

🔑 **Ogni riga su cui x86-64 era piu' lento di i386 ora e' piu' veloce.** La sola
che resta indietro e' il ping-pong futex (#554), e il divario e' passato da
3,1× a 1,35×.

## Dove vanno i cicli — scomposizione per fase (#392)

Lo strumento e' `kern/syscall_profile.{h,c}`, `cmake -DUROS_SYSCALL_PROFILE=ON`,
spento di default. Trap `urmach_msg` sulla hot path mmot, `-smp 1`, KVM,
mediana di 16 trap consecutivi, frequenza 3.993 MHz campionata.

🔴 **Questa e' la tabella da rifare quando arriva il PCID (#412)**: `inter` cambia
spazio di indirizzamento e `intra` no, quindi il PCID deve muovere `SWITCH`
sulla prima e lasciarlo fermo sulla seconda. Se muove entrambe o nessuna, la
premessa del #412 va riletta.

| fase | ablato (cicli) | con guardia (cicli) | cosa chiude |
|---|--:|--:|---|
| entry | 60 | 60 | SYSCALL, swapgs, frame, dispatch |
| get buf | 60 | 60 | `ikm_cache_get` |
| **COPYIN** | **180** | **180** | `copyinmsg` — la copia in invio |
| resolve | 150 | 120 | nome → porta |
| queue | 120 | 150 | diritti, limiti di coda, i due lock |
| **pick rcv** | **690** | **150** | trova e verifica il ricevente (conteneva uno `splx`) |
| claim | 60 | 60 | cambio di stato sotto `thread_lock` |
| park snd | 120 | 120 | il mittente sulla coda di reply |
| deliver | 120 | 120 | consegna il messaggio, prepara lo switch |
| wait | 3150 | 2040 | fuori dal processore — **il tempo dell'altro capo** |
| **SWITCH** | **450** | **450** | `switch_context` + `thread_dispatch` |
| **splx** | **600** | **60** | abbassa il livello di interrupt |
| resume | 60 | 90 | sveglia, `ith_state`, trailer |
| copyout | 210 | 210 | l'header tradotto: porte → nomi |
| **PUT** | **150** | **150** | `copyoutmsg` — la copia in ricezione |
| residue | 60 | 30 | quel che i mark non nominano |
| return | 51 | 66 | ritorno → SYSRET (media, dal percorso in assembly) |
| **totale su processore** | **3141** | **2076** | |

Letture:

- **Il soffitto del #391 (register-IPC) sono le due copie: 330 cicli su 2076, il
  10-12%.** La macchina della hand-off e' il 63-68%.
- **`claim` e' 60 cicli**, cioe' al pavimento dello strumento: il cambio di stato
  dello scheduler — quello di cui parla il #319 — non costa niente. Costava la
  ricerca del ricevente, e dentro c'era uno `splx`.
- **A `-smp 4` non cresce niente**: `pick rcv` 690→660, `SWITCH` 450→420,
  `splx` 600→630. Stessa forma del #482 sulla fault COW a otto processori.
- ⚠️ **Il quanto del TSC in questo guest e' 30 cicli** e un paio di timestamp ne
  costa 60: le quattro colonne a 60 sono **al di sotto della risoluzione**, non
  misurate a 60.
- ⚠️ **17 mark × 60 = 1020 cicli di strumento** su 3141 di soggetto. Dichiarato.
  Ogni fase ne porta uno, quindi la correzione e' per colonna e uniforme.

## Come rifare questa misura

    sudo cpupower frequency-set -g performance -d 1.4GHz -u 4GHz
    sudo sh -c 'echo 1 > /sys/devices/system/cpu/cpufreq/boost'

    cmake -S uros -B uros/build-x86_64 -DUROS_BUNDLE_IPC_BENCH=ON \
          -DUROS_BUNDLE_BENCH_ONLY=ON
    ~/uros-tests/431-mediana.sh x64 3 1      # la campagna, 3 boot, mediana
    ~/uros-tests/mediane.sh <log>...         # mediana per (suite, metrica)
    ~/uros-tests/392-giro.sh 1 kvm 150 tag   # un boot con le fasi e il clock

🔴 **`tlp` e' attivo su questa macchina**: staccando il carica batteria applica
il profilo batteria e riporta il governor a `powersave`. Gli script verificano
la condizione prima e dopo e **rifiutano di misurare** se non combacia — un giro
gia' buttato oggi aveva ogni numero gonfiato di 2,93×, che e' esattamente
4,10/1,40, perche' il TSC e' invariante e il core no.

⚠️ Per misurare una sola suite: `-DUROS_BENCH_SUITES=comb`. Il default resta la
lista piena.


## 2026-09-13 — lo stesso a `-smp 4`, e qui la mediana di 3 NON basta

**Stesse condizioni**, stessa sessione, 3 boot per braccio, `~/uros-tests/431-mediana.sh x64 3 4`.
Governor verificato prima e dopo entrambe le campagne; frequenza campionata (415 e 282 campioni).

🔴 **La riga pulita e' una sola, e le altre vanno lette come non risolte.**

| riga | ablato (min / **mediana** / max) | con guardia (min / **mediana** / max) |
|---|---|---|
| **comb null** | 1.61 / **1.66** / 1.99 | 0.86 / **0.88** / 0.89 |
| inter null | 3.44 / **3.48** / 3.56 | **2.48** / 3.82 / 3.83 |
| intra null | 3.45 / **3.57** / 3.70 | **2.53** / 2.62 / 3.00 |
| slow null | 3.58 / **3.84** / 3.96 | **2.97** / 3.98 / 4.09 |

`comb` riproduce smp1 alla cifra: **−47%**, spread stretto su **entrambe** le braccia.

⚠️ Le altre tre sono **bimodali nel braccio con la guardia**: un boot molto piu'
veloce e gli altri sul modo lento, mentre il braccio ablato e' stretto. E' la
lotteria di piazzamento inter-task gia' registrata (**#356**, **#446**: «inter
bimodale»). Con tre boot la mediana cade da qualunque parte stiano due boot su
tre — quindi **quelle tre righe non dicono ne' meglio ne' peggio**, e i loro
minimi (3.44→2.48, 3.45→2.53, 3.58→2.97) sono l'unica cosa che si muove in modo
coerente. Separarle vuol dire molti piu' boot, non una lettura piu' attenta.

I controlli sono fermi anche qui: `mach_null` 0.03→0.03, `mach_port_type`
0.29→0.29, `mach_port_names()` 30.84→30.88.

### Tutte le misurazioni, `-smp 4`

| Sezione | Metrica | prima | dopo | Δ |
|---|---|--:|--:|--:|
| #324 futex vs Mach semaphore ping-pong (block+wake round-trip) | futex WAKE_WAIT ping-pong | 1.47 | 0.63 | -57% |
|  | semaphore ping-pong | 4.17 | 2.94 | -29% |
| Combined SEND\|RCV intra-task (hotpath) | 1024B inline RPC | 1.75 | 0.95 | -46% |
|  | 128B inline RPC | 1.71 | 0.92 | -46% |
|  | 4096B inline RPC | 1.80 | 1.00 | -44% |
|  | null RPC | 1.66 | 0.88 | -47% |
| FLIPC2 buffer group benchmarks | 256B RPC (bufgroup inter) | 0.91 | 0.83 | -9% |
|  | 256B RPC (bufgroup) | 2.70 | 2.90 | +7% |
|  | bufgroup alloc+free | 0.01 | 0.01 | +0% |
| FLIPC2 endpoint benchmarks | 128B RPC (endpoint) | 2.93 | 2.80 | -4% |
|  | endpoint create+destroy | 18.17 | 16.06 | -12% |
|  | null RPC (endpoint) | 2.89 | 2.65 | -8% |
| FLIPC2 game simulation (intra-task throughput) | audio 4KB PCM frame | 0.10 | 0.10 | +0% |
|  | per draw command | 0.00 | 0.00 | sotto risoluzione |
|  | texture 16KB chunk | 0.29 | 0.29 | +0% |
| FLIPC2 inter-task BATCH (vm_remap, amortized) | batch=1 (inter) | 2.77 | 2.26 | -18% |
|  | batch=16 (inter) | 0.20 | 0.17 | -15% |
|  | batch=64 (inter) | 0.07 | 0.07 | +0% |
| FLIPC2 inter-task RPC (urmach_futex hand-off) | 1024B RPC (inter futex) | 2.66 | 1.87 | -30% |
|  | 128B RPC (inter futex) | 2.47 | 1.88 | -24% |
|  | 4096B RPC (inter futex) | 2.76 | 1.98 | -28% |
|  | null RPC (inter futex) | 2.49 | 1.81 | -27% |
| FLIPC2 inter-task RPC (vm_remap shared memory) | 1024B RPC (inter) | 2.86 | 3.63 | +27% |
|  | 128B RPC (inter) | 2.94 | 3.41 | +16% |
|  | 4096B RPC (inter) | 4.12 | 4.00 | -3% |
|  | null RPC (inter) | 3.01 | 3.52 | +17% |
| FLIPC2 intra-task RPC (semaphore path) | 1024B RPC (intra) | 2.88 | 2.19 | -24% |
|  | 128B RPC (intra) | 3.00 | 2.13 | -29% |
|  | 4096B RPC (intra) | 3.04 | 2.55 | -16% |
|  | null RPC (intra) | 2.94 | 2.53 | -14% |
| FLIPC2 isolated channel RPC | 128B RPC (isolated inter) | 3.00 | 2.93 | -2% |
|  | 128B RPC (isolated intra) | 4.06 | 2.12 | -48% |
|  | null RPC (isolated inter) | 2.98 | 2.72 | -9% |
|  | null RPC (isolated intra) | 4.11 | 2.15 | -48% |
| FLIPC2 throughput (single-thread, no kernel) | null desc (batch=1) | 0.03 | 0.03 | +0% |
|  | null desc (batch=16) | 0.00 | 0.00 | sotto risoluzione |
|  | null desc (batch=64) | 0.00 | 0.00 | sotto risoluzione |
| FLIPC2 throughput with data | 1024B produce+consume | 0.04 | 0.05 | +25% |
|  | 128B produce+consume | 0.03 | 0.03 | +0% |
|  | 4096B produce+consume | 0.09 | 0.10 | +11% |
| Inter-task (task-to-task) | 1024B inline RPC | 3.74 | 4.43 | +18% |
|  | 128B inline RPC | 3.67 | 2.64 | -28% |
|  | 4096B inline RPC | 3.96 | 2.93 | -26% |
|  | null RPC | 3.48 | 3.82 | +10% |
| Intra-task (thread-to-thread) | 1024B inline RPC | 3.80 | 3.03 | -20% |
|  | 128B inline RPC | 3.73 | 4.04 | +8% |
|  | 4096B inline RPC | 4.23 | 4.63 | +9% |
|  | null RPC | 3.57 | 2.62 | -27% |
| Kernel RPC (where the MIG checks are) | mach_port_type (kernel RPC) | 0.29 | 0.29 | +0% |
| OOL data (inter-task, PHYSICAL_COPY) | 16 KB OOL inter | 5.37 | 3.13 | -42% |
|  | 4 KB OOL inter | 6.18 | 3.45 | -44% |
|  | 64 KB OOL inter | 10.62 | 13.73 | +29% |
| OOL data (intra-task, PHYSICAL_COPY) | 16 KB OOL | 4.64 | 4.09 | -12% |
|  | 4 KB OOL | 6.34 | 3.85 | -39% |
|  | 64 KB OOL | 6.67 | 3.97 | -40% |
| PP inter-task | 1024B (no PP) | 4.43 | 2.96 | -33% |
|  | 1024B (w/ PP) | 4.05 | 2.81 | -31% |
|  | 128B (no PP) | 3.57 | 3.86 | +8% |
|  | 128B (w/ PP) | 3.59 | 2.59 | -28% |
|  | 4096B (no PP) | 4.09 | 3.99 | -2% |
|  | 4096B (w/ PP) | 5.80 | 4.65 | -20% |
|  | null (no PP) | 3.57 | 2.67 | -25% |
|  | null (w/ PP) | 3.55 | 2.76 | -22% |
| PP intra-task | 1024B (no PP) | 3.74 | 3.18 | -15% |
|  | 1024B (w/ PP) | 3.81 | 4.27 | +12% |
|  | 128B (no PP) | 3.58 | 2.71 | -24% |
|  | 128B (w/ PP) | 3.75 | 3.86 | +3% |
|  | 4096B (no PP) | 3.87 | 3.95 | +2% |
|  | 4096B (w/ PP) | 3.98 | 3.59 | -10% |
|  | null (no PP) | 3.55 | 2.99 | -16% |
|  | null (w/ PP) | 3.54 | 3.94 | +11% |
| Port operations | mach_port_names() | 30.84 | 30.88 | +0% |
|  | port alloc + destroy | 0.38 | 0.36 | -5% |
| Raw syscall (no IPC) | mach_null (noop trap) | 0.03 | 0.03 | +0% |
|  | mach_print("") trap | 0.05 | 0.05 | +0% |
| Slow-path receive (continuation path) | 1024B inline RPC (receiver blocked) | 4.15 | 4.55 | +10% |
|  | 128B inline RPC (receiver blocked) | 3.67 | 2.82 | -23% |
|  | 4096B inline RPC (receiver blocked) | 4.38 | 3.47 | -21% |
|  | null RPC (receiver blocked) | 3.84 | 3.98 | +4% |

⚠️ Nel braccio con la guardia il **boot 1 e' morto** su
`panic(cpu 1): vm_page_release: page 0x2788000 still mapped (#385)`, quindi le
righe dopo la suite FLIPC2 BATCH hanno meno campioni in quel braccio. Il difetto
e' **preesistente**: la campagna di stamattina, sull'epic senza la guardia, lo ha
prodotto in 1 boot su 3 identicamente. Tre boot per braccio non separano 1/3 da
0/3 su un difetto raro — serve la campagna da venti boot, come quella originale.

## Cosa deve fare il PCID (#412) a questa tabella

`inter` cambia spazio di indirizzamento, `intra` no. Il PCID deve muovere la
colonna `SWITCH` della scomposizione per fase **sulla prima e non sulla
seconda**. Se le muove entrambe, o nessuna, la premessa del #412 va riletta
prima del codice.

⚠️ E prima di misurare il PCID, `-smp 4` va reso leggibile: con la bimodalita'
attuale tre boot non distinguono un guadagno del 20% dal rumore.


## 2026-09-13 (3) — `-smp 4` reso leggibile: il flag `-X` (#356/#446), e la slow path scomposta (#559)

**Stesse condizioni**, stessa sessione, 3 boot per braccio, `~/uros-tests/446-ab.sh 3 4`.
Governor `performance` + `boost=1` verificato prima e dopo. A batteria. Suite `intra slow inter comb`.
**Le due braccia sono la STESSA IMMAGINE, un flag di differenza** — voce GRUB 14 contro 18.

🔴 **E il controllo di presenza e' dentro la misura**: il braccio con `-X` deve
stampare `sched: -X ... is ON`. Visto 0/0/0 nel braccio senza e 1/1/1 in quello
con. Un flag che non prende e un flag che non fa niente sono indistinguibili nei
numeri — e questo flag era **letto da due call site e scritto da nessuno** su
x86-64 fino a oggi.

### La lotteria collassa, boot per boot

| | boot 1 | boot 2 | boot 3 |
|---|--:|--:|--:|
| `intra` null **senza** `-X` | **5.51** | 2.59 | 2.43 |
| `intra` null **con** `-X` | 1.52 | 1.43 | 1.43 |
| `slow` null **senza** | **4.61** | 2.69 | 2.46 |
| `slow` null **con** | 1.35 | 1.37 | 1.35 |

Da una dispersione del 130% fra boot a un numero stabile al 3%. E' il risultato
che **#446 aveva misurato su omen** (±30-43% → ±1%), riprodotto su questa
macchina per la prima volta — perche' fino a oggi il flag qui non era
raggiungibile.

### 🔑 L'esperimento DISCRIMINA, ed e' questo a renderlo una misura

| Sezione | Metrica | prima | dopo | Δ |
|---|---|--:|--:|--:|
| Combined SEND\|RCV intra-task (hotpath) | 1024B inline RPC | 0.92 | 1.04 | +13% |
|  | 128B inline RPC | 0.98 | 0.90 | -8% |
|  | 4096B inline RPC | 0.98 | 1.04 | +6% |
|  | null RPC | 0.87 | 0.90 | +3% |
| Inter-task (task-to-task) | 1024B inline RPC | 2.80 | 3.96 | +41% |
|  | 128B inline RPC | 2.63 | 2.26 | -14% |
|  | 4096B inline RPC | 3.01 | 2.76 | -8% |
|  | null RPC | 2.55 | 2.36 | -7% |
| Intra-task (thread-to-thread) | 1024B inline RPC | 2.91 | 1.42 | -51% |
|  | 128B inline RPC | 4.33 | 1.41 | -67% |
|  | 4096B inline RPC | 3.16 | 1.46 | -54% |
|  | null RPC | 2.59 | 1.43 | -45% |
| Slow-path receive (continuation path) | 1024B inline RPC (receiver blocked) | 2.90 | 1.41 | -51% |
|  | 128B inline RPC (receiver blocked) | 2.68 | 1.41 | -47% |
|  | 4096B inline RPC (receiver blocked) | 3.23 | 1.46 | -55% |
|  | null RPC (receiver blocked) | 2.69 | 1.35 | -50% |

`intra` e `slow` crollano del 45-67%; **`inter` e `comb` no**. E' esattamente
cio' che il gate prevede: la hand-off del #356 e' **same-task only**, quindi
`inter` (task diversi) non la prende, e `comb` non passa dalla run queue perche'
usa la hand-off diretta dentro `mach_msg`. Se fossero calate tutte, si sarebbe
misurato qualcos'altro.

⚠️ Le righe di `inter` oscillano ancora (−14% … +41%): quella lotteria **resta**,
ed e' quella che il PCID (#412) puo' togliere aprendo il gate a cross-task. Il
motivo per cui e' chiuso e' scritto in `sched_prim.c` ed e' un costo di i386:
*«an inter-task switch on one CPU costs a cr3 reload = full TLB flush on i386»*.

### Dove vanno i cicli di un trap della SLOW path (#559)

`-smp 1`, KVM, 3.993 MHz campionati, mediana su una forma sola per finestra.
🔴 **Un round trip della slow path e' DUE trap e sono due forme diverse**: la
send non blocca, la receive si. Una mediana sulle due insieme e' una riga mai
accaduta.

| fase (cicli) | hot path (`comb`, 1 trap) | slow: send | slow: receive |
|---|--:|--:|--:|
| entry | 60 | 60 | 60 |
| `get buf` + `COPYIN` | 60 + 150 | `kmsg_get` 150 | — |
| resolve | 150 | 210 | 210 |
| accodamento / risveglio | queue+pick+claim+park+deliver = 570 | `mq_send` 120 | `mq_recv` 180 |
| `wait` (il peer) | 2100 *(wall)* | — | 2520 *(wall)* |
| **`RUNQ`** | **0** | — | **390** |
| `SWITCH` | 450 | — | 540 |
| `splx` + `resume` | 60 + 90 | — | — |
| `copyout` + `PUT` | 210 + 330 | — | 300 + 150 |
| residue | 30 | 60 | 390 |
| **su processore** | **2211** | **651** | **2271** |

🔑 **`RUNQ` + `SWITCH` = 930 su 2271, il 40%** del lavoro di una receive lenta e'
*essere svegliati e ottenere un processore*. E **`RUNQ` e' esattamente zero sulla
hot path**, spread `[0..0]` su sedici sample: zero dove il meccanismo dice che
deve esserlo, perche' una hand-off diretta non tocca mai una run queue.

⇒ Il round trip a due trap costa **651 + 2271 = 2922** cicli su processore contro
i **2211** della hot path, **+32%**, e la voce singola piu' grossa che li separa
e' una fase che la hot path non ha affatto.

### Quale rotta prende davvero il traffico (#559, copertura)

Un trap che arriva in `ipc/mach_msg.c` prende una di tre strade, e la
scomposizione significa una cosa diversa su ognuna. Un boot, le quattro suite:

| rotta | trap |
|---|--:|
| combinata (hot path) | 65.825, di cui **65.635 handed off** |
| **solo SEND** | **242.413** |
| **solo RCV** | **242.431** |

🔑 **La rotta a due trap e' circa l'88% del traffico `mach_msg`.** Cio' che #392
ha profilato e' la minoranza — ed e' il motivo per cui #559 esiste.

❌ E gli stessi contatori hanno corretto un mio errore: **0 su 242.431** receive
sono riprese in `mach_msg_receive_continue()`. `ipc_mqueue_receive()` passa la
continuation a `thread_block()`, che la invoca solo se lo scheduler scarta lo
stack del kernel — qui non succede mai. I mark li restano (sono giusti per il
caso che la prende) ma la descrizione onesta e' «un braccio che esiste e qui non
si prende».


### E la slow path a quattro processori costa 3× quella a uno — la hot path no

Entrambe le letture con `-X`, quindi **cambia solo il numero di processori**.
`-smp 1` e `-smp 4`, KVM, 3.993 MHz campionati, mediana sugli 8 sample bloccati.

| fase (cicli) | smp1 `-X` | smp4 `-X` | rapporto |
|---|--:|--:|--:|
| entry | 60 | 60 | **1.0** |
| resolve | 210 | 360 | 1.7 |
| `wait` *(wall)* | 2520 | 5045 | 2.0 |
| **`RUNQ`** | **420** | **1405** | **3.3** |
| `SWITCH` | 540 | 1230 | 2.3 |
| `mq_recv` | 210 | 840 | 4.0 |
| `copyout` | 300 | 990 | 3.3 |
| **`PUT`** | **150** | **840** | **5.6** |
| residue | 360 | 1020 | 2.8 |
| **su processore** | **2301** | **6797** | **3.0** |
| di cui «ottenere il processore» | 960 (41%) | 2635 (38%) | |

🔑 **Tre volte, e #392 aveva trovato che la HOT path costa uguale a 1 e a 4.**
Due comportamenti opposti sullo stesso kernel, ora misurati invece che supposti.
La *quota* di «ottenere il processore» invece non si muove: 41% contro 38%.

⚠️ `entry` sta fermo a 60 su entrambe, ed e' il controllo che rende leggibile il
resto: l'ingresso nel trap e' lavoro per-CPU e non deve dipendere da quanti
processori ci sono. Se si fosse mosso, la tabella starebbe misurando il clock o
l'acceleratore.

⚠️ `PUT` a 5,6× e `copyout` a 3,3× sono le due copie verso la memoria utente.
Che sia TLB/cache con i thread che si spostano fra processori e' **plausibile e
non misurato**, e resta scritto cosi' invece che come spiegazione.

## Come si riproduce

    sudo cpupower frequency-set -g performance -d 1.4GHz -u 4GHz
    sudo sh -c 'echo 1 > /sys/devices/system/cpu/cpufreq/boost'

    cmake -S uros -B uros/build-x86_64 -DUROS_BUNDLE_IPC_BENCH=ON \
          -DUROS_BUNDLE_BENCH_ONLY=ON -DUROS_BENCH_SUITES="intra slow inter comb"
    ~/uros-tests/446-ab.sh 3 4        # A/B del flag -X, 3 boot per braccio
    ~/uros-tests/392-giro.sh 1 kvm 240 tag 18   # un boot con le fasi, voce 18

🔴 **La voce GRUB non e' stabile**: 14 = bundle ordinario, 18 = lo stesso con
`-X`. Si chiede a `uros/src/mach_kernel/x86_64/boot/grub.cfg`, dove le voci
nuove vanno **in coda** proprio perche' inserirne una in mezzo rinumera in
silenzio ogni invocazione che ne nomina una.

⚠️ Il profilo per fase e' `-DUROS_SYSCALL_PROFILE=ON`, **spento di default**, e
va letto solo per le FASI: con i dump accesi un `comb` null RPC legge 51 µs
contro 2,21, perche' le printf stampano dentro il loop.
