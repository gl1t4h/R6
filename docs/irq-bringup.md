# Interrupt bring-up notes

This title never renders because its frame loop waits on a value that only its
own interrupt callbacks produce. These are the facts established while chasing
that, so the next session does not repeat the work.

## What the two named vectors actually are

The kernel jump tables (see psx-spx "Kernel (BIOS)") name them:

| Vector | Function | Behaviour |
| --- | --- | --- |
| `A(0x72)` | `_96_remove()` | Dequeues the CD interrupt. Returns 0. Documented as broken in the retail BIOS because of the `SysDeqIntRP` bug. |
| `B(0x5B)` | `ChangeClearPAD(int)` | Pad/card auto-acknowledge flag. Returns the previous flag. |

Neither can block a caller: both are void/status functions. Implementing them
(they now return 0 and the previous flag respectively, alongside `A(0x70)`
`_bu_init`, `A(0x71)` `_96_init`, `B(0x4A..0x4C)` `Init/Start/StopCARD2` and
`B(0x56)/B(0x57)` `GetC0Table/GetB0Table`) removes them from the unhandled log
but does not change the frame rate of progress.

## The gate the frame loop actually waits on

- `0x8008B900` is `return *(u16*)0x800B2886`.
- The main loop (`0x8008D0A0` … `0x8008D210`) alternates with `0x8008D160` and
  spins while that halfword is zero. It is cleared by the disc driver
  (`sh $zero, 10374($at)` at `0x8008BBAC`) and set from its interrupt path.
- `VSync()`-style wait at `0x80084D60` spins on `*(0x800B397C)`, the title's own
  vblank counter, and prints `VSync: timeout` (string at `0x80096020`) when the
  counter does not move. That message appears in the guest TTY at boot, which
  proves the counter is only advanced by the guest's vblank callback.

Both values are guest data maintained by guest code, so no host-side shortcut
can set them without pretending to be the game.

## The real bug found: the kernel hands over with interrupts enabled

The boot state had `COP0 SR = 0`. The only guest write to `SR` in a whole run
is a read-modify-write at `0x8008BE84`:

```
8008be78: mfc0 $v0, $12
8008be7c: lui  $v1, 0x4000
8008be80: or   $v0, $v0, $v1     ; set CU2 (GTE enable)
8008be84: mtc0 $v0, $12          ; preserve everything else
```

It preserves the incoming bits, so starting at `SR=0` meant `IE=0` forever and
*no interrupt was ever deliverable*. The interpreter now starts at
`SR = 0x00000401` (global enable + IP2 mask), which is what the retail kernel
leaves for an application; the guest keeps `IE` set through that write and
`I_MASK` is programmed to `1` later, so vblank delivery becomes possible.

`deliver_interrupt` also now honours `SR` (it will not enter a handler while
the guest has interrupts masked, which is what real hardware does) and the disc
interrupt bit is only raised when the controller has it enabled, so a stale CD
flag cannot starve the vblank path.

## Where it still stops

With delivery enabled the guest does enter its own dispatcher (`0x80087660`,
queued with `SysEnqIntRP`) and runs real handler code. It does not return to
the `$ra` sentinel (`0x0000FFFC`): the first PCs executed inside the handler are
`0x80083EAC`/`0x80083EB0`/`0x80083EB4`/`0x80083EB8`, the four-instruction store
loop of the title's `memset` at `0x80083EA0`, with a 32-bit count. A runaway
handler is bounded at 400,000 steps and the interrupted stream is resumed, but
the resumed context has already been mutated, so the run then spins in that
same loop and the display stays off (`draw area 0,0 to 1023,511`, display off,
0 rasterised pixels).

The dispatcher's first action is `lhu $v0, 10($v1)` with
`$v1 = *(0x800B12C8)`, so the next concrete check is that pointer's value at the
moment the handler is entered: if it is null, the handler is being entered
through a state the title has not finished initialising.

## Diagnostics added

