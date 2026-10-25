# Differences between va_list on 32-bit and 64-bit architectures

## The Bug We Fixed

The crash in MIG was caused by a design that worked on 32-bit but failed on 64-bit.
This document explains why.

---

## 1. va_list on 32-bit Architectures (i386)

### Simple Implementation

On i386, `va_list` is simply a **pointer**:

```c
typedef char* va_list;
```

### How It Works

The variadic arguments are pushed onto the **stack** in order, one after the other:

```
Stack (grows downward):
+------------------+
|   arg4 (int)     |  <- va_list points here after 3 va_arg
+------------------+
|   arg3 (char*)   |  <- va_list points here after 2 va_arg
+------------------+
|   arg2 (int)     |  <- va_list points here after 1 va_arg
+------------------+
|   arg1 (char*)   |  <- initial va_list
+------------------+
|   right          |  <- last fixed parameter
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

### Passing by Value

When you pass `va_list` to a function:

```c
void SkipVFPrintf(FILE *file, char *fmt, va_list pvar) {
    (void) va_arg(pvar, char*);  // Consumes one argument
}

void vWriteCopyType(FILE *file, ipc_type_t *it, char *left, char *right, va_list pvar) {
    SkipVFPrintf(file, left, pvar);   // pvar is COPIED
    SkipVFPrintf(file, right, pvar);  // pvar is still at the original point!
}
```

**On 32-bit**: `va_list` is a pointer. Passing it by value copies the pointer.
Both calls see the same starting address, but the changes
made by `va_arg` in `SkipVFPrintf` are NOT propagated to the caller.

**HOWEVER**: On many 32-bit compilers of the time ('80s-'90s), this
"worked by accident" because:
1. The pointer was modified in-place in some cases
2. The garbage values read were often harmless (0 or mapped addresses)
3. The original MIG code was tested only on VAX, m68k, and early i386

---

## 2. va_list on 64-bit Architectures (x86_64)

### Complex Implementation

On x86_64, `va_list` is an **array of structures**:

```c
typedef struct {
    unsigned int gp_offset;     // Offset in the register save area (GP regs)
    unsigned int fp_offset;     // Offset in the register save area (FP regs)
    void *overflow_arg_area;    // Pointer to the args on the stack
    void *reg_save_area;        // Pointer to the register save area
} va_list[1];
```

### Why So Complex?

The x86_64 ABI (System V AMD64) passes the first 6 integer arguments in **registers**:
- `%rdi`, `%rsi`, `%rdx`, `%rcx`, `%r8`, `%r9`

And the first 8 floating-point arguments in:
- `%xmm0` - `%xmm7`

Only the following arguments go on the stack!

```
Call: func(file, it, left, right, arg1, arg2, arg3, arg4, arg5, arg6, ...)
                ^     ^    ^     ^     ^     ^     ^     ^     ^     ^
               rdi   rsi  rdx   rcx   r8    r9   STACK STACK STACK STACK

For varargs after 'right' (4th parameter):
- arg1 (5th) -> %r8
- arg2 (6th) -> %r9  
- arg3 (7th) -> STACK
- arg4 (8th) -> STACK
- ...
```

### The Register Save Area

When a variadic function is called, the prologue saves ALL the registers
that may contain arguments into an area of memory:

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
|      %r9         |  <- gp_offset starts here or later
+------------------+ offset 48
|     %xmm0        |
+------------------+ ...
|     %xmm7        |
+------------------+ offset 176

Stack overflow area (arguments beyond the registers):
+------------------+
|      arg7        |  <- overflow_arg_area
+------------------+
|      arg8        |
+------------------+
|       ...        |
```

### How va_arg Works on x86_64

```c
// Pseudo-code for va_arg(ap, type)
if (type is integer && ap->gp_offset < 48) {
    // Take from the register save area
    result = *(type*)(ap->reg_save_area + ap->gp_offset);
    ap->gp_offset += 8;
} else if (type is floating && ap->fp_offset < 176) {
    // Take from the saved XMM registers
    result = *(type*)(ap->reg_save_area + ap->fp_offset);
    ap->fp_offset += 16;
} else {
    // Take from the stack overflow area
    result = *(type*)(ap->overflow_arg_area);
    ap->overflow_arg_area += sizeof(type);
}
```

