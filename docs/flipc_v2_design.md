# FLIPC v2 — Design Document

## 1. Obiettivo

Canale IPC ad alte prestazioni per comunicazioni hot-path tra server/driver
trusted su Uros.  Complementare a Mach IPC (sicurezza, capability), non
sostitutivo.

**Target**: centinaia di nanosecondi per messaggio; zero kernel trap nel fast
path.

**Casi d'uso**:
- ext2_server ↔ ahci_driver (comandi I/O disco)
- client ↔ ext2_server (read/write ad alto throughput)
- game engine ↔ GPU driver (command buffer + texture streaming)
- audio server ↔ client (buffer bassa latenza)
- qualsiasi coppia di server trusted con comunicazione frequente

## 2. Principi architetturali

1. **Setup via Mach IPC, data via shared memory pura.**
   Il kernel alloca la regione condivisa, la mappa in entrambi i task, fornisce
   un semaforo Mach per il wakeup.  Dopo il setup, sender e receiver comunicano
   *senza* coinvolgere il kernel nel data path.

2. **Dual plane: control + data.**
   - *Control plane*: ring di descriptor piccoli (64 byte) — comandi, ack,
     metadati.  Lock-free, nel ring buffer shared.
   - *Data plane*: pagine mappate direttamente (COW o mapping read-only) —
     blocchi disco, texture, audio buffer.  Zero-copy.

3. **Adaptive wakeup.**
   Spin-poll → fallback semaforo Mach.  Il fast path non entra mai nel kernel.

4. **Niente Message Engine kernel.**
   A differenza del FLIPC v1, il kernel non partecipa al routing dei messaggi.
   Il routing è diretto: un canale collega esattamente due endpoint (punto a
   punto).

## 3. Concetti fondamentali

### 3.1 Channel

Un **channel** è la primitiva FLIPC v2.  Connette esattamente due task:
un *producer* e un *consumer*.  Ogni channel contiene:

- Una regione di shared memory mappata in entrambi i task
- Un **descriptor ring** (control plane)
- Un **data region** opzionale (data plane, pagine condivise)
- Stato di sincronizzazione (puntatori, flag wakeup)

Un task può avere più channel aperti (es. ext2_server ha un channel verso
ahci_driver e N channel verso i client).

### 3.2 Descriptor

Un **descriptor** è un record a 64 byte nel ring.  Contiene:

```c
struct flipc2_desc {
    uint32_t    opcode;         /* tipo di operazione                    */
    uint32_t    flags;          /* flag specifiche per opcode            */
    uint64_t    cookie;         /* ID operazione (per correlazione req/reply) */
    uint64_t    data_offset;    /* offset nella data region (0 = no data) */
    uint64_t    data_length;    /* lunghezza dati in byte                */
    uint64_t    param[3];       /* parametri opcode-specific             */
    uint32_t    status;         /* stato completamento (nelle reply)     */
    uint32_t    _reserved;      /* allineamento a 64 byte               */
};
```

**64 byte = 1 cache line**.  Il ring non contiene payload, solo metadati.
I dati bulk sono referenziati tramite `data_offset` + `data_length` nella
data region.

### 3.3 Data Region

Porzione della shared memory (o pagine mappate separatamente) dove risiedono
i dati bulk.  Due modalità:

- **Inline**: la data region è parte della shared memory del channel.
  Il producer scrive dati a `data_offset`, il consumer li legge.
  Adatto per messaggi medi (< qualche KB).

- **Page-mapped**: il producer registra pagine dal proprio address space
  tramite una chiamata di setup (Mach IPC).  Il kernel le mappa read-only
  nel consumer.  Adatto per dati grandi (texture, blocchi disco).
  Il descriptor contiene l'offset nella regione mappata.

### 3.4 Adaptive Wakeup

```
PRODUCER                              CONSUMER
   │                                     │
   │  write descriptor nel ring          │  spin-poll sul ring
   │  WRITE_FENCE()                      │  (busy loop, N iterazioni)
   │  advance tail pointer               │
   │                                     │  ← vede nuovo descriptor
   │                                     │     → processa, zero trap
   │                                     │
   │  ┌─ se sleeping_flag == 1 ──┐       │  ┌─ spin timeout ──────┐
   │  │  semaphore_signal()      │       │  │  set sleeping_flag=1│
   │  │  (unico kernel trap)     │       │  │  WRITE_FENCE()      │
   │  └──────────────────────────┘       │  │  check ring again   │
   │                                     │  │  semaphore_wait()   │
   │                                     │  │  clear sleeping_flag│
   │                                     │  └─────────────────────┘
```

