# Users, authentication and capabilities

Status: not implemented. A direction for #366, and the place to keep the ideas
until that work starts. `capability_system.md` says what exists and
`capability_userland.md` how ordinary programs should meet it. This one is about
what neither settles: what a user is here.

## The problem

Uros has capabilities and no users. A login needs someone to log in as, and the
POSIX programs on their way to x86-64 (#414, #496) ask for a uid and expect a
file to have an owner and mode bits. Three things have to be worked out
together: an authentication server, how users and capabilities meet, and how far
classic Unix rights have to be mapped onto capabilities, if they have to be at
all.

## Where it stands

- There is no user store, no credential and no identity a login could check
  (#366).
- `getuid()` and the calls beside it answer 0, "no users yet"
  (`uros/src/lib/libposix-uros/handlers.c`).
- `ext_server` reads an inode's owner and mode to answer `stat`, and checks
  neither at `open()`. The one permission test near there, in
  `ext2fs_file_is_executable()`, is under `#if 0`.
- cap_server knows a requester as a port (`capability_system.md` §2), and a
  token is good for whoever holds its bytes: its `owner` is never written and no
  check reads it (#747).

## Decided: opening a file does not go through the authentication server

Asked at every `open()`, it would add a message to one of the most common calls
a program makes, and with that server slow or gone no file would open. So it
works at login, once. Who you are is settled there; what you may do is settled
by whoever owns the resource. That is the rule the capability system already
has: cap_server issues, the owner verifies.

## The idea

Everything from here on is a proposal.

1. **Login.** The authentication server checks the credential and has a token
   issued to the session that says who it is.
2. **`open()`.** The request goes to `ext_server`, which owns the file. It checks
   the token and compares the identity with the inode's owner and mode.
3. **`read` and `write`.** They check nothing. The descriptor is a materialised
   capability (`capability_system.md` §7, `capability_userland.md` §2).

The delicate step is the second: how `ext_server` knows who is asking without
believing the client. The token answers that. It verifies itself, with
`urmach_cap_verify()`: one trap, and no message to any server. After the first
time `ext_server` could remember the identity on the client's port, and later
opens would cost nothing more.

Unix rights would then be mapped at one boundary only, and only if they need
mapping at all. uid and mode bits stay something the file server reads at
`open()`, a session holds capabilities, and the rest of the system never learns
what a user is.

The Hurd, which also stands on Mach, is worth reading here: its auth server
takes part when an identity is established with a server, not at every open. Its
code is GPL, so only the idea.

## A session as a world

This is a further proposal, on top of the one above.

A task reaches the system only through the ports it holds, so those ports are its world. A login could build one for the session: a child of `bootstrap` that hands the session's tasks a name server of its own, a manifest of its own, and a host port that says what they may use. Then a user's rights would be the ports of that world and nothing else. What a session does not hold it cannot name, and logging out would mean ending the world, not revoking its rights one at a time.

This is the first level of the virtualization direction: nothing inside can tell, the protocols are the same, and a port passed on is the real port, so only what a parent chooses to intercept pays an extra hop. It meets three things that do not exist yet: a nested `bootstrap` that gives out a subset of its rights, a proxy for the host port, and a name server per world. Today `name_server` holds one mount table, and a mount is visible to every task.

The token above would still have its place: `ext_server` has to know who asks, and a session could keep a token in its world. Whether the two ideas are one design or two is open.

## Before it

- #747: a token has to be bound to whoever holds it. As long as it is not,
  stealing its bytes steals the user.
- `ext_server` needs the check at `open()`, which it has never had.
- #496: `proc_server` is where a process's credentials are likely to live. Its
  port should not assume that a uid is always 0.

## Open questions

- Does a token carry the identity in a field, or is a user a new resource type
  (`capability_system.md` §3)?
- Where does a process's identity live, how does it cross `fork()` and `exec()`,
  and what takes the place of setuid?
- How does a server acting for a user present that user: a token derived with
  `cap_derive()`, reduced to what the job needs?
- What does logging out, or disabling a user, revoke? A revocation takes a
  token's subtree (`capability_system.md` §6.3), and a descriptor must die with
  its capability (`capability_userland.md` §2).
- Is there a root? An identity that passes every check is the ambient authority
  capability systems exist to remove.
- The authentication server reads its user store from a filesystem before anyone
  has logged in. With what identity, and what does it need from the boot
  manifest?
- What is the credential: a password, or something else (#366)?
- Is a session a world, and if so, what is left for the token to do? A shared file, a server every session reaches, and a user with two sessions at once are the cases to try it on.
