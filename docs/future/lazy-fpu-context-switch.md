# Lazy FPU Context Switch — Possibile Implementazione Futura

## Motivazione

Ad ogni context switch il kernel salva e ripristina l'intero stato FPU/SSE/AVX
via FXSAVE/XSAVE. Con XSAVE completo (AVX-512) il buffer può arrivare a ~2 KB.
Per thread che non usano mai la FPU (kernel threads, loop IPC puro) questo è
spreco puro. Il meccanismo lazy elimina save/restore quando non necessario.

Misurazione di riferimento: sull'ipc_bench attuale (~500k msg/s su KVM) il
salvataggio FXSAVE da solo pesa stimati 80-150 cicli per switch.

## Meccanismo

Il processore ha il flag `CR0.TS` (Task Switched). Quando è settato, qualsiasi
istruzione FPU/SSE/AVX genera una trap `#NM` (Device Not Available, vettore 7).

Flusso lazy:

```
context_switch(old, new):
    salva registri interi di old
    if fpu_owner == old:
        FXSAVE/XSAVE → old->pcb->fps   # salva solo se necessario
        fpu_owner = NULL
        CR0.TS = 1                       # arma la trap per new
    ripristina registri interi di new
    # FPU di new non viene ripristinata qui

#NM handler (primo uso FPU del thread corrente):
    clts                                 # disarma CR0.TS
    if current->pcb->fps.initialized:
        FXRSTOR/XRSTOR ← current->pcb->fps
    else:
        FINIT                            # primo uso assoluto
    fpu_owner = current
    iret
```

## File coinvolti

| File | Modifica necessaria |
|------|---------------------|
| `i386/AT386/model_dep.c` | set `CR0.TS` dopo `fpu_init()` |
| `i386/fpu.c` | `fpu_save()`, `fpu_switch()`, handler `#NM` |
| `i386/fpu.h` | `kernel_fpu_begin()` / `kernel_fpu_end()` |
| `i386/AT386/model_dep.c` | registrare handler vettore 7 |
| `kern/thread.c` | `thread_dispatch()` → set `CR0.TS` se cambio thread |
| `i386/pcb.c` | campo `fps.initialized` in `pcb_t` |

L'infrastruttura parziale esiste già: `pcb->ims.ifps` e `fp_kind` in `i386/fpu.h`.

## Problema critico: FPU nel kernel

A partire dall'issue #43, `kern/printf.c` usa aritmetica FPU per la conversione
`%f/%e/%g`. Con `CR0.TS` settato, un `printf("val=%f", x)` nel kernel causa `#NM`.

Soluzioni possibili (in ordine di preferenza):

### Opzione A — Conversione float puramente intera in printf (consigliata)
Riscrivere la conversione `%f/%e/%g` in `kern/printf.c` senza istruzioni FPU,
usando solo aritmetica intera (come fa `lib/vsprintf.c` di Linux). Elimina il
problema alla radice. Costo: ~200-300 righe di codice integer-only per Grisu/Dragon4
semplificato.

### Opzione B — kernel_fpu_begin / kernel_fpu_end
Aggiungere wrapper che brackettano ogni uso FPU nel kernel:

```c
/* i386/fpu.h */
static inline void kernel_fpu_begin(void) {
    clts();               /* CR0.TS = 0 */
    /* eventuale FXSAVE del thread corrente se fpu_owner != NULL */
}
static inline void kernel_fpu_end(void) {
    stts();               /* CR0.TS = 1 */
}
```

Richiede disciplina: ogni sito nel kernel che usa FPU deve essere marcato.
Attualmente solo `kern/printf.c` (blocco float) ne ha bisogno.

## Problema SMP

Con NCPUS > 1, se un thread migra da CPU-A a CPU-B mentre ha stato FPU "dirty"
su CPU-A, CPU-A deve salvare lo stato prima che CPU-B possa ripristinarlo.
Richiede IPI (Inter-Processor Interrupt) di sincronizzazione:

```
cpu_A: context_switch(old=T, new=X)
    if fpu_owner[cpu_A] == T && T migra su cpu_B:
        FXSAVE → T->pcb->fps
        fpu_owner[cpu_A] = NULL
        CR0.TS[cpu_A] = 1
    # cpu_B può ora FXRSTOR liberamente
```

Non rilevante finché Uros gira su singolo core.

## Signal delivery

In `sendsig()` (consegna segnale a userspace), lo stato FPU va incluso nel
signal frame. Con lazy save, se `fpu_owner == current` ma il salvataggio non
è ancora avvenuto, bisogna forzarlo prima di copiare `pcb->fps` nel frame:

```c
/* prima di costruire il signal frame */
if (fpu_owner == current_thread()) {
    fxsave(&current_thread()->pcb->fps);
    fpu_owner = NULL;
    stts();
}
```

## Stima del guadagno atteso

| Scenario | Cicli risparmiati per switch |
|----------|------------------------------|
| Thread kernel (no FPU) | ~100-200 (FXSAVE eliminato) |
| Thread user senza FPU attiva | ~100-200 |
| Thread user con FPU attiva | 0 (save/restore comunque) |
| ipc_bench (IPC puro) | ~15-25% latenza stimata |

## Prerequisiti prima di implementare

1. Riscrivere `kern/printf.c` float senza FPU (Opzione A) oppure aggiungere
   `kernel_fpu_begin/end` (Opzione B).
2. Leggere e mappare completamente `i386/fpu.c` — l'infrastruttura esistente
   potrebbe essere già parzialmente lazy o fare assunzioni incompatibili.
3. Aggiungere test di regressione FPU (calcolo float in userspace verificabile
   dopo molti context switch) prima e dopo la modifica.

## Riferimenti

- Intel SDM Vol. 3A, §13.5 — XSAVE/XRSTOR, §2.5 — CR0.TS
- Linux `arch/x86/kernel/fpu/core.c` — implementazione lazy reference
- OSF Mach `i386/fpu.c`, `i386/fpu.h` — scaffolding esistente
