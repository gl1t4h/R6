# Interrupt bring-up notes

This title never renders because its frame loop waits on a value that only its
own interrupt callbacks produce. These are the facts established while chasing
that, so the next session does not repeat the work.

## Correction: I_STAT acknowledges with zero bits, not one

The notes below were written while the interrupt controller used the wrong
polarity. On the PS1, `I_STAT` is acknowledged by writing a **zero** to a bit:
zero clears that source, one leaves it unchanged (psx-spx, "Interrupt
Acknowledge"). The model originally cleared the bits written as one, so
libetc's dispatcher (`0x8008BA00`) cleared every source except the one it was
servicing. Its retry loop at `0x8008BB18` therefore saw the vblank bit still
pending every pass and re-ran all 11 handlers until the step budget expired.
That is why 144 of 151 handlers overran and why the tail always showed the
VSync walker loop (`0x8008C318`-`0x8008C33C`) without being stuck on its own.

With the polarity corrected (`HardwareMap::write32`, and halfword writes merged
in `MipsInterpreter::write16`) the dispatcher exits normally:

- 80M-instruction run: 1,256 vblanks, 1,265 interrupts delivered, **2** handler
  overruns (was 512 in 30M).
- `intrData`/VSync count advance, the guest returns from `ReturnFromException`,
  and the 50k-step handler budget is no longer the limiting factor.
- CD interrupt traffic fell from 233k status reads to 262, because the
  dispatcher no longer re-enters the CD callback on every retry.

The remaining blocker is no longer the HLE interrupt entry model. It is the
game's own CD streaming callback: after the loader's two reads the guest parks
in its CD wait and never submits a scene (`docs/recomp-findings.md`).

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

## What made the vblank callback run

The game's VSync counter (`*(u32*)0x800B397C`, incremented inside `0x8008C2E8`,
which also walks the eight `VSyncCallback` slots at `0x800B395C`) is driven by
libetc, not by the game. Three mistakes kept it from ever executing:

1. **The kernel's exception exit was missing.** `A(13h)` is `setjmp(buf)` and
   `B(19h)` is `HookEntryInt(jmp_buf)`. After the priority chains have run, the
   kernel jumps to the pc stored in that structure, with `$v0 = 1` and `$a0`
   pointing at it (psx-spx "BIOS Interrupt/Exception Handling"). libetc puts
   its dispatcher (`0x8008B990` -> `0x8008BA00`) there, and *that* is what calls
   the registered callbacks. `B(17h) ReturnFromException` inside a handler now
   ends exception handling instead of returning to the caller.
2. **The hook ran too late.** libetc's dispatcher only routes an interrupt whose
   bit is still set in `I_STAT`, but the guest's own chain handler acknowledges
   vblank (`0x80087838`, writes `0xFFFFFFFE`) before the hook used to run. The
   hook is now entered first, then the `SysEnqIntRP` chain.
3. **Sub-word hardware writes were dropped.** The dispatcher acknowledges with
   `sh` to `I_STAT`; `write16` only handled RAM, the disc and the scratchpad, so
   the vblank bit never cleared and the dispatcher re-entered on every poll.
   Half-word stores to hardware now reach the 32-bit register model (`I_STAT`
   uses the zero-clears-bits meaning described at the top of this document).

With those three fixed, `--watch-pc 0x8008C2E8` counts thousands of hits and
`--peek 0x800B397C` climbs, so the VSync counter moves for the first time.

## The pad wait

`pad_tick()` only advanced in the main poll branch, so while a handler ran the
modelled pad transfer never completed; the driver's wait at `0x80088040` (which
polls `I_STAT` bit 7) could only be left by overrunning the handler budget. Pad
timing now advances everywhere, and a received byte raises the controller
interrupt as well as a sent one. The loop still runs (about 2k entries per 40M
instructions) but it is no longer where the run ends.

## Disc reads

The CD model was answering commands but never delivering a sector the library
could accept, and the game sat in `CdRead: retry...` forever.

The controller now follows the real interrupt protocol: a command is
acknowledged with INT3, completes with INT2, and each sector raises INT1. The
result FIFO (`+1`) and data FIFO (`+2`) are readable in every bank; only `+3`
is banked (interrupt flags in banks 1/3, interrupt mask in banks 0/2). A sector
is only presented after the guest acknowledges the previous INT1, and the next
one is presented when the FIFO drains if that acknowledgement has already
arrived, which matches libcd's ack-then-DMA order.

`GetlocL`/`GetlocP`/`Getparam`/`GetTN`/`GetTD`/`GetID`/`Test` answer with the
byte sequences libcd expects in BCD, and the disc interrupt is refreshed on
register *reads* and after DMA, not only on writes, because the flag can also
be raised by the data FIFO running dry.

Sector contents were verified against the image directly: sector 166 of
`Spider-Man (USA).bin` (Mode 2, 2352-byte raw, user data at +24) holds real
program data, and our reader returns exactly those bytes.

With that, runs advance through whole files instead of failing the first
sector.

