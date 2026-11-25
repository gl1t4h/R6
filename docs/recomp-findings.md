# Recompilation findings

This document records reproducible technical findings that do not contain game
assets or copyrighted binaries.

## 2026-11-24 — Spider-Man (USA) disc layout

The user's local BIN/CUE was inspected without copying it into this repository.

- The BIN is a raw **Mode2/2352** track.
- An ISO9660 primary volume descriptor is present at logical sector 16.
- The root directory contains:
  - `SLUS_008.75;1`
  - `SYSTEM.CNF;1`
  - `CD.HED;1`
  - `CD.WAD;1`
  - `CINEMAS.XA;1`
  - `CINEMAS` directory
- The main executable has a `PS-X EXE` header.
- The executable's reported initial program counter is `0x8008739C`.
- Its reported load address is `0x80010000`.
- Its reported payload size is `0x000B6800` bytes.

These values are metadata for identifying the user's input and are not a
replacement for the executable or any other game data.

## What this means for the native recompilation

The first implementation milestone should be an input-analysis pipeline, not
an emulator:

1. Parse the local ISO9660 filesystem from the raw BIN.
2. Locate and validate the PS-X EXE header.
3. Make a local-only analysis copy of the executable.
4. Decode MIPS R3000A instructions and build a symbol/function database.
5. Generate native C/C++ translation units from the analyzed code.
6. Implement native Windows replacements for PlayStation platform services.
7. Keep game data local and load it only from the user's own workspace.

The repository currently implements steps 1 and 2 in
`tools/inspect-ps1-disc.py`. Steps 3–7 are not complete.

## First-pass MIPS analysis

`tools/analyze-mips.py` now performs a conservative control-flow walk from the
entry point and statically discovered in-image `JAL` targets. It records basic
blocks, branch edges, delay-slot instructions, direct calls, indirect calls,
and return sites. It deliberately emits metadata rather than generated game
code, and its output should be kept outside Git.

The walk is intentionally bounded (`32` function candidates and `5,000`
instructions per function by default) so a first pass cannot spend excessive
time treating data as code. These limits are configurable for later targeted
analysis.

This is an analysis aid, not yet a production disassembler or recompilation
backend. The next milestone is explicit MIPS R3000A instruction decoding and
validation against synthetic control-flow fixtures.

The first decoder and synthetic tests are now in `tools/mips_decode.py` and
`tests/test_mips_decode.py`. They cover common arithmetic, memory, branch,
jump, call, return, and delay-slot forms. The decoder is intentionally
dependency-free so it can become part of the future translation pipeline.

## Native translation prototype

`tools/generate-native-stub.py` now consumes a local PS-X EXE and emits a
local-only C++ translation prototype outside the repository. It currently
translates a small safe subset (`nop`, selected arithmetic/immediate
instructions, `lui`, `ori`, and returns) and fails loudly at unsupported
instructions. This proves the decoder-to-C++ pipeline without pretending that
the game is already playable.

The generated prototype now accepts `r6::Memory&` and lowers common
little-endian `lb`, `lh`, `lw`, `lbu`, `lhu`, `sb`, `sh`, and `sw` operations.
The memory object is deliberately bounds-checked so invalid address mappings
fail during development instead of silently corrupting the host process.

Control-flow lowering now models the MIPS delay slot before returning the next
program counter for direct jumps, calls, branches, `jr`, and `jalr`. The
generated prototype uses a `PlatformServices` boundary so controller, audio,
and video replacements can be implemented without embedding PlayStation
hardware code into the translator.

The runtime now has a `FunctionDispatcher` that maps translated PlayStation
addresses to native C++ function pointers. Generated translation units expose a
registration function and return the next PC after each translated block.
Execution is bounded by a step budget while the function graph is incomplete,
preventing accidental infinite loops during development.

The local generator now uses `tools/mips_cfg.py` to walk from the entry point,
follow direct calls and control-flow edges, emit a bounded native function
candidate for each discovered function, and register them with the dispatcher.
Indirect calls and jump tables are left unresolved rather than treating
arbitrary data as code.

The CFG walker now tracks a small constant register state through `lui`,
`ori`, `addiu`, `addu`, and `subu`. If that proves the target of an indirect
`jalr` or `jr` lies inside the executable image, the target is followed;
otherwise it remains unresolved. This deliberately favors incomplete analysis
over false code discovery.

