# Uros / OSFMK — Analisi Completa del Codebase

> Generato: 3 marzo 2026  
> Branch: `feature/32-bootstrap-printf-init` (HEAD: d11a821)  
> Repository: [AlessandroSangiuliano/Uros](https://github.com/AlessandroSangiuliano/Uros)

---

## 1. Statistiche Generali

| Metrica | Valore |
|---------|--------|
| File C (`.c`) | 1068 |
| Header (`.h`) | 1182 |
| Assembly (`.s`/`.S`) | 136 |
| MIG definitions (`.defs`) | 61 |
| LOC totali (C+H) | ~759.240 |
| CMakeLists.txt | 16 |
| Target architettura | i386 (AT386) — ELF32 |
| Standard C | gnu11 (aggiornato da gnu89) |
| Build system | CMake 3.21+ / Ninja |
| Boot | Multiboot-compatible (GRUB/QEMU) |

---

## 2. Panoramica Architetturale

Uros è un sistema operativo **multiserver** basato sul microkernel **OSF Mach** (derivato CMU Mach 3.0).
L'architettura segue il modello classico Mach: un microkernel minimale che fornisce IPC, VM e scheduling,
con i servizi OS (paging, filesystem, networking) implementati come task utente separati.

### Architettura Multiserver

A differenza di un kernel monolitico (Linux, BSD), dove tutti i servizi OS girano in kernel space,
in Mach il kernel fornisce solo **cinque primitive fondamentali**:

| Primitiva | Descrizione |
|-----------|-------------|
| **Task** | Unità di protezione — contiene uno spazio di indirizzi e un insieme di porte |
| **Thread** | Unità di esecuzione — gira dentro un task |
| **Port** | Canale di comunicazione unidirezionale (coda di messaggi protetta) |
| **Message** | Collezione tipizzata di dati trasmessa tramite porte |
| **Memory Object** | Astrazione della memoria virtuale, gestibile da server esterni |

Tutto il resto — paging, filesystem, networking, device I/O ad alto livello — è implementato
da **server userspace** che comunicano tra loro e col kernel esclusivamente via **IPC (message passing)**.

#### Schema dell'Architettura

```
┌─────────────────────────────────────────────────────────────────────┐
│                         SPAZIO UTENTE                               │
│                                                                     │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐              │
│  │  bootstrap    │  │ default_pager│  │  unix server │  ...         │
│  │  (primo task) │  │  (swap/VM)   │  │  (POSIX)     │              │
│  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘              │
│         │                 │                  │                       │
│    ┌────┴─────────────────┴──────────────────┴────┐                 │
│    │        Mach IPC (porte + messaggi)            │                 │
│    └────┬─────────────────┬──────────────────┬────┘                 │
│         │                 │                  │                       │
├─────────┼─────────────────┼──────────────────┼──────────────────────┤
│         ▼                 ▼                  ▼       KERNEL SPACE   │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │                    MACH MICROKERNEL                          │    │
│  │                                                             │    │
│  │  ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌──────────────┐  │    │
│  │  │  IPC    │  │   VM    │  │  Sched  │  │   Device     │  │    │
│  │  │ (ports, │  │ (pmap,  │  │ (tasks, │  │  framework   │  │    │
│  │  │  msgs)  │  │  fault) │  │ threads)│  │  (console,   │  │    │
│  │  └─────────┘  └─────────┘  └─────────┘  │   disk, net) │  │    │
│  │                                          └──────────────┘  │    │
│  └─────────────────────────────────────────────────────────────┘    │
│                                                                     │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │                    HARDWARE (i386)                           │    │
│  └─────────────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────────────┘
```

#### Comunicazione IPC tra Componenti

La comunicazione avviene tramite **porte Mach** (kernel-managed message queues).
Ogni servizio espone una o più porte; i client inviano messaggi con `mach_msg()`.

```
                     ┌──────────────────────┐
                     │      KERNEL          │
                     │                      │
  ┌──────────┐       │  memory_object_      │       ┌──────────────┐
  │ Processo  │──────►│  data_request()  ────│──────►│ default_pager│
  │ utente    │       │                      │       │              │
  │           │◄──────│  memory_object_  ◄───│───────│ data_supply()│
  │           │       │  data_supply()       │       │              │
  └──────────┘       └──────────────────────┘       └──────────────┘
       │                                                    │
       │              ┌──────────────────────┐              │
       │              │   device driver      │              │
       └──────────────│   (in-kernel)        │◄─────────────┘
                      │   device_read/write  │
                      └──────────────────────┘
```

**Flusso tipico di un page fault**:
1. Un thread accede a un indirizzo non mappato → trap nel kernel
2. Il kernel VM (`vm/vm_fault.c`) cerca il `memory_object` associato
3. Se l'oggetto è gestito dal **default_pager**, il kernel invia
   `memory_object_data_request` al pager via IPC
4. Il default_pager legge i dati dal backing store (device di swap)
   usando `device_read()`
5. Il default_pager risponde con `memory_object_data_supply()`
6. Il kernel mappa la pagina nello spazio del task e riprende il thread

#### Ruolo di Ciascun Server

| Server | Ruolo | Porta Registrata |
|--------|-------|-----------------|
| **bootstrap** | Primo task. Carica gli altri server dal modulo multiboot. Gestisce il "bootstrap port" per la registrazione dei servizi. | `bootstrap_port` |
| **default_pager** | External memory manager di default. Gestisce swap/paging per tutti i `memory_object` senza un pager dedicato. | `default_pager` (registrato via bootstrap) |
| **unix server** | Emulazione POSIX (processi, file, segnali). In MkLinux era il "Linux server". | Vari |
| **name_server** | Servizio di naming: mappa nomi simbolici → porte Mach. Permette ai server di trovarsi. | `netname` |

#### External Memory Management (EMM)

La caratteristica più distintiva di Mach è che la gestione della memoria virtuale è
**esternalizzata**: il kernel delega ai server userspace (detti "pager") la responsabilità
di fornire e ricevere i dati delle pagine.

```
          memory_object_init()
Kernel ─────────────────────────────► Pager
          memory_object_data_request()
Kernel ─────────────────────────────► Pager
          memory_object_data_supply()
Kernel ◄───────────────────────────── Pager
          memory_object_data_return()
Kernel ─────────────────────────────► Pager (page-out)
          memory_object_terminate()
Kernel ─────────────────────────────► Pager
```

- **`memory_object_init`**: Il kernel informa il pager che un oggetto è stato mappato
- **`memory_object_data_request`**: Il kernel chiede una pagina (page-in / page fault)
- **`memory_object_data_supply`**: Il pager fornisce la pagina richiesta
- **`memory_object_data_return`**: Il kernel restituisce una pagina modificata (page-out)
- **`memory_object_terminate`**: L'oggetto non è più mappato da nessuno

Questo modello permette di implementare pager specializzati (es. network pager,
file-backed pager, compressed pager) senza modificare il kernel.

#### Registrazione dei Server via Bootstrap

```
bootstrap (task 1)
    │
    ├─ task_get_bootstrap_port() → ottiene la porta del kernel
    │
    ├─ bootstrap_create_service("default_pager") → crea porta per il pager
    │
    ├─ fork+exec default_pager (task 2)
    │   └─ task_get_bootstrap_port() → ottiene porta del bootstrap
    │   └─ bootstrap_check_in("default_pager") → registra la sua porta
    │
    ├─ fork+exec unix_server (task 3)
    │   └─ bootstrap_look_up("default_pager") → trova la porta del pager
    │
    └─ Il kernel usa la porta del default_pager per i page fault
```

### Flusso di Boot

```
GRUB/Multiboot
    │
    ▼
pstart (i386/start.S)          ← Entry point ELF @ 0x100000
    │
    ▼
setup_main (kern/startup.c)    ← Inizializzazione kernel:
    │                              printf_init, panic_init, sched_init,
    │                              vm_mem_bootstrap, vm_mem_init,
    │                              ipc_bootstrap, ipc_init,
    │                              machine_init, clock_init,
    │                              task_init, thread_init
    │
    ▼
bootstrap_create               ← Carica il bootstrap server come primo task
    │
    ▼
bootstrap (user/sbin)          ← Legge la configurazione, carica altri server:
    │                              - default_pager
    │                              - file_systems / unix
    │                              - name_server
    │
    ▼
default_pager                  ← Gestore swap/paging esterno (da integrare)
```

### Grafo delle Dipendenze di Build

```
                    migcom (host tool) ✅
                         │
           ┌─────────────┼─────────────────┐
           │             │                  │
           ▼             ▼                  ▼
        libmach     mach_kernel        server .defs
        libsa_mach  (i386 ELF)
        libcthreads
           │             │
           ▼             │
        libsa_fs         │
           │             │
           ▼             │
       bootstrap ◄───────┘ (caricato dal kernel)
           │
           ▼
      default_pager (prossimo obiettivo)
```

---

## 3. Componenti del Sistema

### 3.1 mach_kernel — Il Microkernel

**Stato: ✅ Compila e fa boot**

Il cuore del sistema. ~20MB di sorgenti, 694 righe di CMakeLists.txt.

| Modulo | Directory | Descrizione |
|--------|-----------|-------------|
| **kern/** | `mach_kernel/kern/` | Task, thread, scheduler, timer, kalloc, zalloc, IPC bootstrap |
| **vm/** | `mach_kernel/vm/` | Virtual memory, page fault, pmap, vm_map, vm_object |
| **ipc/** | `mach_kernel/ipc/` | Message passing, porte, diritti (send/receive/send-once) |
| **device/** | `mach_kernel/device/` | Framework device, driver SCSI, console, dischi |
| **i386/AT386/** | `mach_kernel/i386/` | x86: trap, pmap, locore.S, interrupt.S, GDT/IDT, PIC |
| **ddb/** | `mach_kernel/ddb/` | Kernel debugger integrato |
| **flipc/** | `mach_kernel/flipc/` | Fast Local IPC |
| **dipc/** | `mach_kernel/dipc/` | Distributed IPC |
| **xmm/** | `mach_kernel/xmm/` | External Memory Management |
| **scsi/** | `mach_kernel/scsi/` | Sottosistema SCSI |
| **chips/busses/** | `mach_kernel/chips/` | Driver chip e bus |

**Entry point**: `i386/start.S` → `pstart` → `kern/startup.c:setup_main()`

**Ordine di inizializzazione** (da startup.c):
1. `printf_init()`, `panic_init()`
2. `sched_init()` — Scheduler
3. `vm_mem_bootstrap()`, `vm_mem_init()` — Virtual memory
4. `ipc_bootstrap()`, `ipc_init()` — IPC / message passing
5. `machine_init()` — Specifico macchina (i386)
6. `clock_init()` — Clock
7. `task_init()`, `thread_init()` — Task e thread

**Linker script** (`i386/AT386/kernel.ld`):
- `OUTPUT_FORMAT("elf32-i386")`, `ENTRY(pstart)`
- Caricamento a `0x100000` (1MB) — compatibile multiboot
- Sezione `.multiboot` nei primi 8KB
- Esporta: `_text_start/end`, `_data_start/end`, `_bss_start/end`, `_kernel_end`

**Assembly**: `locore.S` è composto da concatenazione di 4 file:
`assym.S` + `start.S` + `locore.S` + `cswitch.S`

**genassym**: genera `assym.S` con offset strutture kernel (THREAD_*, TASK_*, PCB_*) usando `genassym.c`.

**MIG nel kernel**: 14 `.defs` processati (mach, mach_port, mach_host, clock, exc, notify, device, sync, ledger, bootstrap, prof, mach_debug, mach_i386, memory_object_default).
- **TODO**: `device_reply.defs` commentato ("may still have issues").

---

### 3.2 bootstrap — Primo Task Userspace

**Stato: ✅ Compila e viene caricato dal kernel**

Il bootstrap server è il primo processo caricato dal kernel. Legge la configurazione di boot
e carica gli altri server (default_pager, file systems, unix server).

| File | Descrizione |
|------|-------------|
| `bootstrap.c` | Logica principale (1587 righe), gestione porte, caricamento server |
| `load.c` | Loader generico binari |
| `elf.c` | Loader formato ELF |
| `a_out.c` | Loader formato a.out |
| `service.c` | Servizio bootstrap ports |
| `strfcns.c` | Funzioni stringa standalone |
| `formats.c` | Selezione formato (i386: a.out + ELF) |
| `boot_dep.c` | Dipendenze architettura i386 |
| `set_regs.c` | Setup registri i386 |
| `nostdlib_stubs.c` | Stub per build `-nostdlib` |

**Build**: Standalone ELF32, `-nostdlib`, entry point `__start_mach`.

**Dipendenze**: `libsa_fs`, `libcthreads`, `libmach`, `libsa_mach` (con `--start-group`/`--end-group` per dipendenze circolari).

**MIG**: 4 server stub — `bootstrap.defs`, `exc.defs`, `notify.defs`, `service.defs`.

**nostdlib_stubs.c**: Fornisce simboli mancanti nel build standalone:
- `__stack_chk_fail` → `panic("stack smashing detected")`
- `thread_switch()` → `syscall_thread_switch()` (Mach trap)
- `cthread_sp()` → inline asm `movl %esp`
- `device_read_overwrite()` → `syscall_device_read_overwrite()` (Mach trap)

**Configurazione di default**: carica `name_server`, `default_pager`, `unix`.

**cthread_stack_size** settato a 1MB.

**10 issue risolte durante il porting** (documentate in `BUILD_NOTES.md`):
1. MIG header shadowing (mach.h generato vs mach.h sistema)
2. Kernel vs export `.defs` incompatibilità
3. `prof.defs` parse failure
4. `null` minuscolo generato da migcom (workaround: `-Dnull=0`)
5. `ms_*.c` dipendenza da sed
6. `extern __inline__` semantica C89
7. Dipendenze circolari tra librerie
8. ELF `__NO_UNDERSCORES__`

---

### 3.3 default_pager — Gestore Swap/Paging

**Stato: ❌ NON ancora nel build CMake (OSFMK_BUILD_DEFAULT_PAGER=OFF)**

Il default pager è il memory manager esterno di Mach. Gestisce il backing store (swap)
per gli oggetti di memoria che non hanno un pager dedicato. È il **prossimo componente
critico da integrare** per avere un sistema funzionante.

#### 3.3.1 File Sorgente

| File | Righe | Descrizione |
|------|-------|-------------|
| `default_pager.c` | 1037 | Main loop, `seqnos_memory_object_default_server`, demux, partizioni |
| `dp_memory_object.c` | 1316 | Implementazione `memory_object` interface: data_request, data_return, init, terminate |
| `dp_backing_store.c` | 2462 | Gestione backing store: allocazione cluster, I/O device, read/write |
| `kalloc.c` | 366 | Allocatore memoria (sostituto del kernel kalloc per userspace) |
| `wiring.c` | ~170 | Wire/unwire pagine in memoria fisica |
| `strfcns.c` | ~120 | Funzioni stringa: `strbuild()`, `strprefix()`, `ovbcopy()` |
| `default_pager_internal.h` | 624 | Header principale con tutte le strutture dati |
| `externs.h` | 41 | Dichiarazioni cross-module |
| `diag.h` | — | Macro di diagnostica (dprintf, ASSERT) |
| `queue.h` | — | Implementazione coda doppiamente linkata |
| `types.h` | — | Typedef di base |
| `wiring.h` | — | Header wiring |
| `mach/default_pager_object.defs` | — | Interfaccia MIG del pager |

#### 3.3.2 Strutture Dati Principali

**`vstruct`** (Virtual memory Structure) — rappresenta un oggetto VM:
```c
struct vstruct {
    struct vs_links   vs_links;          // lista doppiamente linkata
    mutex_t           vs_lock;           // lock per accesso concorrente
    unsigned int      vs_readers;        // contatore lettori
    memory_object_t   vs_mem_obj;        // porta memory object
    mach_port_seqno_t vs_seqno;          // numero sequenza MIG
    mach_port_t       vs_control_port;   // porta controllo kernel
    mach_port_t       vs_control_port_name;
    unsigned int      vs_next_seqno;
    boolean_t         vs_waiting_seqno;  // qualcuno aspetta?
    boolean_t         vs_waiting_read;
    boolean_t         vs_waiting_write;
    boolean_t         vs_waiting_async;
    int               vs_async_pending;  // I/O asincroni pendenti
    int               vs_errors;
    struct vs_map    *vs_map;            // mappa cluster->disco
    unsigned int      vs_size;           // dimensione in pagine
    unsigned int      vs_clshift;        // log2(cluster size)
    unsigned int      vs_dmap;           // flag: mappa diretta vs indiretta
};
```

**`backing_store`** — rappresenta un backing store (device di swap):
```c
struct backing_store {
    queue_chain_t    bs_links;
    mutex_t          bs_lock;
    int              bs_port_num;
    mach_port_t      bs_port;
    boolean_t        bs_private;
    boolean_t        bs_clsize;        // cluster size
    MACH_PORT_FACE   bs_device;        // device port
    int              bs_priority;
    unsigned int     bs_pages_total;
    unsigned int     bs_pages_free;
    unsigned int     bs_pages_in;
    unsigned int     bs_pages_in_fail;
    unsigned int     bs_pages_out;
    unsigned int     bs_pages_out_fail;
};
```

**`paging_segment`** — segmento di un backing store:
```c
struct paging_segment {
    struct backing_store *ps_bs;      // backing store padre
    int                  ps_segtype;  // PS_PARTITION o PS_FILE
    MACH_PORT_FACE       ps_device;
    vm_offset_t          ps_offset;   // offset nel device
    vm_size_t            ps_recnum;
    unsigned int         ps_pgnum;    // pagine totali
    unsigned int         ps_pgcount;  // pagine usate
    unsigned int        *ps_bmap;     // bitmap allocazione
    mutex_t              ps_lock;
    boolean_t            ps_going_away;
};
```

**`vs_map` / `clmap`** — mappa cluster->blocchi disco:
```c
struct vs_map {
    unsigned int ps_ind : 1;     // indiretta?
    unsigned int ps_idx : 3;     // indice paging_segment
    unsigned int ps_offset : 28; // offset (blocchi da 8KB = 256TB max)
};
```

#### 3.3.3 Flusso di Esecuzione

1. `main()` in `default_pager.c`:
   - Ottiene `bootstrap_port` dal kernel via `task_get_bootstrap_port()`
   - Registra come `default_pager` con il bootstrap server
   - Inizializza le strutture interne
   - Entra nel main loop: `mach_msg_server()` con demux

2. Quando il kernel ha bisogno di page-out:
   - Invia `memory_object_data_return` → `seqnos_memory_object_data_return()`
   - `dp_memory_object.c` scrive i dati sul backing store via `default_write()`
   - `dp_backing_store.c` fa I/O sul device con `device_write()`

3. Quando il kernel ha bisogno di page-in:
   - Invia `memory_object_data_request` → `seqnos_memory_object_data_request()`
   - `dp_memory_object.c` legge dal backing store via `default_read()`
   - Risponde al kernel con `memory_object_data_supply()`

4. Wiring (`wiring.c`):
   - Override di `vm_allocate` → alloca + wira automaticamente
   - Usato per strutture dati interne del pager (devono stare in RAM)

#### 3.3.4 Problemi Noti nel Codice

| Problema | File | Severità |
|----------|------|----------|
| `free()` NON implementata — stampa solo un messaggio | `kalloc.c` | 🔴 Critico |
| `ps_delete()` è uno stub → `return KERN_FAILURE` | `dp_backing_store.c` | 🟡 Medio |
| Blocchi `#if 0` con codice morto | vari | 🟢 Basso |
| `vm_allocate` override a livello globale per auto-wiring | `wiring.c` | 🟡 Medio |
| Bitfield `ps_offset : 28` limita indirizzamento | `default_pager_internal.h` | 🟢 Basso |
| Lock ordering complesso tra `vs_lock` e `bs_lock` | `dp_backing_store.c` | 🟡 Medio |

#### 3.3.5 Makefile Legacy

```makefile
PROGRAMS        = default_pager
LIBS            = -lcthreads -lmach -lsa_mach -lmach
MIG_DEFS        = default_pager_object.defs
OFILES          = default_pager.o default_pager_objectServer.o \
                  wiring.o kalloc.o strfcns.o \
                  dp_memory_object.o dp_backing_store.o
```

**Dipende da**: `libcthreads`, `libmach`, `libsa_mach` (stesse librerie del bootstrap).

**MIG**: `mach/default_pager_object.defs` → genera `default_pager_objectServer.c`.

---

### 3.4 file_systems — Filesystem Library

**Stato: ✅ Operativo (libsa_fs.a)**

| Filesystem | Directory | Descrizione |
|------------|-----------|-------------|
| UFS | `ufs/` | BSD Unix File System |
| ext2fs | `ext2fs/` | Linux ext2 |
| minixfs | `minixfs/` | Minix FS |

Produce `libsa_fs.a`, usata dal bootstrap per leggere i binari da disco.
Dispatch via `AT386/fs_switch.c`.

---

### 3.5 mach_services/lib — Librerie Userspace

**Stato: ✅ Tutte le librerie principali nel build CMake**

| Libreria | Descrizione | CMake |
|----------|-------------|-------|
| **migcom** | Compilatore MIG (Flex+Bison, host-native) | ✅ + CTest |
| **libmach** | API Mach userspace (mach_msg, vm_allocate, task_create...) | ✅ |
| **libsa_mach** | Standalone Mach (no libc) | ✅ |
| **libcthreads** | C threads library | ✅ |
| **librthreads** | Real-time threads | ✅ |
| **libpthreads** | POSIX threads | ✅ |
| **libflipc** | Fast Local IPC | ✅ |
| **libservice** | Service location | ✅ |
| **libmachid** | Mach ID client | ✅ |
| **libnetname** | Network name service client | ✅ |
| **libxmm** | External memory management | ❌ Esclusa (K&R) |

**migcom**: Tool host, genera stub C da file `.defs`. Produce codice C moderno (gnu11).
Ha una suite di test CTest. Genera `ident.c` a build time. Linka `libfl` + `libdl`.

**libmach**: 16 `.defs` processati dall'export tree. Esclude `ms_*.c` (dipendenza sed).
Preprocessa assembly `.s` per i386.

**Funzione chiave di build** (`mach_services/lib/CMakeLists.txt`):
- `add_mig_defs()` — processa un `.defs` file via `mig` script → genera `_user.c`, `_server.c`, `.h`
- `maybe_add()` — aggiunge sottodirectory se ha un `CMakeLists.txt`
- Device stubs condivisi: `device.defs` processato centralmente e risultato condiviso da tutte le librerie

---

### 3.6 Altre Componenti

| Componente | Directory | Descrizione | Stato |
|------------|-----------|-------------|-------|
| **xkern** | `src/xkern/` | Framework networking modulare (TCP, UDP, IP, ARP, Ethernet) | Non in CMake |
| **stand** | `src/stand/` | Boot standalone (AT386, HP700) | Non in CMake |
| **tgdb** | `src/tgdb/` | GDB stub per debug kernel remoto | Non in CMake |
| **makedefs** | `src/makedefs/` | Definizioni build system ODE legacy | Obsoleto |
| **osc** | `src/osc/` | Configurazione OSC | Obsoleto |

---

## 4. Sistema di Build CMake

### 4.1 Struttura

```
osfmk/
├── CMakeLists.txt              ← Top-level: opzioni, CTest, add_subdirectory(src)
├── CMakePresets.json           ← 4 preset: debug, debug-nosan, release, relwithdebinfo
└── src/
    ├── CMakeLists.txt          ← Router: symlink mach/machine→i386, routing condizionale
    ├── mach_kernel/CMakeLists.txt    ← 694 righe: genassym, MIG, compile, link
    ├── bootstrap/CMakeLists.txt      ← 153 righe: MIG server, standalone ELF
    ├── file_systems/CMakeLists.txt   ← 78 righe: libsa_fs.a
    └── mach_services/lib/
        ├── CMakeLists.txt            ← 133 righe: add_mig_defs(), hub librerie
        ├── migcom/CMakeLists.txt     ← Flex+Bison, CTest
        ├── libmach/CMakeLists.txt    ← 16 MIG .defs, assembly i386
        ├── libsa_mach/CMakeLists.txt
        ├── libcthreads/CMakeLists.txt
        ├── librthreads/CMakeLists.txt
        ├── libpthreads/CMakeLists.txt
        ├── libflipc/CMakeLists.txt
        ├── libservice/CMakeLists.txt
        ├── libmachid/CMakeLists.txt
        ├── libnetname/CMakeLists.txt
        └── libxmm/CMakeLists.txt     ← Presente ma esclusa (K&R)
```

### 4.2 Opzioni di Configurazione

| Opzione | Default | Descrizione |
|---------|---------|-------------|
| `OSFMK_TARGET_AT386` | ON | Configurazione Mach per AT386 (x86) |
| `OSFMK_BUILD_TOOLS` | ON | Costruisci tool host (mig, migcom, librerie) |
| `OSFMK_BUILD_KERNEL` | ON | Costruisci il kernel Mach |
| `OSFMK_BUILD_BOOTSTRAP` | ON | Costruisci il bootstrap task |
| `OSFMK_BUILD_DEFAULT_PAGER` | **OFF** | Costruisci il default pager (da abilitare) |
| `EXPORT_PLATFORM` | auto | Nome piattaforma per directory export |

### 4.3 Preset Disponibili

| Preset | Tipo | Flag | Uso |
|--------|------|------|-----|
| `debug` | Debug | `-g -O1 -fsanitize=address,undefined` | ASAN + UBSAN |
| `debug-nosan` | Debug | `-g -O0 -fno-inline` | GDB / Valgrind |
| `release` | Release | `-O2 -DNDEBUG` | Produzione |
| `relwithdebinfo` | RelWithDebInfo | `-O2 -g -DNDEBUG` | Profiling |

### 4.4 Flag di Compilazione (target i386)

**Kernel:**
```
-std=gnu11 -m32 -march=i686 -mno-sse -ffreestanding -nostdlib -nostdinc
-fno-builtin -fno-stack-protector -w -include stdint.h
```

**Userspace (bootstrap, default_pager, libmach, etc.):**
```
-std=gnu11 -m32 -march=i686 -msse -msse2 -fno-builtin -w
-mstack-protector-guard=global -include stdint.h
```

### 4.5 Comandi Rapidi

```bash
# Configurazione e build
cmake --preset debug
cmake --build --preset debug

# Solo kernel
cmake --build --preset debug --target mach_kernel

# Solo bootstrap
cmake --build --preset debug --target bootstrap_server

# Test (migcom)
ctest --preset debug

# Clean completo
cmake --build --preset debug --target clean-all
```

---

## 5. MIG (Mach Interface Generator)

### 5.1 Pipeline

```
file.defs → cc -E -P (preprocessore) → migcom → {file.h, fileUser.c, fileServer.c}
```

### 5.2 File .defs per Componente

**Kernel** (14 processati + 1 TODO):

| File | Subsystem | Descrizione |
|------|-----------|-------------|
| `mach.defs` | 2000 | Core Mach calls |
| `mach_port.defs` | 3100 | Port operations |
| `mach_host.defs` | 200 | Host operations |
| `clock.defs` | 1000 | Clock interface |
| `exc.defs` | 2400 | Exceptions |
| `notify.defs` | 64 | Notifications |
| `device.defs` | 2800 | Device interface |
| `device_request.defs` | — | Device requests |
| `sync.defs` | — | Synchronization |
| `ledger.defs` | — | Ledger interface |
| `bootstrap.defs` | 999999 | Bootstrap interface |
| `prof.defs` | — | Profiling |
| `mach_debug.defs` | — | Debug interface |
| `mach_i386.defs` | — | i386 specific |
| `memory_object_default.defs` | — | Memory object defaults |
| ~~`device_reply.defs`~~ | — | **TODO: commentato, ha issues** |

**Bootstrap** (4 server stub):
- `bootstrap.defs`, `exc.defs`, `notify.defs`, `service.defs`

**Default pager** (1):
- `mach/default_pager_object.defs`

**Librerie** (16 dal export tree):
- Vari `.defs` processati da `add_mig_defs()` in `mach_services/lib/CMakeLists.txt`

**Totale**: 61 file `.defs` nel codebase.

---

## 6. Progresso di Modernizzazione

### 6.1 Completato ✅

| Commit | Descrizione |
|--------|-------------|
| `8d19b43` | K&R → ANSI C prototypes across all userspace libraries |
| `b7fb9ac` | Remove dead code: `#if 0` blocks, obsolete architectures, SCCS markers |
| `11961e9` | Replace unsafe string ops with bounded versions (strlcpy/snprintf) |
| `4512848` | Add missing `const` qualifiers to pointer parameters |
| `931b576` | Replace hardcoded magic numbers with named constants |
| `e248747` | Fix unsafe pointer-to-integer casts with `uintptr_t` |
| `e42ccb5` | Fixed issue #21 (XXX threading bugs) |
| `b82abde` | Issue #22 refactoring completed (goto → structured loops) |
| `cb4da92` | Closing issue #23 (bounded string ops) |
| `ed2b3dd` | Fix RTHREAD_STACK_OFFSET undeclared |
| `e132f47` | Remove dead code: CMU history block and VARARGS lint comments |
| `63f957f` | Fix pointer-to-int cast in cthread_exit |
| `3f82905` | Remove XXX markers from boot_dep.c |
| `3ed2ed9` | **gnu89 → gnu11 upgrade** |
| `9e6e0d7` | **MIG genera codice C moderno** |
| `1857b9d` | **Bootstrap: build, load and start** |
| `4e64a84` | Map PF_R ELF segments; fix early-boot mods_addr read |
| `a957263` | **Dual console — VGA primary con serial mirror** |
| `e724067` | Guard debug traces behind dprintf (DEBUG builds only) |
| `2925155` | Test per bootstrap server e printf_init |

### 6.2 In Corso 🔄

- Branch `feature/32-bootstrap-printf-init`: test inizializzazione printf nel bootstrap
- 17 file non staged (work in progress)

### 6.3 Da Fare ❌

| Priorità | Task | Note |
|----------|------|------|
| **P0** | Integrare default_pager nel build CMake | Ha tutte le sorgenti, manca solo il CMakeLists.txt |
| **P0** | Far funzionare il default_pager come secondo server | Critico per sistema operativo funzionante |
| **P1** | Fix `device_reply.defs` nel kernel | Commentato con TODO nel kernel CMakeLists.txt |
| **P1** | Modernizzare libxmm (K&R) | Attualmente esclusa dal build |
| **P2** | Integration test con QEMU | Script `run-qemu.sh` già presente |
| **P2** | Portare xkern a CMake | Framework networking, ~2.4MB |
| **P3** | Portare stand/tgdb a CMake | Boot standalone e debug remoto |

### 6.4 GitHub Issues Tracker

| # | Priorità | Categoria | Stato |
|---|----------|----------|--------|
| [#14](https://github.com/AlessandroSangiuliano/Uros/issues/14) | P1 | K&R → ANSI C | Open (fatto per userspace) |
| [#15](https://github.com/AlessandroSangiuliano/Uros/issues/15) | P1 | Remove `register` | Open |
| [#18](https://github.com/AlessandroSangiuliano/Uros/issues/18) | P2 | Add `const` | Open |
| [#22](https://github.com/AlessandroSangiuliano/Uros/issues/22) | P3 | goto → structured loops | Open |
| [#23](https://github.com/AlessandroSangiuliano/Uros/issues/23) | P0 | Unsafe string ops | Open |
| [#24](https://github.com/AlessandroSangiuliano/Uros/issues/24) | P2 | Dead code cleanup | Open |
| [#25](https://github.com/AlessandroSangiuliano/Uros/issues/25) | P3 | Magic numbers | Open |
| [#26](https://github.com/AlessandroSangiuliano/Uros/issues/26) | P0 | pointer↔int → intptr_t | Open |
| [#27](https://github.com/AlessandroSangiuliano/Uros/issues/27) | P1 | XXX comments & threading | Open |

---

## 7. Problemi Noti e Note Tecniche

### 7.1 Header Shadowing MIG
I file generati da MIG (es. `mach.h`) possono shadows gli header di sistema. Soluzione adottata:
gli header MIG generati vanno in `generated/` e NON in `generated/include/` per evitare conflitti.

### 7.2 Export Tree vs Kernel .defs
I `.defs` nell'export tree (`export/powermac/include/`) differiscono da quelli nel kernel
(`mach_kernel/mach/`). Le librerie userspace DEVONO usare quelli dall'export tree.

### 7.3 Dipendenze Circolari tra Librerie
`libmach` ↔ `libcthreads` ↔ `libsa_mach` hanno dipendenze circolari. Risolto con
`-Wl,--start-group` / `-Wl,--end-group`.

### 7.4 Semantica inline C89 vs C11
Con `extern __inline__` in C89 (gnu89), le funzioni inline erano emesse in ogni `.o`.
Con gnu11, la semantica è cambiata. Workaround attuale: `-Wl,--allow-multiple-definition`.

### 7.5 default_pager `free()` non implementata
In `kalloc.c` del default_pager, `free()` stampa solo un messaggio e non libera memoria.
Potenziale memory leak a runtime.

### 7.6 `ps_delete()` stub
In `dp_backing_store.c`, la funzione per rimuovere un paging segment è uno stub che
ritorna sempre `KERN_FAILURE`. Non è possibile rimuovere swap a runtime.

### 7.7 `vm_allocate` override in wiring.c
Il default pager sovrascrive `vm_allocate` per auto-wirare tutta la memoria allocata.
Questo impedisce al kernel di paginarla out (il pager stesso non può essere paginato!).

---

## 8. Layout delle Directory di Output

```
build-debug/
├── export/
│   └── osfmk/
│       └── x86_64/            (o i386)
│           ├── kernel/
│           │   └── mach_kernel    ← Kernel ELF
│           └── user/
│               ├── sbin/
│               │   └── bootstrap  ← Bootstrap server ELF
│               └── lib/
│                   ├── libmach.a
│                   ├── libsa_mach.a
│                   ├── libcthreads.a
│                   ├── libsa_fs.a
│                   └── ...
├── generated/
│   ├── include/
│   │   ├── mach/machine -> mach/i386 (symlink)
│   │   ├── device/device.h (MIG generated)
│   │   └── machine/{types.h, va_list.h}
│   ├── bootstrap_mig/
│   │   ├── bootstrapServer.c
│   │   ├── excServer.c
│   │   └── ...
│   ├── *_user.c, *_server.c   ← MIG stubs per librerie
│   └── assym.S                ← Offset strutture kernel
└── src/
    ├── mach_kernel/           ← Object files kernel
    └── ...
```

---

## 9. Script e Tool

| File | Descrizione |
|------|-------------|
| `scripts/mig` | Script shell per pipeline MIG (`cc -E` → `migcom`) |
| `scripts/run-qemu.sh` | Lancio QEMU per test kernel |
| `scripts/generate_ode_headers.py` | Genera header dall'export tree ODE |
| `tools/modernize_knr.py` | Tool per conversione K&R → ANSI C |
| `tools/check_ascii.py` | Verifica caratteri non-ASCII nei sorgenti |
| `tools/find_nonascii_write.c` | Trova scritture non-ASCII |
| `tools/attach_nonascii_debug.sh` | Debug caratteri non-ASCII |

---

## 10. Prossimi Passi Suggeriti

### Priorità Immediata (P0)

1. **Creare `default_pager/CMakeLists.txt`**
   - Basato sul pattern del bootstrap: MIG server stub, standalone ELF, stesse librerie
   - MIG: `mach/default_pager_object.defs` → `default_pager_objectServer.c`
   - Sorgenti: `default_pager.c`, `dp_memory_object.c`, `dp_backing_store.c`, `kalloc.c`, `wiring.c`, `strfcns.c`
   - Link: `-lcthreads -lmach -lsa_mach`
   - Abilitare `OSFMK_BUILD_DEFAULT_PAGER=ON`

2. **Far partire il default_pager come secondo server**
   - Il bootstrap già lo carica dalla configurazione
   - Verificare che le porte MIG siano correttamente registrate
   - Testare con QEMU: kernel + bootstrap + default_pager

### Priorità Alta (P1)

3. **Fix `device_reply.defs`** nel kernel CMakeLists.txt
4. **Modernizzare libxmm** (K&R → ANSI C) per includerla nel build

### Priorità Media (P2)

5. **Integration test QEMU** — automatizzare boot test
6. **Portare xkern a CMake** — framework networking

### Priorità Bassa (P3)

7. Portare `stand/` e `tgdb/` a CMake
8. Chiudere le GitHub issues #14-#27 formalmente (molte sono già implementate)

### Bozza:
┌──────────────────────────────────────────┐
│         User-space Servers               │
│  ┌─────────┐ ┌──────────┐ ┌──────────┐  │
│  │bootstrap│ │default   │ │file      │  │
│  │ server  │ │ pager    │ │ systems  │  │
│  └────┬────┘ └─────┬────┘ └─────┬────┘  │
│       │ IPC (ports) │            │       │
├───────┴─────────────┴────────────┴───────┤
│              mach_kernel                 │
│  kern/ ipc/ vm/ device/ i386/ ddb/      │
│  (task, thread, scheduler, pmap, ...)   │
├──────────────────────────────────────────┤
│         Hardware (i386/AT386)            │
└──────────────────────────────────────────┘