- **Fast path** (consumer attivo/spinning): zero kernel trap.
- **Slow path** (consumer idle): un kernel trap per il wakeup (semaphore_signal).
- Il consumer ri-controlla il ring *dopo* aver settato `sleeping_flag` per
  evitare race condition (lost wakeup).

## 4. Layout memoria del Channel

La shared memory di un channel ha layout fisso:

```
Offset 0                                           Offset = channel_size
┌──────────────┬──────────────┬─────────────────────┐
│  Channel     │  Descriptor  │     Data Region     │
│  Header      │  Ring        │  (inline buffers)   │
│  (256 byte)  │  (N × 64 B) │                     │
└──────────────┴──────────────┴─────────────────────┘
```

### 4.1 Channel Header (256 byte)

```c
struct flipc2_channel_header {
    /* --- costanti (set al setup, immutabili) --- */
    uint32_t    magic;              /* FLIPC2_MAGIC = 0x464C5032 ("FLP2") */
    uint32_t    version;            /* versione protocollo                */
    uint32_t    channel_size;       /* dimensione totale shared memory    */
    uint32_t    ring_offset;        /* offset del ring dall'inizio        */
    uint32_t    ring_entries;       /* numero slot nel ring (potenza di 2)*/
    uint32_t    data_offset;        /* offset data region                 */
    uint32_t    data_size;          /* dimensione data region             */
    uint32_t    desc_size;          /* sizeof(flipc2_desc), per estensibilità */

    /* --- producer-owned (scritti dal producer, letti dal consumer) --- */
    volatile uint32_t   prod_tail;      /* prossimo slot da scrivere     */
    uint32_t            _pad_prod[15];  /* padding a cache line          */

    /* --- consumer-owned (scritti dal consumer, letti dal producer) --- */
    volatile uint32_t   cons_head;      /* prossimo slot da leggere      */
    volatile uint32_t   cons_sleeping;  /* 1 = consumer bloccato su sem  */
    uint32_t            _pad_cons[14];  /* padding a cache line          */

    /* --- semaforo Mach (set al setup) --- */
    mach_port_t         wakeup_sem;     /* semaphore port per wakeup     */
    uint32_t            _pad_sem[15];

    /* --- statistiche (best-effort, non sincronizzate) --- */
    volatile uint64_t   prod_total;     /* descriptor prodotti           */
    volatile uint64_t   cons_total;     /* descriptor consumati          */
    volatile uint64_t   wakeups;        /* numero di semaphore_signal    */
    uint8_t             _reserved[232 - 3*8]; /* fino a 256 byte totali  */
};
```

`prod_tail` e `cons_head` sono su cache line separate per evitare false
sharing.

### 4.2 Descriptor Ring

Array circolare di `ring_entries` descriptor (potenza di 2).  Indice
calcolato con maschera:

```c
#define FLIPC2_RING_IDX(ptr, hdr)  ((ptr) & ((hdr)->ring_entries - 1))
```

Il ring è pieno quando `prod_tail - cons_head == ring_entries`.
Il ring è vuoto quando `prod_tail == cons_head`.

I puntatori `prod_tail` e `cons_head` sono monotonicamente crescenti (wrappano
a 32 bit).  Questo elimina l'ambiguità full/empty dei ring buffer classici.

### 4.3 Data Region

Spazio libero dopo il ring.  Gestito dal producer con un allocatore semplice
(bump allocator circolare o free list).  Il consumer non scrive nella data
region — la legge soltanto.

Per dati page-mapped, `data_offset` nel descriptor punta alla regione di
pagine mappate separatamente (non alla data region inline).  Un bit in `flags`
distingue i due casi.

## 5. API

### 5.1 Setup (slow path — via Mach IPC)

