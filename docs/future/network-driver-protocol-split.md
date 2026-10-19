# Network: who owns the card, who speaks the protocol

Status: not implemented. A direction for the network stack, #479.

## The problem

In Uros a network card belongs to a driver server in user space, and the TCP/IP
stack is a server too; no network driver ever runs in the kernel. Something has
to connect the two: give each received frame to the protocol that wants it,
without the driver knowing what IP is, and without the protocol knowing which
card or which driver it is talking to.

## The idea, from IBM's CDLI

IBM's Common Data Link Interface (CDLI), whose code sat in this tree under
`device/` until #433 deleted it, kept the two apart:

- A driver registers its device with a *type*: Ethernet, for one.
- A protocol registers as a *demuxer* on a type, for the frame types it wants:
  0x0800 for IPv4, 0x86DD for IPv6, 0x0806 for ARP.
- The driver hands every received frame to the demultiplexer, which calls the
  protocol that registered for it. The driver never learns what IP is.

The interface was small: attach and detach a device, add and remove a demuxer,
add a filter, add a status callback. A demuxer is two callbacks, one for a
received frame and one for a change of status, such as the link going down.

What it does better than BSD's `ifnet`: the registration is dynamic and per
type, not a table compiled into the kernel.

## In Uros's terms

- **A registration is a right.** "This server may receive frames of type 0x0800
  from this card" is a capability, issued and checked like the others
  (`capability_system.md`), and the protocol server holds it.
- **The two halves live in different tasks.** The table of demuxers is the
  driver's state, or a small demultiplexing server's, behind IPC, not a locked
  list in one address space.
- **The data path is a ring.** Frames travel on FLIPC v2 rings
  (`flipc_v2_design.md`), zero-copy from the card's receive ring where the
  IOMMU allows it, not in `mbuf`s.
- **Two stacks can share a card.** A new stack brought up next to the old one,
  or a raw path for a benchmark, each holds the rights for the types it takes,
  and the driver does not change.

## What is not taken

CDLI's code. It was in-kernel by construction, with global locks and linked
lists in one address space; it spoke BSD's `struct mbuf`; and it carried a
proprietary IBM notice. Only the idea is kept, here.

## Before it

The data path, decided first: the rings a driver and a protocol share. And
the userland, which comes before the network (#479).
