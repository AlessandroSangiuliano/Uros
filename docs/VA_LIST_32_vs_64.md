# Differenze tra va_list su architetture 32-bit e 64-bit

## Il Bug che Abbiamo Risolto

Il crash in MIG era causato da un design che funzionava su 32-bit ma falliva su 64-bit.
Questo documento spiega perché.

---

## 1. va_list su Architetture 32-bit (i386)

### Implementazione Semplice

Su i386, `va_list` è semplicemente un **puntatore**:

```c
typedef char* va_list;
```

### Come Funziona

Gli argomenti variadici sono impilati sullo **stack** in ordine, uno dopo l'altro:

```
Stack (cresce verso il basso):
+------------------+
|   arg4 (int)     |  <- va_list punta qui dopo 3 va_arg
+------------------+
|   arg3 (char*)   |  <- va_list punta qui dopo 2 va_arg
+------------------+
|   arg2 (int)     |  <- va_list punta qui dopo 1 va_arg
+------------------+
|   arg1 (char*)   |  <- va_list iniziale
+------------------+
|   right          |  <- ultimo parametro fisso
+------------------+
|   left           |
+------------------+
|   it             |
+------------------+
|   file           |
+------------------+
|   return addr    |
+------------------+
```

### Passaggio per Valore

Quando passi `va_list` a una funzione:

```c
void SkipVFPrintf(FILE *file, char *fmt, va_list pvar) {
    (void) va_arg(pvar, char*);  // Consuma un argomento
}

void vWriteCopyType(FILE *file, ipc_type_t *it, char *left, char *right, va_list pvar) {
    SkipVFPrintf(file, left, pvar);   // pvar viene COPIATO
    SkipVFPrintf(file, right, pvar);  // pvar è ancora al punto originale!
}
```

**Su 32-bit**: `va_list` è un puntatore. Passarlo per valore copia il puntatore.
Entrambe le chiamate vedono lo stesso indirizzo di partenza, ma le modifiche
fatte da `va_arg` in `SkipVFPrintf` NON si propagano al chiamante.

**TUTTAVIA**: Su molti compilatori 32-bit dell'epoca (anni '80-'90), questo
"funzionava per caso" perché:
1. Il puntatore veniva modificato in-place in alcuni casi
2. I garbage values letti erano spesso innocui (0 o indirizzi mappati)
3. Il codice originale MIG era testato solo su VAX, m68k, e early i386

---

## 2. va_list su Architetture 64-bit (x86_64)

### Implementazione Complessa

Su x86_64, `va_list` è un **array di strutture**:

```c
typedef struct {
    unsigned int gp_offset;     // Offset nel register save area (GP regs)
    unsigned int fp_offset;     // Offset nel register save area (FP regs)
    void *overflow_arg_area;    // Puntatore agli args sullo stack
    void *reg_save_area;        // Puntatore all'area di salvataggio registri
} va_list[1];
```

### Perché Così Complesso?

L'ABI x86_64 (System V AMD64) passa i primi 6 argomenti interi in **registri**:
- `%rdi`, `%rsi`, `%rdx`, `%rcx`, `%r8`, `%r9`

E i primi 8 argomenti floating-point in:
- `%xmm0` - `%xmm7`

Solo gli argomenti successivi vanno sullo stack!

```
Chiamata: func(file, it, left, right, arg1, arg2, arg3, arg4, arg5, arg6, ...)
                ^     ^    ^     ^     ^     ^     ^     ^     ^     ^
               rdi   rsi  rdx   rcx   r8    r9   STACK STACK STACK STACK

Per varargs dopo 'right' (4° parametro):
- arg1 (5°) -> %r8
- arg2 (6°) -> %r9  
- arg3 (7°) -> STACK
- arg4 (8°) -> STACK
- ...
```

### Il Register Save Area

Quando una funzione variadica viene chiamata, il prologo salva TUTTI i registri
potenzialmente contenenti argomenti in un'area di memoria:

```
Register Save Area (176 bytes):
+------------------+ offset 0
|      %rdi        |
+------------------+ offset 8
|      %rsi        |
+------------------+ offset 16
|      %rdx        |
+------------------+ offset 24
|      %rcx        |
+------------------+ offset 32
|      %r8         |
+------------------+ offset 40
|      %r9         |  <- gp_offset inizia da qui o dopo
+------------------+ offset 48
|     %xmm0        |
+------------------+ ...
|     %xmm7        |
+------------------+ offset 176

Stack overflow area (argomenti oltre i registri):
+------------------+
|      arg7        |  <- overflow_arg_area
+------------------+
|      arg8        |
+------------------+
|       ...        |
```

### Come va_arg Funziona su x86_64

```c
// Pseudo-codice per va_arg(ap, type)
if (type è integer && ap->gp_offset < 48) {
    // Prendi dal register save area
    result = *(type*)(ap->reg_save_area + ap->gp_offset);
    ap->gp_offset += 8;
} else if (type è floating && ap->fp_offset < 176) {
    // Prendi dai registri XMM salvati
    result = *(type*)(ap->reg_save_area + ap->fp_offset);
    ap->fp_offset += 16;
} else {
    // Prendi dallo stack overflow area
    result = *(type*)(ap->overflow_arg_area);
    ap->overflow_arg_area += sizeof(type);
}
```