```c
/* Crea un channel. Restituisce un handle e il send right per invitare il peer. */
flipc2_return_t
flipc2_channel_create(
    uint32_t            channel_size,   /* dimensione totale (min 4KB, max 16MB) */
    uint32_t            ring_entries,   /* slot nel ring (potenza di 2)          */
    flipc2_channel_t   *channel,        /* [out] handle locale                   */
    mach_port_t        *invite_port     /* [out] send right da passare al peer   */
);

/* Accetta un invito e si connette al channel (peer). */
flipc2_return_t
flipc2_channel_accept(
    mach_port_t         invite_port,    /* send right ricevuto dal creator       */
    flipc2_channel_t   *channel         /* [out] handle locale                   */
);

/* Chiude il channel e rilascia le risorse. */
flipc2_return_t
flipc2_channel_destroy(
    flipc2_channel_t    channel
);

/* Mappa pagine del producer nel consumer per zero-copy data plane. */
flipc2_return_t
flipc2_channel_map_pages(
    flipc2_channel_t    channel,
    vm_offset_t         source_addr,    /* indirizzo nel producer                */
    vm_size_t           size,           /* dimensione mapping                    */
    uint64_t           *region_id       /* [out] ID regione per i descriptor     */
);
```

### 5.2 Produce / Consume (fast path — user-space puro)

```c
/* Ottieni il prossimo slot libero nel ring (NULL se ring pieno). */
static inline struct flipc2_desc *
flipc2_produce_reserve(flipc2_channel_t channel);

/* Pubblica il descriptor: avanza prod_tail, sveglia consumer se necessario. */
static inline void
flipc2_produce_commit(flipc2_channel_t channel);

/* Pubblica N descriptor in batch. */
static inline void
flipc2_produce_commit_n(flipc2_channel_t channel, uint32_t n);

/* Ottieni il prossimo descriptor disponibile (NULL se ring vuoto). */
static inline struct flipc2_desc *
flipc2_consume_peek(flipc2_channel_t channel);

/* Rilascia il descriptor: avanza cons_head. */
static inline void
flipc2_consume_release(flipc2_channel_t channel);

/* Rilascia N descriptor in batch. */
static inline void
flipc2_consume_release_n(flipc2_channel_t channel, uint32_t n);

/* Attendi un descriptor (spin + fallback semaforo). */
static inline struct flipc2_desc *
flipc2_consume_wait(flipc2_channel_t channel, uint32_t spin_count);

/* Poll: quanti descriptor disponibili? */
static inline uint32_t
flipc2_available(flipc2_channel_t channel);
```

Tutte le funzioni fast-path sono `static inline` in un header, compilate
direttamente nel codice del server/driver.  Nessuna libreria da linkare
nel fast path.

### 5.3 Data Region helpers

```c
/* Alloca spazio nella data region inline. Restituisce offset o -1. */
static inline uint64_t
flipc2_data_alloc(flipc2_channel_t channel, uint32_t size);

/* Libera spazio nella data region inline. */
static inline void
flipc2_data_free(flipc2_channel_t channel, uint64_t offset, uint32_t size);

/* Puntatore ai dati dato un offset. */
static inline void *
flipc2_data_ptr(flipc2_channel_t channel, uint64_t offset);
```

## 6. Protocollo fast-path (dettaglio)

### 6.1 Produce (sender)

```c
struct flipc2_desc *d = flipc2_produce_reserve(ch);
if (!d) { /* ring pieno — backpressure */ }

d->opcode = MY_OP;
d->cookie = request_id;
d->data_offset = flipc2_data_alloc(ch, payload_size);
memcpy(flipc2_data_ptr(ch, d->data_offset), payload, payload_size);
d->data_length = payload_size;

WRITE_FENCE();
flipc2_produce_commit(ch);
```

`flipc2_produce_commit` incrementa `prod_tail`, poi:
```c
WRITE_FENCE();
if (hdr->cons_sleeping) {
    semaphore_signal(hdr->wakeup_sem);
    hdr->wakeups++;
}
```

### 6.2 Consume (receiver)

```c
struct flipc2_desc *d = flipc2_consume_wait(ch, SPIN_DEFAULT);

/* processa il descriptor */
void *data = flipc2_data_ptr(ch, d->data_offset);
handle_request(d->opcode, d->cookie, data, d->data_length);

flipc2_data_free(ch, d->data_offset, d->data_length);
flipc2_consume_release(ch);
```

`flipc2_consume_wait` implementa l'adaptive wakeup:
```c
for (uint32_t i = 0; i < spin_count; i++) {
    if (prod_tail != cons_head) return &ring[cons_head & mask];
    PAUSE();    /* hint al processore: spin-wait loop */
}
/* spin esaurito — blocca */
hdr->cons_sleeping = 1;
WRITE_FENCE();
/* ricontrolla per evitare lost wakeup */
if (prod_tail != cons_head) {
    hdr->cons_sleeping = 0;
    return &ring[cons_head & mask];
}
semaphore_wait(hdr->wakeup_sem);
hdr->cons_sleeping = 0;
return &ring[cons_head & mask];
```