---

## 3. The Pass-by-Value Problem

### On x86_64, va_list Is an Array

In C, when you declare:
```c
void func(va_list pvar);
```

And `va_list` is `struct __va_list_tag[1]`, then:
- `pvar` is of type `struct __va_list_tag*` (decay from array to pointer)
- Passing `pvar` passes the **pointer to the structure**
- The changes to `*pvar` ARE visible to the caller!

### But the Problem Is Different

The problem in our MIG code was:

```c
void vWriteCopyType(..., va_list pvar) {
    SkipVFPrintf(file, left, pvar);   // Prints and consumes args for 'left'
    SkipVFPrintf(file, right, pvar);  // Should continue from where left finished
}
```

On x86_64, the changes to `pvar` in `SkipVFPrintf` ARE visible. But the problem
was that **vfprintf()** consumes the va_list internally, and after vfprintf the va_list
is in an indeterminate state!

### The Real Bug

```c
static void SkipVFPrintf(FILE *file, char *fmt, va_list pvar) {
    // ... skip comment portion, consuming some args ...
    
    (void) vfprintf(file, fmt, pvar);  // <-- THIS IS THE PROBLEM!
    
    // After vfprintf, 'pvar' is in an indeterminate state according to the C standard.
    // We cannot assume that it points to the following arguments.
}
```

**The C99 standard says**:
> "The object `ap` may be passed as an argument to another function; if that
> function invokes the `va_arg` macro with parameter `ap`, the value of `ap`
> in the calling function is indeterminate and shall be passed to the
> `va_end` macro prior to any further reference to `ap`."

---

## 4. The Solution

### Approach 1: va_copy (C99+)

```c
static void SkipVFPrintf(FILE *file, char *fmt, va_list pvar) {
    va_list copy;
    va_copy(copy, pvar);
    
    // ... skip comment, consuming from 'copy' ...
    
    vfprintf(file, fmt, copy);
    va_end(copy);
    
    // 'pvar' has not been touched, but we have not advanced the caller
}
```

This does not solve the problem of advancing the caller's va_list.

### Approach 2: Pre-format the Strings (OUR SOLUTION)

```c
// Instead of:
WriteCopyType(file, argType,
              "%s%s", "/* %s%s */ %s->%s",
              ref, argVarName, who, msgField);

// We use:
char left[256], right[256];
snprintf(left, sizeof(left), "%s%s", ref, argVarName);
snprintf(right, sizeof(right), "/* %s%s */ %s->%s", 
         ref, argVarName, who, msgField);
WriteCopyTypeSimple(file, argType, left, right);
```

**Advantages**:
- No propagation of varargs
- No risk of undefined behavior
- Clearer, more debuggable code
- Works identically on all architectures

---

## 5. Summary Table

| Aspect | 32-bit (i386) | 64-bit (x86_64) |
|---------|---------------|-----------------|
| va_list type | `char*` | `struct[1]` (24 bytes) |
| Args passing | All on the stack | First 6 ints in registers |
| Pointer size | 4 bytes | 8 bytes |
| va_arg implementation | Simple increment | Complex logic |
| va_list passing | Copy of the pointer | Pointer to struct |
| Changes visible to the caller | Depends on the compiler | Yes (array decay) |

---

## 6. Lessons Learned

1. **Never assume that va_list works after vfprintf()** - the standard says it is indeterminate

2. **Legacy 32-bit code can hide bugs** - it worked "by accident" on simpler architectures

3. **Pre-formatting is safer** - it completely avoids the varargs propagation problem

4. **Use va_copy when necessary** - if you have to pass va_list to several functions that consume it

5. **Test on multiple architectures** - a bug may show up only on 64-bit or only with certain compilers

---

## 7. References

- [System V AMD64 ABI](https://refspecs.linuxbase.org/elf/x86_64-abi-0.99.pdf) - Section 3.5.7 "Variable Argument Lists"
- [C99 Standard](http://www.open-std.org/jtc1/sc22/wg14/www/docs/n1256.pdf) - Section 7.15 "<stdarg.h>"
- [GCC va_list implementation](https://gcc.gnu.org/onlinedocs/gccint/Varargs.html)

---

*Document created during the porting of OSF/Mach MIG from 32-bit to 64-bit, February 2026*
