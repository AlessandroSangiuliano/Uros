# Capabilities and userland programs — a direction, not a description

🔴 **Nothing in this document is built.** `docs/capability_system.md` describes
what exists; this one records a design discussion about how ordinary programs —
`ls`, `grep`, `top`, a browser — should meet the capability system. It is here
so the reasoning is not re-derived, and so the open questions are the ones that
are actually open.

---

## 1. The axis is not "system program vs user program"

That was the first framing and it is wrong. The question that sorts programs
is:

**Where does this program's authority come from, and is it knowable before the
program runs?**

| | standing authority | knowable in advance | example |
|---|---|---|---|
| shell builtin | — | — | `cd` |
| transient reader | none | **no** | `cat`, `ls`, `head`, `grep` |
| system observer | yes, narrow | yes | `top`, `ps` |
| long-lived application | yes, broad | yes | a browser, an editor |

A manifest is a statement made **before the program runs**. It is therefore the
right instrument exactly where the third column says *yes*, and the wrong one
where it says *no*.

---

## 2. An argument is a name, not a capability

The loose phrase "these tools take their authority from their arguments" is
wrong and worth correcting, because the correction is the whole design.

`/etc/shadow` is thirteen bytes. It carries no right. What differs between the
two worlds is **who calls `open()`**.

### (a) Ambient authority — UNIX today

```sh
cat /etc/shadow
```

The string is a *name*. `cat` opens it, and what decides is the **process's
identity** — uid, groups. The argument *selects*; the process's standing
authority *permits*. The program has, at every instant, everything the user
has. This is what capability systems exist to remove.

### (b) The capability travels with the invocation

```sh
cat < /etc/shadow
```

The **shell** opens it — the shell being the thing that knows what the user
asked for — and `cat` receives a descriptor. It names nothing and can name
nothing: it has authority for that file and no other.

🔑 **This form already exists in UNIX.** `cat < file` is the capability shape
and `cat file` is the ambient one, and the only difference is who calls
`open()`. The choice is not a new model; it is which of two paths the shell
already has becomes the normal one.

🔑 **And the descriptor is a materialised capability** — the second family in
`capability_system.md` §7. Verified once at open, then `read`/`write` re-verify
nothing, exactly as a page becomes a PTE and a device grant becomes an IOMMU
domain. It is what keeps the check off the hot path, and it carries the same
obligation: the descriptor must die when the capability is revoked.

---

## 3. So what is a subtree manifest for?

Not "the authority of `grep`". It is **the bound on how much ambient authority
the fallback path may exercise**.

The fallback is needed: `grep -r` opens thousands of files by name and no shell
can open them in advance. There the personality resolves the name against the
*process's* directory capability, and the manifest says how large that
directory is.

| form | who opens | `grep`'s authority |
|---|---|---|
| `grep x < file` | the shell | **that file**, nothing else |
| `grep x file` | `grep` | ambient, **bounded to a subtree** by the manifest |
| UNIX today | `grep` | ambient, **the whole filesystem** |

⚠️ Row three is where we are. Row two is a real improvement and costs nothing
in the coreutils. Row one is the goal and costs a shell that knows what it is
doing.

🔑 Which means "manifest for tools, yes or no" has an answer that depends on
**how much work the shell does**. That is why `cd` not being a program is not a
curiosity: the shell is already where the user's authority lives.

⚠️ And a manifest that says `required FILE any READ` is **worse than none**,
because it grants everything while looking like a policy. Same failure as a
`.cmf` that is compiled and never shipped.

---

## 4. System observers

`top` and `ps` read every task's state. That authority exists before they are
invoked, does not depend on arguments, and is genuinely privileged.

**This is where a manifest earns its keep**: few entries, each named, and the
thing is revocable. A short list, and the shortest one in the system.

---

## 5. A long-lived application: the browser

The opposite extreme from `cat`, and the case where a manifest pays most,
because three things stack:

- authority that is **standing** (a profile directory, a network endpoint, a
  surface) and held for days across operations the user never names;
- an authority set that **is** enumerable in advance;
- the **least trustworthy program on the machine**.

⇒ It gets the most detailed manifest in the system. See
`capability_system.md` §8 for the worked flow of a disk write.

### 5.1 The graphical part

Three things, and they are not alike:

**The surface** — `RESOURCE_FRAMEBUFFER`, already a reserved type. Standing and
nameable: it belongs in the manifest.

🔴 **Input — and here a manifest alone is dangerous.** A program that can read
input events *is* a keylogger. "Read the keyboard" must not be a capability
anybody holds. The **compositor** owns input and delivers it only to the
surface that has focus; the browser holds "this surface", and events arrive on
it.

That is the same shape as everything else — *whoever owns the resource
verifies* — applied to the most delicate resource there is.

**The GPU**, if it is ever handed over, is a device capability and follows
`capability_system.md` §3: it names the **class**, not the bus address.

### 5.2 The part that makes a browser special

🔴 **A browser runs other people's code** — JavaScript, WASM, an extension
installed six months ago. The manifest describes the authority of **the
browser**. It says nothing whatever about a **tab**.

⇒ The browser must not merely *hold* capabilities; it must be a **capability
server itself**. `cap_derive` takes a token and makes a weaker child:

```
browser              DIRECTORY ~/.browser                     read|write
  ├─ tab github.com  DIRECTORY ~/.browser/storage/github.com  read|write
  └─ tab bank.example DIRECTORY ~/.browser/storage/bank.example read|write
        └─ worker     (same, read only)
```

🔑 **Closing a tab is `cap_revoke` on that tab's derived token**, and the
`by_parent` cascade takes everything beneath it — workers included. The
delegation tree the system already has, used at the level it was designed for.

⚠️ Without this, the browser's manifest protects the system *from the browser*
and does not protect **the user** from a tab.

---

## 6. What must be measured before any of this is built

The design documents quote ~50 ns for a verify and 3–6% overhead on `open()`.
Those are *design* numbers. They have not been measured, and they are not the
numbers that decide the shape.

🔴 **The number that decides the shape is the lock.** `cap_check_locked` runs
under `cap_lock`, a **global simple lock** in `kern/cap.c`. If every `open()`
verifies, that lock is on every file open in the system. On a uniprocessor it
is cost; with AMP it is a **global serialisation point** — and uniprocessor and
AMP are both first-class here.

Two measurements, in this order:

1. **Does the lock scale?** Concurrent verifies from N tasks, time per verify
   as N grows. If it degrades, the design changes *before* the personality is
   written, not after.
2. Only then: the per-call cost, against an `open()` that verifies nothing
   today.

⚠️ And §2's materialised-descriptor answer is what keeps measurement 1 from
deciding everything: verify at `open()`, and the descriptor carries the
authority for every `read` afterwards. The lock is then on opens, not on I/O.

---

## 7. Summary

| | manifest | notes |
|---|---|---|
| `cd` and other builtins | none | not a process |
| `cat`, `ls`, `grep`, `head`, `tail` | subtree, as a **bound on the fallback** | goal is the shell opening; `cat < f` already is that |
| `top`, `ps` | narrow, named entries | authority is standing and privileged |
| browser, editor | the most detailed in the system | **and must derive per tab / per document** |

Open questions this leaves:

- Does the shell become a capability broker, and if so how does it hold the
  user's authority across sessions?
- What resolves a path against a directory capability, and where does that code
  live — the personality, or a filesystem server?
- Does input focus scoping need a resource type of its own, or is it a property
  of the surface capability?
- Measurement 1 above, which gates everything.