---

## 3. Il Problema del Passaggio per Valore

### Su x86_64, va_list è un Array

In C, quando dichiari:
```c
void func(va_list pvar);
```

E `va_list` è `struct __va_list_tag[1]`, allora:
- `pvar` è di tipo `struct __va_list_tag*` (decay da array a puntatore)
- Passare `pvar` passa il **puntatore alla struttura**
- Le modifiche a `*pvar` SONO visibili al chiamante!

### Ma il Problema è Diverso

Il problema nel nostro codice MIG era:

```c
void vWriteCopyType(..., va_list pvar) {
    SkipVFPrintf(file, left, pvar);   // Stampa e consuma args per 'left'
    SkipVFPrintf(file, right, pvar);  // Dovrebbe continuare da dove left ha finito
}
```

Su x86_64, le modifiche a `pvar` in `SkipVFPrintf` SONO visibili. Ma il problema
era che **vfprintf()** consuma la va_list internamente, e dopo vfprintf la va_list
è in uno stato indeterminato!

### Il Vero Bug

```c
static void SkipVFPrintf(FILE *file, char *fmt, va_list pvar) {
    // ... skip comment portion, consuming some args ...
    
    (void) vfprintf(file, fmt, pvar);  // <-- QUESTO È IL PROBLEMA!
    
    // Dopo vfprintf, 'pvar' è in stato indeterminato secondo lo standard C.
    // Non possiamo assumere che punti agli argomenti successivi.
}
```

**Lo standard C99 dice**:
> "The object `ap` may be passed as an argument to another function; if that
> function invokes the `va_arg` macro with parameter `ap`, the value of `ap`
> in the calling function is indeterminate and shall be passed to the
> `va_end` macro prior to any further reference to `ap`."

---

## 4. La Soluzione

### Approccio 1: va_copy (C99+)

```c
static void SkipVFPrintf(FILE *file, char *fmt, va_list pvar) {
    va_list copy;
    va_copy(copy, pvar);
    
    // ... skip comment, consuming from 'copy' ...
    
    vfprintf(file, fmt, copy);
    va_end(copy);
    
    // 'pvar' non è stato toccato, ma non abbiamo avanzato il chiamante
}
```

Questo non risolve il problema di avanzare la va_list del chiamante.

### Approccio 2: Pre-formattare le Stringhe (LA NOSTRA SOLUZIONE)

```c
// Invece di:
WriteCopyType(file, argType,
              "%s%s", "/* %s%s */ %s->%s",
              ref, argVarName, who, msgField);

// Usiamo:
char left[256], right[256];
snprintf(left, sizeof(left), "%s%s", ref, argVarName);
snprintf(right, sizeof(right), "/* %s%s */ %s->%s", 
         ref, argVarName, who, msgField);
WriteCopyTypeSimple(file, argType, left, right);
```

**Vantaggi**:
- Nessuna propagazione di varargs
- Nessun rischio di undefined behavior
- Codice più chiaro e debuggabile
- Funziona identicamente su tutte le architetture

---

## 5. Tabella Riassuntiva

| Aspetto | 32-bit (i386) | 64-bit (x86_64) |
|---------|---------------|-----------------|
| Tipo di va_list | `char*` | `struct[1]` (24 bytes) |
| Passaggio args | Tutti su stack | Primi 6 int in registri |
| Dimensione puntatore | 4 bytes | 8 bytes |
| va_arg implementazione | Semplice incremento | Logica complessa |
| Passaggio va_list | Copia del puntatore | Puntatore a struct |
| Modifiche visibili al caller | Dipende dal compilatore | Sì (array decay) |

---

## 6. Lezioni Apprese

1. **Mai assumere che va_list funzioni dopo vfprintf()** - lo standard dice che è indeterminato

2. **Il codice legacy 32-bit può nascondere bug** - funzionava "per caso" su architetture più semplici

3. **Pre-formattare è più sicuro** - evita completamente il problema della propagazione varargs

4. **Usare va_copy quando necessario** - se devi passare va_list a più funzioni che la consumano

5. **Testare su più architetture** - un bug può manifestarsi solo su 64-bit o solo con certi compilatori

---

## 7. Riferimenti

- [System V AMD64 ABI](https://refspecs.linuxbase.org/elf/x86_64-abi-0.99.pdf) - Sezione 3.5.7 "Variable Argument Lists"
- [C99 Standard](http://www.open-std.org/jtc1/sc22/wg14/www/docs/n1256.pdf) - Sezione 7.15 "<stdarg.h>"
- [GCC va_list implementation](https://gcc.gnu.org/onlinedocs/gccint/Varargs.html)

---

*Documento creato durante il porting di OSF/Mach MIG da 32-bit a 64-bit, Febbraio 2026*