The same analysis now resolves aligned in-image `lw` values. This covers a
basic jump-table/function-pointer pattern where a constant table address is
loaded and then used by `jr` or `jalr`. Indexed tables are bounded to a small
number of in-image entries; external pointers remain unresolved.

`tools/report-indirects.py` now emits a local JSON report of potential
indirect `jr` and `jalr` sites. The report marks each site as requiring
contextual resolution and caps output size; it does not claim that every
static candidate is reachable code.

The report now re-walks discovered CFG-reachable functions and separately
lists unresolved indirect sites that are actually reachable under the current
analysis limits. This distinguishes the full static candidate count from the
smaller set that the native translator must still model.

The runtime now separates platform behavior behind `PlatformServices` and
`HardwareMap`. This provides native seams for controller input, timers, GPU
registers, disc-sector reads, audio, video, and BIOS calls without embedding
emulation logic in the translator. The current null implementation is a
development stub; it is not a complete game runtime.

`WindowsPlatformServices` now provides a concrete local backend for raw BIN
user-sector reads and a monotonic timer. It opens the user's BIN in place; it
does not extract or copy it. Controller, GPU, audio, and BIOS methods remain
explicit stubs until their native contracts are defined and tested.

The Windows backend now maps arrow/keyboard controls to an active-low
PlayStation-style pad state when built on Windows. It also retains bounded
audio samples and the latest presented framebuffer in host memory. These are
development sinks, not a finished audiovisual frontend; GPU command decoding
and BIOS behavior remain separate milestones.

## 2026-11-24 — GPU command decoding and BIOS vectors

The software GPU is now a command decoder rather than a single-command sink.

`r6::SoftwareGpu` models VRAM as 1024x512 16-bit BGR555 pixels and tracks the
parameter length of every GP0 packet (`gp0_word_count`). That length table is
the important part: before it existed, an unsupported command consumed one word
and every following word was misread as a command. Poly-lines and CPU-to-VRAM
uploads have data-dependent lengths, so their packets are completed from the
vertex stream and the header size field respectively.

Implemented drawing paths are fill rectangle, flat triangle, flat variable and
fixed-size rectangles, VRAM-to-VRAM copy, CPU-to-VRAM upload and VRAM-to-CPU
download. Drawing area (`GP0 E3`/`E4`), drawing offset (`E5`) and the mask
setting (`E6`) are tracked, and display area plus display enable are tracked
through GP1. Textured and gouraud primitives are decoded, counted and reported
through `unsupported_commands()` instead of being drawn with guessed shading.

`GPUSTAT` now reports command, DMA and read readiness so guest polling loops do
not spin forever, and `GPUREAD` returns buffered download halfwords.

`r6::BiosServices` replaces the A0/B0 kernel vectors reached by jumping to
`0xA0`/`0xB0`/`0xC0` with the function number in `$t1`. TTY output (`putchar`,
`puts`, `printf` with a documented format subset), `InitHeap` and `_exit` are
implemented; the B0 event family (`OpenEvent`, `CloseEvent`, `DeliverEvent`,
`UnDeliverEvent`, `WaitEvent`, `TestEvent`, `EnableEvent`, `DisableEvent`) uses
a bounded 16-slot event table. `WaitEvent` returns whether the event was
delivered rather than blocking, because the runtime is single-threaded.
Unimplemented vectors return zero and are recorded, so the next analysis pass
can enumerate exactly which kernel functions this game still needs.

Both layers are covered by `r6-tests`, a dependency-free C++ executable
registered with CTest. It includes a regression that runs an unsupported
textured packet ahead of a supported rectangle and asserts the rectangle still
lands at the right address.

## 2026-11-24 — Textured primitives

Textured rendering is implemented in the software GPU, so a decoded frame now
carries colour instead of flat polygons.

Sampling reads from the texture page selected by `GP0(E1)`: 4-bit CLUT
(one halfword holds four palette indices), 8-bit CLUT (two indices per
halfword), and 15-bit direct colour. Palette location comes from the CLUT
attribute in bits 16-31 of the first texture-coordinate word for polygons and
of the coordinate word for rectangles, decoded as `x = (clut AND 3Fh) * 16`,
`y = (clut SHR 6) AND 1FFh`. Coordinates pass through the texture window first:
`(coord AND NOT(mask*8)) OR ((offset AND mask)*8)`.

