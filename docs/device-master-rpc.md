# Device Master RPC Interface

Kernel primitives for userspace drivers. All RPCs are sent to the
`master_device_port` (privileged — only the bootstrap server and
tasks it delegates to may hold a send right).

MIG subsystem `device_master`, base ID **2900**.

Source files:
- `osfmk/src/mach_kernel/device/device_master.defs` — MIG definitions
- `osfmk/src/mach_kernel/device/device_master.c` — kernel implementation
- `osfmk/src/mach_kernel/device/device_master.h` — kernel header

## PCI Configuration Space

### device_pci_config_read

```c
kern_return_t device_pci_config_read(
    mach_port_t  master_port,   // master_device_port
    unsigned int bus,           // PCI bus number (0-255)
    unsigned int slot,          // PCI device/slot (0-31)
    unsigned int func,          // PCI function (0-7)
    unsigned int reg,           // register offset (must be 4-byte aligned)
    unsigned int *data          // OUT: 32-bit value read
);
```

Reads a 32-bit register from PCI configuration space using the
legacy I/O port mechanism (CF8h/CFCh). Supports both PCI config
mode 1 and mode 2.

Returns `KERN_FAILURE` if PCI is not available (`NPCI=0`) or the
tag is invalid.

### device_pci_config_write

```c
kern_return_t device_pci_config_write(
    mach_port_t  master_port,
    unsigned int bus,
    unsigned int slot,
    unsigned int func,
    unsigned int reg,           // must be 4-byte aligned
    unsigned int data           // 32-bit value to write
);
```

Writes a 32-bit register to PCI configuration space.

## Interrupt Forwarding

### device_intr_register

```c
kern_return_t device_intr_register(
    mach_port_t  master_port,
    unsigned int irq,           // IRQ number (0-15 on i386 PIC)
    mach_port_t  notify_port    // send right for notifications
);
```

Registers an IRQ for forwarding to userspace. When the IRQ fires,
the kernel sends a simple `mach_msg_header_t` to `notify_port` with:

- `msgh_id = 3000 + irq` (IRQ_NOTIFY_MSGH_BASE + irq)
- `msgh_size = sizeof(mach_msg_header_t)` (24 bytes)
- No message body

The kernel replaces the existing ISR with a forwarding handler.
Only one registration per IRQ is allowed. The driver must call
`mach_msg_receive()` on a port set containing `notify_port` to
receive interrupts.

**Typical driver loop:**

```c
mach_port_t notify;
mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &notify);
mach_port_insert_right(mach_task_self(), notify, notify,
                       MACH_MSG_TYPE_MAKE_SEND);
device_intr_register(master_device_port, ahci_irq, notify);

for (;;) {
    mach_msg_header_t msg;
    mach_msg_receive(&msg, ...);
    /* handle interrupt: read AHCI IS register, process completions */
}
```

Returns `KERN_RESOURCE_SHORTAGE` if the IRQ is already registered.

### device_intr_unregister

```c
kern_return_t device_intr_unregister(
    mach_port_t  master_port,
    unsigned int irq
);
```

Restores the original interrupt handler for the given IRQ.

## DMA Buffer Allocation

### device_dma_alloc

```c
kern_return_t device_dma_alloc(
    mach_port_t  master_port,
    unsigned int size,          // requested size (rounded up to page)
    unsigned int *vaddr,        // OUT: kernel virtual address
    unsigned int *paddr         // OUT: physical address
);
```

Allocates a physically contiguous, wired memory region suitable
for DMA. Uses `kmem_alloc_contig()` which guarantees physical
contiguity. The physical address is extracted via `pmap_extract()`.

The returned `vaddr` is a kernel virtual address. To access the
buffer from userspace, the driver must map it into its own address
space using `device_map()` on the appropriate device, or the kernel
can be extended to map it directly (future work).

Typical uses for AHCI:
- Command List (1 KB, 1 KB aligned)
- Received FIS area (256 bytes per port)
- Command Tables (~256 bytes + PRDT entries each)

### device_dma_free

```c
kern_return_t device_dma_free(
    mach_port_t  master_port,
    unsigned int vaddr,         // kernel virtual address from dma_alloc
    unsigned int size           // same size passed to dma_alloc
);
```

Frees a previously allocated DMA buffer.

## Error Codes

| Code | Meaning |
|------|---------|
| KERN_SUCCESS | Operation completed |
| KERN_INVALID_ARGUMENT | Bad port, out-of-range parameter, or IRQ not registered (unregister) |
| KERN_FAILURE | PCI not available, invalid PCI tag, or pmap_extract failed |
| KERN_RESOURCE_SHORTAGE | IRQ already registered by another driver |

## Security Model

All RPCs require the `master_device_port`. In the current boot flow,
only the bootstrap server holds a send right to this port. A userspace
driver must receive it from the bootstrap server via IPC.

## Future Evolution (x86-64)

On x86-64 with IOMMU (VT-d), the DMA primitives will be extended
with IOMMU domain management, allowing full isolation of device DMA
without kernel-mediated buffer allocation. PCI config access will
migrate to ECAM (memory-mapped PCI Express configuration) for zero
I/O port overhead.