## Why the loader rejected the read

The retry loop was not about sector contents at all. Two register-level defects
kept libcd from ever seeing a transfer:

1. **The data FIFO never looked ready.** The transfer routine at `0x8008D8B8`
   spins on bit 6 of `0x1F801800` ("data FIFO not empty") before it programs
   DMA3 and only then does it collect the sector. Our status register only set
   that bit when index 1 was selected, which the caller never does, so the loop
   spun about a million times per run. It now reports a buffered sector
   regardless of the selected index (302 status reads per run instead of 1M).
2. **The response FIFO was cleared on acknowledgement.** libcd acknowledges
   first (`0x8008C4E4`, writes 7 to `0x1F801803`) and then reads the response
   bytes out; clearing the FIFO on the ack left it with an empty response, so
   `_cd_status` (`0x800B3B20`) stayed 0 and the loader's
   `if (!(CdStatus() & 0x10)) retry` branch always fired. The FIFO now survives
   an acknowledgement and is only cleared when a new command is issued.

With those two fixed, DMA3 runs as the library intends: `BCR = 0x00010200`
(one block of 512 words) and `CHCR = 0x11000000`, so one 2048-byte sector per
transfer, and sectors now reach the guest (15 per 40M-instruction run).

The failure has moved forward: instead of `CdRead: retry...` with nothing
transferred, the loader now reports `CdRead: sector error`, then re-runs
`CD_init` and tries again. So the transfer path is accepted and the remaining
rejection is on sector content/position validation in the game's own wrapper,
not in the CD controller.

## The sector check, and why it failed

The message came from one comparison in the loader's data-ready callback:

```
0x80089A38  jal  0x80087084        ; DMA the sector body (512 words)
0x80089A40  jal  0x80087220        ; position (BCD) -> LBA
0x80089A50  lw   $v1, 0x800B1C5C   ; expected sector
0x80089A58  beq  $v0, $v1, ok      ; else "CdRead: sector error"
```

`0x80087220` is a BCD position converter that subtracts 150 (the two-second
pregap), so it turns a header of `00:02:16` into LBA 16. `0x800B1C5C` is field
`+0x20` of the loader state at `0x800B1C3C` and holds the sector the guest
expects next.

Two defects made the comparison fail:

1. **The drive did not present the 12-byte sector header.** A read sector
   starts with the address (minute/second/frame in BCD), the mode byte and,
   for Mode 2, the 8-byte subheader. libcd DMAs those 12 bytes first
   (`BCR = 0x00010003`) and validates the position before it collects the
   payload. Our FIFO began directly with the 2048 user bytes, so the "header"
   was really ISO9660 PVD text (`\x01CD001...PLAYSTATION`) and the check
   failed. `CdromDevice::load_sector` now builds header + payload, reads the
   real header from the image through `PlatformServices::read_disc_header`,
   and synthesises one from the LBA when a backend has no raw access.
2. **The short DMA was answered from the result FIFO.**
   `read_word(position_packet)` returned bytes from the command response,
   which the guest had already consumed; with the response empty it fell
   through to sector data. DMA3 on the disc channel always drains the data
   FIFO, so the special case is gone.

Earlier defects in the same path were also fixed: DMA3 stores now use the
KSEG0 alias (`0x800B382C`, not `0x000B382C`), and the controller reports the
real interrupt codes (INT3 acknowledge, INT2 complete, INT1 per sector) instead
of treating INT1 as an acknowledgement.

With that the comparison sees `v0 == v1` for every sector and the loader
streams whole files: `sfx.vab` is printed, sector 390 (`CD.HED`) is read, and
runs reach `CD.WAD` at LBA 397. A sector is presented only after the guest
acknowledges the previous data-ready interrupt, or when the FIFO drains after
that acknowledgement, so libcd's ack-then-DMA order works either way.

## The blocker left: handlers that never return

The disc path is no longer the obstacle. With reads accepted the guest reaches
its own frame loop and waits on vertical blank, and the HLE interrupt model
still struggles:

- Before the sector-header fix, `144` of `151` delivered vblank handlers hit
  the 50k-step budget. The last handler spun in guest wait loops
  (`0x800880AC`, earlier `0x8008949C`), a few hundred iterations each time.
- The handler runs an interrupt routine injected by the host rather than the
  guest's own exception vector, so nesting and blocking are only approximated.
- The game's `VSync`/`CD timeout` messages still appear when a wall-clock
  timer expires between the handler and the code that acknowledges it.
  The root counters are currently driven by the host clock, so runs are not
  deterministic; folding them onto an emulated cycle counter is the next
  correctness step.

The long-term fix is to run the guest's real exception path instead: take
vector `0x80000080`, set EPC/CAUSE/SR, let a handler return through `RFE`, and
deliver new interrupts only at safe points.

## Still open

- Frame is black: `2 primitives (1 fills)`, display off, draw area `0,0`, and a
  `--dump-frame` PPM of 320x240x3 is 230,400 bytes of zeroes.