Per-pixel colour is `(texel * vertexcolour) / 128`, so `0x808080` is neutral and
the "raw texture" command variants bypass modulation. A zero texel is
transparent and leaves the destination untouched. When the primitive is a
semi-transparent variant and the texel (or its CLUT entry) has bit 15 set, the
pixel blends with the destination using the mode in texture-page bits 5-6:
`B/2+F/2`, `B+F`, `B-F`, `B+F/4`, with clamping.

Polygons use affine interpolation of `u`/`v` from the edge-function weights,
matching the hardware's lack of perspective correction; this is also why
texture warping on large PS1 polygons will need to be reproduced rather than
"fixed". Rectangles advance `u` per column and `v` per row.

Gouraud shading (`GP0 30h-3Fh`) and lines (`40h-5Fh`) remain unsupported and
are still reported through `unsupported_commands()`.

## 2026-11-24 — Interpreter runner and first guest execution

`r6-run` boots the user's own disc image for pipeline validation. It parses the
ISO9660 root, follows the `BOOT` line in `SYSTEM.CNF`, validates the `PS-X EXE`
header, loads the payload at its reported address, and executes from the
reported entry point on the native interpreter.

The interpreter covers the integer R3000A instruction set, including the
unaligned `lwl`/`lwr`/`swl`/`swr` forms, correct branch-delay-slot ordering,
`jal`/`jalr` link addresses, HI/LO multiply and divide, COP0 register moves and
`rfe`, and the 2 MiB RAM / 1 KiB scratchpad / hardware register map.

First observed run of the USA executable:

- Entry point `0x8008739C`, loaded from LBA 24.
- 86,250 instructions executed before the guest returned through `$ra`.
- One kernel call: `A(0x39) InitHeap`.
- No GPU or DMA traffic, as expected before the kernel thread exists.

Implementation notes for this stage:

- `--entry <address>` overrides the header entry point, which is useful when
  tracing a startup path by hand.
- `BREAK` now stops the run with its own stop reason. PsyQ startup code ends
  with `BREAK` after `main` returns, so this is a normal exit, not a fault. It
  used to be a no-op, which let the guest fall through into whatever followed.
- The report lists the most recent guest PCs, so the last few instructions
  before a stop are visible without re-running under a debugger.

## 2026-11-24 — Corrected local extraction

An earlier version of `tools/inspect-ps1-disc.py --extract-exe` read the
requested byte count in one contiguous `read()`. On a Mode2/2352 track only
2048 of every 2352 bytes are file data, so that single read spliced 304 bytes
of sync pattern, header, subheader and EDC/ECC into the result. The first
sector was correct and every sector after it was shifted, which silently
corrupted any local analysis copy and every report generated from it.

`read_user_sectors()` now reads sector by sector and `read_executable()`
truncates the joined user areas to the directory-record size. Both are covered
by `tests/test_inspect_disc.py`, which builds a synthetic Mode2/2352 image with
non-zero sync, subheader and EDC regions and asserts the extracted bytes match
the file data exactly. No game data is used by the test.

The C++ readers were already sector-correct; only the Python extractor was
wrong. Findings in this document that depend on an extracted copy were
re-checked against the corrected image.

## 2026-11-24 — Corrected startup analysis

The `0x8008739C` entry is a self-contained C runtime startup, not the middle of
an unrelated function:

1. Zeroes the BSS range `0x800B5994`–`0x800C65D4`.
2. Loads `$v0` from `0x800B3E70` and builds the stack pointer as
   `($v0 - 8) OR 0x80000000`.
3. Computes the heap base `0x800C65D4` and size from `0x800B3E6C`, storing them
   at `0x800B123C` and `0x800B1240`.
4. Saves `$ra`, sets `$gp = 0x800B47F4`, `$fp = $sp`.
5. Calls `0x8008DC98`, which is the `InitHeap` syscall stub (`$t1 = 0x39`,
   `jr 0xA0`).
6. Restores `$ra`, calls `0x8002C354`, then executes `BREAK` at `0x80087440`.

`0x8002C354` is the C `main`: a 72-byte frame that saves every callee-saved
register and first calls `0x80087444`. That routine is
the C++ static-constructor runner: it walks a 12-entry function-pointer table
at `0x800B47C4` with `jalr` and then returns.

Observed stop: 86,250 instructions, one `InitHeap` call, no GPU, DMA or
interrupt traffic, ending at PC `0x0`. The recent-PC trace shows all twelve
constructors returning normally into the loop at `0x80087494`, then the
routine's epilogue restoring `$ra` as zero.