### 6.3 Batch produce/consume

Per throughput massimo, si possono riservare/rilasciare N descriptor in
blocco, con una singola fence e un singolo check wakeup.  Utile per
command buffer GPU o batch di richieste I/O.

## 7. Kernel support necessario

Il kernel fornisce solo servizi di setup (slow path):

### 7.1 System call / MIG RPC

```
flipc2_channel_create(task, size, ring_entries) → (memory_object, semaphore)
```

Il kernel:
1. Alloca `size` byte di memoria fisica contigua (wired)
2. Crea un memory object mappabile in entrambi i task
3. Crea un semaforo Mach
4. Inizializza il channel header (magic, version, offsets)
5. Restituisce memory object + semaphore send right

```
flipc2_channel_map_pages(task, source_va, size, dest_task) → region_id
```

Il kernel:
1. Valida che `source_va..source_va+size` sia valido nel source task
2. Crea mapping read-only (o COW) nel dest task
3. Restituisce un `region_id` per i descriptor

### 7.2 Cosa il kernel NON fa

- Non tocca i descriptor
- Non fa routing di messaggi
- Non ha un Message Engine
- Non interviene nel data path dopo il setup
- Non fa polling o processing di buffer

## 8. Sicurezza

1. **Isolamento via mapping**: ogni channel è mappato solo nei due task
   partecipanti.  Un task non può accedere a channel altrui.

2. **Read-only data region per il consumer**: le pagine page-mapped sono
   read-only nel consumer — il producer mantiene il write.

3. **Validazione al setup**: il kernel verifica i parametri (size, allineamento,
   permessi task) solo durante la creazione.  Dopo il setup, i task comunicano
   direttamente — errori nel protocollo (es. offset fuori range) sono
   responsabilità dei task.

4. **Trust model**: FLIPC v2 è per task *trusted* (server di sistema).
   Non ha l'isolamento a grana fine di Mach IPC (capabilities, port rights).
   Un server compromesso può corrompere il channel.  Per client non trusted
   si usa Mach IPC tradizionale.

5. **Futuro con IOMMU (x86-64)**: su hardware con VT-d, il kernel potrà
   proteggere anche le pagine DMA — il driver non potrà DMA fuori dalla
   regione autorizzata.

## 9. Confronto con FLIPC v1

| Aspetto              | FLIPC v1                          | FLIPC v2                        |
|----------------------|-----------------------------------|---------------------------------|
| Topologia            | Multi-nodo (NORMA/KKT)            | Punto-a-punto locale            |
| Message Engine       | Nel kernel, fa il routing          | Nessuno — comunicazione diretta |
| Buffer model         | Pool a dimensione fissa, 1MB fisso | Ring descriptor 64B + data region configurabile |
| Data transfer        | Buffer ref nel ring                | Descriptor + page mapping zero-copy |
| Dimensione channel   | 1MB fisso                          | 4KB – 16MB configurabile       |
| Wakeup               | Semaforo (sempre kernel trap)      | Adaptive spin + semaforo        |
| Endpoint model       | Send/Receive separati              | Bidirezionale (producer/consumer per ruolo) |
| Kernel involvement   | Ogni messaggio (ME)                | Solo setup                      |
| API                  | ~25 funzioni + macro               | ~12 funzioni inline             |

## 10. Piano di implementazione

### Fase 1 — Strutture dati e header
- `flipc2.h`: header pubblico con struct, costanti, inline fast-path
- `flipc2_types.h`: tipi e codici di ritorno

### Fase 2 — Kernel support
- Nuove MIG RPC in `device_master.defs` o nuovo `flipc2.defs`
- Allocazione shared memory, creazione semaforo, mapping

### Fase 3 — Libreria userspace
- `libflipc2/`: channel_create, channel_accept, channel_destroy
- Setup via Mach IPC, mapping shared memory

### Fase 4 — Test e benchmark
- `flipc2_test` server: latenza singolo descriptor, throughput batch,
  confronto con Mach IPC
- Misurare: ns per descriptor, throughput MB/s con page mapping

### Fase 5 — Integrazione
- ext2_server ↔ ahci_driver via FLIPC v2 (primo canale reale)
- Benchmark I/O end-to-end con e senza FLIPC v2

### Fase 6 — Page mapping e zero-copy
- flipc2_channel_map_pages
- Integrazione con il sistema VM per COW / read-only mapping
- Benchmark con dati bulk (4KB, 64KB, 1MB)
