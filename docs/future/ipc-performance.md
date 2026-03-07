# IPC Performance — Analisi e Possibili Implementazioni Future

## Stato attuale

L'ipc_bench misura ~500k msg/s su KVM (single core, messaggi inline piccoli).
Il path critico per ogni messaggio è:

```
userspace: mach_msg_trap (SYSENTER)
kernel:    ipc_kmsg_alloc()
           ipc_kmsg_copyin()          # copia dati dal task mittente
           ipc_mqueue_send()          # accoda o trasferisce
           -- context switch --
           ipc_mqueue_receive()       # ricevente si sveglia
           ipc_kmsg_copyout()         # copia dati nel task ricevente
           ipc_kmsg_free()
userspace: ritorno da mach_msg_trap (SYSEXIT)
```

Ogni round-trip coinvolge 2 SYSENTER/SYSEXIT, 2 allocazioni kmsg, 2 bcopy,
2 port lookup con lock, e almeno 1 context switch completo (con FXSAVE/FXRSTOR).

---

## 1. Continuations — Analisi dello stato nel codebase

### L'infrastruttura esiste già

OSF Mach ha un supporto continuations quasi completo ma **disabilitato**:

- `thread_t.continuation` (kern/thread.h:387) — campo per il function pointer
- `thread_block(continuation)` (kern/sched_prim.c:1632) — accetta continuation
- `Switch_context` (i386/cswitch.S:180) — salva continuation nel thread e
  supporta il resume tramite `Thread_continue` (cswitch.S:220)
- `call_continuation` (i386/locore.S:1186) — jump diretto alla continuation
- `ipc_mqueue_receive` (ipc/ipc_mqueue.c:958) — accetta parametro continuation

### Come è neutralizzata

In `thread_block_reason()` (kern/sched_prim.c:1520-1528):

```c
thread->at_safe_point = (int) continuation;   /* salva come flag */
/* ... */
continuation = (void (*)(void)) 0;            /* AZZERATA */
```

La continuation viene usata solo come tag per il meccanismo "safe point"
(abort/halt sicuro durante IPC). Poi viene forzata a zero. Di conseguenza
`thread_invoke` passa sempre 0 a `switch_context`:

```c
old_thread = switch_context(old_thread, 0, new_thread);  /* continuation = 0, SEMPRE */
```

In `cswitch.S`, quando continuation è 0, il kernel fa il salvataggio completo
dei registri callee-saved e dello stack pointer — come se le continuations non
esistessero.

Inoltre, il valore passato dall'IPC è `SAFE_EXTERNAL_RECEIVE` (= -3), non
un vero function pointer — è un sentinella per thread_act.c, non una
continuazione eseguibile.

### Perché è stata disabilitata

Probabile causa: il modello thread-activation di OSF Mach (thread_shuttle +
thread_activation split) è più complesso di CMU Mach 3.0. Il safe-point
mechanism controlla se un thread può essere interrotto durante una receive.
Implementare continuation vere richiedeva separare correttamente lo stato
tra shuttle e activation — lavoro che non è stato completato.

### Cosa serve per riabilitarle

1. **Non azzerare la continuation** in `thread_block_reason` — propagarla
   fino a `thread_invoke` → `switch_context` → `Switch_context`

2. **Scrivere `ipc_mqueue_receive_continue()`** — funzione reale che:
   - Riacquista `imq_lock(mqueue)`
   - Fa `reset_timeout_check` se necessario
   - Legge `self->wait_result` e `self->ith_state`
   - Completa il receive (copyout, seqno, ecc.)
   - Ritorna a userspace via `return_from_trap`

3. **Passare una vera continuation** (non -3) da `mach_msg.c` e `ipc_mig.c`