That zero is a stack-mapping problem, not a game problem. `0x800B3E70` holds
`0x800000`, so the startup computes `$sp = 0x807FFFF8`, which is outside the
2 MiB of main RAM; stores to it are not retained, so `0x80087458`'s
`sw $ra, 12($sp)` is lost and `0x8008749C` reloads zero. The header's own
stack fields are empty (`0x00000000`, and a garbage size), so nothing in the
image supplies a usable stack.

Runs now print a warning when the guest stack pointer leaves main RAM, so this
class of failure is visible instead of surfacing later as a return to zero.

## 2026-11-24 — Boot progress with a valid stack

`--poke ADDRESS=VALUE` patches one guest word before the run starts, so a
startup global can be corrected without editing the image. Patching
`0x800B3E70` to `0x801FFFF0` gives the guest a real stack and changes the run
completely: the C++ constructors return, `main` continues, and the executable
reaches its own library initialisation.

Observed with the corrected stack (20,000,000-instruction budget):

- 49 kernel calls instead of one: `A(0x13)`, `A(0x28)`, `A(0x2a)`, `A(0x39)`,
  `A(0x3f)`, `A(0x44)` (`FlushCache`, four times), `A(0x49)`, `A(0x70)`,
  `A(0x72)`, plus the `B(0x08)`/`B(0x0c)` event pair eight times each and a
  dozen more `B` vectors.
- Guest TTY output from the game's own library:
  `VSync: timeout`, `ResetGraph:jtb=800b0de0,env=800b0e28` and
  `CD_init:addr=800b3df4`.
- Traffic on the GPU status register, GP0 reads, the DMA control block at
  `0x1F8010E8`/`0x1F8010F0`, DMA2 channel control and the interrupt mask.
- 28 kernel vectors still unhandled, which is the accurate measure of how much
  of the kernel this title expects.

The run then settles in a seven-instruction loop at `0x80084C04` that loads a
word through `0x800B0FA4` twice and branches until the value changes. Nothing
in the guest writes that word, so it is waiting for a VBlank-driven update.
That is the next real milestone: deliver vertical-blank interrupts and dispatch
registered callbacks, because library timeouts are not a substitute for them.

## 2026-11-24 — Hardware map: timers, DMA and interrupt flags

`r6::HardwareMap` now models the registers a booting game polls:

- Three root counters. The value register reads the platform clock in the low
  16 bits and the offset is captured when the guest writes the value register.
  Mode and target registers read back what was written. A real counter is a
  wall-clock device, so library timeouts behave the way they do on hardware;
  this is why the counters are not derived from executed instructions.
- DMA channel control registers. A start or trigger write is accepted and the
  busy and trigger bits read back clear, because the transfer is already done.
  Previously the writes were counted and dropped, so any guest polling for
  completion spun forever.
- `I_STAT`/`I_MASK` with write-one-to-acknowledge semantics on the status
  register.

`r6-tests` covers the two behaviours that matter for bring-up: a started DMA
channel reads back idle, and a timer mode register reads back its value.

## 2026-11-24 — Why nothing renders yet

DMA2 linked lists, the path GPU drawing packets actually take, are now walked
by the interpreter: a channel-control write with the start bit, linked-list
sync mode and RAM-to-GPU direction sends every packet word from the list to
GP0. A test builds a two-word list in emulated RAM and asserts both words
arrive.

Interrupt registration is implemented too. `C(0x02)`/`C(0x03)`
(`SysEnqIntRP`/`SysDeqIntRP`) now keep the queued entry per priority,
`C(0x0A)` (`ChangeClearRCnt`) and `A(0x44)` (`FlushCache`) return instead of
being recorded as unknown, and `--vblank` delivers a vertical-blank interrupt
by entering the queued routine and resuming the interrupted stream when it
returns.

With `--vblank` the run does reach the game's own dispatcher: entry
`0x800C1528`, routine `0x80087660`. That dispatcher maintains a VBlank counter
at `0x800C1538` (capped at 150), but the run then parks inside the handler, in
the same timer-polling delay loop the main thread uses, until the handler step
budget expires. It is therefore off by default; the default run reaches
40,000,000 instructions instead of stopping at 1.1 million.

What the traces say about the missing renderer:

- The guest never writes GP0 at all: `rendered_primitives()` and
  `unsupported_commands()` both stay at zero, so there is nothing to rasterise
  rather than a broken rasteriser.
- After `CD_init` the guest polls the CD controller status
  (`0x1F801800`) once and then spends the rest of the run alternating between
  the timer delay loop and GPU status reads.