- `--vblank` generates the vblank IRQ; delivery is separate and follows `SR`.
- The report prints `IRQ: SR/I_STAT/I_MASK`, every guest write to `SR` with its
  PC, and the first 64 PCs executed inside the last delivered handler.
- `Unhandled kernel calls` now includes the calling PC (`ra=`).

## Update: context save/restore and I_STAT acknowledgement

Two further defects were found and fixed after the notes above.

1. Entering a handler did not save the interrupted context, so a handler that
   returned (or one that was abandoned) left its own registers in the guest.
   The loop now saves and restores all 32 GPRs, HI, LO and CAUSE.
2. The kernel acknowledges a serviced interrupt by clearing its `I_STAT` bit.
   Without that the vblank bit stayed pending and the handler was re-entered
   every 1,024 instructions (1,515 deliveries in 30,000,000).

Effect: handler overruns dropped from 3 to 0 and delivery repeats cleanly. The
entered routine now spends its budget in the word-fill loop at `0x800653D8`
(`*(u32*)a3 = 0x33333333` until `a3` has covered `s1 >> 2` words) with a 32-bit
size taken from a register the routine never sets for itself, so the queued
entry is still not being called with the state the retail kernel supplies. The
run then stops on an unsupported jump to address 8 (`jr $t0` at `0x8009A6F8`
with `$t0 = 8`), which is the guest following a table that a runaway handler
had already overwritten.

Interrupt delivery stays opt-in (`--vblank`): enabling it currently costs guest
progress (0 GPU primitives versus 2 without it), so it is not on by default.

## Renaming

The project targets any PlayStation title, so the title-specific front end was
renamed from `R6.exe` to `spider-man-r6.exe` (`src/spiderman_player.cpp`, CMake
target `spider-man-r6`). Generic tooling keeps the `r6-` prefix: `r6-run`,
`r6-tests`, `r6-diagnostic` and the `r6::` headers.

## Where the chain actually breaks (latest)

Interrupt plumbing is now correct in the parts that can be verified:

- the queued `SysEnqIntRP` **chain** is walked (each node is `{next, routine}`);
- every routine in the chain runs with the interrupted context saved/restored;
- `I_STAT` is acknowledged when the chain finishes;
- the guest's `memset`/`memcpy`/`bzero` kernel vectors are real, so its
  structures are initialised;
- a pad/SIO0 model answers `JOY_STAT`/`JOY_DATA` and raises the controller
  interrupt (I_STAT bit 7) after a modelled shift-out delay.

Result per 40,000,000 instructions with `--vblank`: ~520 deliveries, **0**
handler overruns. The guest's handler chain (`0x80087660`, the pad/vblank
dispatcher) runs every tick and returns.

Two independent blockers remain, both measured:

1. The guest spends its time in the pad transfer wait at `0x80088040`. That
   loop exits only when `I_STAT` bit 7 is set *at the check*, or when
   `TIMER2` mode has bit 9 (`0x200`) set and its elapsed count exceeds the
   baseline. `TIMER2` mode reads back as 0 there, so the timeout branch can
   never fire, and the driver re-acknowledges the pad bit faster than the
   modelled byte arrives.
2. The vblank callback registered through libetc (`0x8008C2E8`, the function
   that increments `*(0x800B397C)` and walks the eight `VSyncCallback`
   slots) is **never entered** - a watch shows 0 hits. It is reached only
   through libetc's own exception entry, and `A(13h) HookEntryInt` takes a
   *save buffer*, not a routine: pointing the interpreter at that buffer
   derails immediately (`0x800B28BC` is data). Emulating libetc's entry, or
   registering `0x8008C2E8` as the vblank callback ourselves, is the next
   step. Until `*(0x800B397C)` moves, `VSync()` times out, the frame loop
   never advances and the display stays off.

Diagnostics added for that work: `--watch-pc ADDR` (execution count at an
address), `--peek ADDR` (word at the end of the run), `--hot` (sampled
whole-run PC histogram), plus the interrupt-entry log, per-handler PC trace and
handler histogram already present.