4. **Gestire abort/interrupt** — se un thread con continuation riceve
   TH_ABORT, il kernel deve chiamare la continuation con errore, non fare
   stack unwind (non c'è stack)

### Rischi e svantaggi

| Rischio | Dettaglio |
|---------|-----------|
| **Debugging** | Thread bloccato con continuation non ha kernel stack; backtrace impossibile in DDB e kgdb. Bisogna estendere db_trace per mostrare `thread->continuation` |
| **Activation model** | Ogni activation ha il suo PCB; il thread_shuttle migra tra activations. La continuation deve essere salvata/ripristinata correttamente durante i migrate |
| **Timer** | `reset_timeout_check(&self->timer)` dopo `thread_block` (ipc_mqueue.c:1019) deve essere replicato nella continuation |
| **Safe-point interaction** | Il meccanismo at_safe_point usa la continuation come flag. Riabilitarla richiede separare i due usi |
| **Regressioni sottili** | Il codice post-block in `ipc_mqueue_receive` (righe 1018-1029) gestisce wait_result e lock re-acquire; tutto deve essere replicato senza bug nella continuation |
| **Test coverage** | Servono test di regressione IPC estensivi prima e dopo — l'ipc_bench attuale non copre timeout, abort, port death |

### Stima impatto

Con continuation attive sul path IPC sincrono, lo switch non salva/ripristina
registri kernel e non mantiene lo stack allocato. Stima conservativa: 30-50%
riduzione latenza IPC round-trip.

---

## 2. kmsg Pool Dedicato

### Cos'è
`ipc_kmsg_alloc` chiama `kalloc(sizeof(ipc_kmsg_t) + msg_size)` per ogni
messaggio. `kalloc` è una general-purpose allocator con lock globale. Per
messaggi frequenti e di piccola dimensione questo è overhead costante.

Un pool pre-allocato di `ipc_kmsg_t` + buffer inline fino a una soglia (es.
256 byte) permette allocazione O(1) lockless (con per-CPU free list).

### Struttura

```c
/* ipc/ipc_kmsg.h */
#define IPC_KMSG_POOL_SIZE      512     /* messaggi pre-allocati per CPU */
#define IPC_KMSG_INLINE_MAX     256     /* soglia per pool vs kalloc */

struct ipc_kmsg_pool {
    ipc_kmsg_t  *freelist;              /* stack LIFO lockless */
    int          count;
    char         _pad[CACHE_LINE - sizeof(ipc_kmsg_t*) - sizeof(int)];
} __attribute__((aligned(64)));

/* una struttura per CPU */
PERCPU_DEFINE(struct ipc_kmsg_pool, ipc_kmsg_pool);
```

### Flusso

```c
ipc_kmsg_t ipc_kmsg_alloc(mach_msg_size_t msg_size) {
    if (msg_size <= IPC_KMSG_INLINE_MAX) {
        kmsg = percpu_pool_pop();
        if (kmsg) return kmsg;
    }
    /* fallback a kalloc per messaggi grandi o pool esaurito */
    return kalloc(sizeof(ipc_kmsg_t) + msg_size);
}

void ipc_kmsg_free(ipc_kmsg_t kmsg) {
    if (kmsg->ikm_size <= IPC_KMSG_INLINE_MAX) {
        if (percpu_pool_push(kmsg)) return;
    }
    kfree(kmsg, sizeof(ipc_kmsg_t) + kmsg->ikm_size);
}
```

### Stima guadagno
- `kalloc` su path caldo: ~50-100 cicli con lock.
- Pool hit: ~5-10 cicli (load + store su free list).
- Su 500k msg/s con 2 alloc/msg: ~50-100M cicli/s risparmiati.

### File coinvolti

| File | Modifica |
|------|----------|
| `ipc/ipc_kmsg.c` | `ipc_kmsg_alloc()`, `ipc_kmsg_free()` |
| `ipc/ipc_kmsg.h` | definizioni pool, soglia |
| `kern/cpu_data.h` | aggiungere `ipc_kmsg_pool` a `cpu_data_t` |
| `kern/startup.c` | inizializzare il pool per ogni CPU |

### Rischi e svantaggi
- Memoria fissa pre-allocata anche quando l'IPC è idle
  (512 × ~300 byte = ~150 KB per CPU, accettabile).
- Con SMP, lo steal tra CPU aggiunge complessità. Per ora (single core)
  non rilevante.
- Pool exhaustion sotto carico: il fallback a kalloc garantisce correttezza
  ma perde il beneficio.

---

## 3. Zero-Copy per Messaggi Grandi

### Cos'è
Per dati OOL (out-of-line), il kernel copia con bcopy. Per dati grandi
(> soglia) è più efficiente trasferire la ownership delle pagine fisiche
dal task mittente al ricevente senza copiare byte.

### Meccanismo esistente

OSF Mach ha `vm_map_copy_t` con tre strategie interne:
- `VM_MAP_COPY_ENTRY_LIST` — lista di vm_map_entry (grandi OOL)
- `VM_MAP_COPY_OBJECT` — vm_object con pagine (CoW)
- `VM_MAP_COPY_KERNEL_BUFFER` — bcopy in buffer kernel (piccoli)

Il problema: `ipc_kmsg_copyin_body` forza `VM_MAP_COPY_KERNEL_BUFFER`
anche per dati che beneficerebbero del page remapping. La soglia va
calibrata e il path entry_list completato.

### Soglia consigliata

```c
/* ipc/ipc_kmsg.c */
#define IPC_KMSG_ZERO_COPY_THRESHOLD    (4 * PAGE_SIZE)   /* 16 KB */

if (ool_size >= IPC_KMSG_ZERO_COPY_THRESHOLD)
    vm_map_copyin(map, addr, size, TRUE /* destroy src */, &copy);
else
    /* bcopy classico */
```

### File coinvolti

| File | Modifica |
|------|----------|
| `ipc/ipc_kmsg.c` | `ipc_kmsg_copyin_body()`, `ipc_kmsg_copyout_body()` |
| `vm/vm_map.c` | verificare `vm_map_copyin` con `destroy=TRUE` |

### Rischi e svantaggi
- Semantica "move": il mittente perde le pagine dopo il send.
- CoW introduce page fault lazy nel ricevente al primo write.
- Bug nel trasferimento ownership corrompe la VM di uno dei due task.

---

## 4. Port Lookup Cache per-Thread

### Cos'è
`ipc_port_translate_send(space, name)` fa hash table lookup + `ip_lock`
ad ogni operazione. Cache per-thread degli ultimi N (name → ipc_port_t*).

### Struttura

```c
/* kern/thread.h */
#define IPC_PORT_CACHE_SIZE     4

struct ipc_port_cache_entry {
    mach_port_name_t    name;
    ipc_port_t         *port;
    ipc_space_t        *space;      /* per invalidazione */
};

struct thread {
    /* ... */
    struct ipc_port_cache_entry port_cache[IPC_PORT_CACHE_SIZE];
};
```

### Invalidazione
La cache va svuotata quando:
- La porta viene distrutta (`ipc_port_destroy`)
- Il name viene deallocato dallo space
- Il thread cambia task/space

### Rischi e svantaggi
- 64 byte extra per thread (trascurabile).
- Invalidazione scorretta → use-after-free. Serve generation counter
  o check di validità prima di usare la cache.
- Complessità nel path di distruzione porta.

---

## 5. Direct Thread Switch (Fast Path Send→Receive)

### Cos'è
Quando il mittente chiama send e il destinatario è già bloccato in receive,
il kernel trasferisce il messaggio e fa context switch diretto, bypassando
lo scheduler.

### Flusso

```
mach_msg_send (client):
    if (receiver bloccato su questa porta):
        trasferisci kmsg direttamente a receiver->ith_kmsg
        Switch_context(client, receiver)   /* switch immediato */
```

### Codice candidato nel codebase

`mach_msg.c:1269` già controlla se il receiver è in `SAFE_EXTERNAL_RECEIVE`:

```c
if (!(((receiver->at_safe_point == SAFE_EXTERNAL_RECEIVE) || ...
```

Questo check è il punto di innesto naturale per il direct switch.

### File coinvolti

| File | Modifica |
|------|----------|
| `ipc/ipc_mqueue.c` | `ipc_mqueue_send()` — detect receiver pronto |
| `kern/sched_prim.c` | `thread_handoff(old, new)` |
| `i386/cswitch.S` | supporto handoff |

### Rischi e svantaggi
- Su SMP, direct switch ha senso solo se mittente e destinatario sono
  sulla stessa CPU.
- Bypassa la policy di scheduling (il receiver potrebbe avere priorità
  inferiore). Da decidere se è accettabile.
- Finestra critica in cui entrambi i thread cambiano stato atomicamente.

---

## Priorità e Roadmap Suggerita

| # | Ottimizzazione | Impatto | Complessità | Dipendenze |
|---|---------------|---------|-------------|------------|
| 1 | kmsg pool | alto | bassa | nessuna |
| 2 | Continuations (riabilitare) | molto alto | media-alta | analisi safe-point |
| 3 | Direct thread switch | alto | media | beneficia di continuations |
| 4 | Port lookup cache | medio | media | nessuna |
| 5 | Zero-copy OOL | medio | alta | vm_map_copy stabile |

**Sequenza consigliata**: kmsg pool → continuations → direct switch.

Le continuations esistono nel codice ma vanno riabilitate con cura: il
meccanismo safe-point va separato, la continuation IPC va scritta, e i
tool di debugging (DDB, kgdb) vanno estesi. Non è lavoro da zero ma
richiede attenzione ai dettagli del modello activation-shuttle.

## Stima target post-ottimizzazione

| Configurazione | msg/s stimati |
|----------------|---------------|
| Attuale (baseline) | ~500k |
| + kmsg pool | ~600k |
| + continuations | ~1.2M |
| + direct switch | ~2M |
| + port cache | ~2.2M |

Stime conservative, basate su dati CMU Mach 3.0 / Mach 4 su hardware
comparabile (i486/Pentium). Hardware moderno con cache grandi e branch
predictor potrebbe dare risultati migliori.

## Riferimenti

- Draves, R. et al. — "The Case for User-level Resource Management" (1993)
- Bershad, B. et al. — "Lightweight Remote Procedure Call" (SOSP 1989)
- Liedtke, J. — "On Micro-Kernel Construction" (SOSP 1995)
- OSF Mach: `ipc/ipc_mqueue.c`, `kern/sched_prim.c`, `kern/mach_msg.c`
- Mach 4 (Utah): implementazione continuations in `kern/thread.h`