- 19 kernel vectors are still unhandled, including the sound, card and
  `A(0x72)` entries.

That diagnosis was right. The string at `0x800965FC` is `CD timeout: `, and
the loop was the game's own disc-timeout reporter printing
`%s:(%s) Sync=%s, Ready=%s`.

## 2026-11-24 — CD-ROM controller

`r6::CdromDevice` now models the byte-wide register block at
`0x1F801800`-`0x1F801803`: index selection, parameter FIFO, response FIFO, data
FIFO, the interrupt flag and its acknowledge, plus `GetStat`, `Setloc`,
`ReadN`, `Pause`/`Stop`, `GetlocL`, `Test`, `GetID`, `GetTN` and `GetTD`.
Commands acknowledge first (`INT1`) and report completion (`INT2`, or
`INT3` for a read) once the guest acknowledges, which is the order library
"sync" helpers wait for. DMA channel 3 copies sectors from the data FIFO into
main RAM.

The interpreter routes those four addresses as bytes before its 32-bit
hardware path, because the register behind each one depends on the index.

Effect on the run: the `CD timeout` path is no longer taken, and the guest now
submits GPU work. With a corrected stack and the disc controller in place,
40,000,000 instructions produce 49 kernel calls, three DMA2 linked lists, 71
packet words and two rasterised primitives. Before the controller existed the
same run produced zero GPU traffic.

What is still missing for a visible frame:

- VRAM holds no non-zero pixel and the display is still off, so the packets
  are initialisation packets rather than a scene.
- The frame loop depends on the vertical-blank callback. `--vblank` reaches the
game's dispatcher (`0x80087660`) but that dispatcher never returns under the
current direct-call model, so the run parks inside it after about a million
instructions.
- GTE maths is still not computed, so any 3D geometry would arrive degenerate.

## Why the frame loop never advances (2026-11-25)

`0x8008B900` is a two-instruction leaf that returns `*(u16*)0x800B2886`. The
program's main loop (`0x8008D0A0` … `0x8008D210`) alternates with
`0x8008D160`, calls that leaf, and loops while the halfword is zero. Nothing in
the main flow writes it: it is set by the title's own CD interrupt callback.
The guest is therefore waiting on a disc interrupt, not on the display or on
the timer, which explains why 40,000,000 instructions can execute with the
display still off.

Work done to reach that conclusion:

- The disc controller now raises `I_STAT` bit 2 when its interrupt flag is
  non-zero (`HardwareMap::write_cdrom`), which is what the kernel's own
  dispatch would do.
- Interrupt delivery was split out of `poll_vblank` into
  `MipsInterpreter::deliver_interrupt`. It enters the queued routine with
  `CAUSE.IP2` set and `$a0`-`$a3` zeroed, matching how the retail kernel
  enters a dispatcher instead of inheriting whatever the interrupted code left
  in argument registers.
- With delivery enabled the guest does execute its own callback path
  (`0x8008BE64` → `0x800916xx` → game code), but the routine leaves its
  context instead of returning through `$ra`, and one of the calls it makes
  lands in a `memset`-style loop (`0x80083EA0`) with a 32-bit size, so it
  cannot finish. A runaway handler is now bounded at 400,000 steps: the
  interrupted PC and `$ra` are restored and the stream resumes, and delivery
  stops after three overruns rather than spending the whole budget.

Unhandled kernel vectors at boot are `A(0x13)`, `B(0x19)`, `B(0x5B)`,
`A(0x72)`, `B(0x4A)`, `B(0x56)`, `B(0x57)`, `B(0x4B)` and `A(0x70)`. The
memory-card probes are harmless, but `A(0x72)` and `B(0x5B)` are called with
result pointers (`0x800B2888`, `0x800B2884`) from inside the callback the frame
loop depends on, so they are the next thing to implement: the callback is
almost certainly calling them, getting a placeholder result, and then using it
as a length.

## Double-clickable player

`src/player_main.cpp` builds `R6.exe`, a windowed (no console) front end that
locates the user's own BIN/CUE in its own folder, `Downloads`, `Desktop` or
`Documents`, falls back to an open-file dialog, boots the disc's PS-X EXE with
the same stack correction, and presents the 320x240 display area scaled 3x.
ESC quits; F12 writes `r6-frame.ppm` next to the executable. `--shot <file>
--steps N` runs the same path headless and saves one frame. While the guest has
not drawn anything the window shows live instruction, primitive, disc-sector
and stop-state counters rather than a black rectangle.
