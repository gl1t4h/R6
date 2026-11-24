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

The early return is the expected consequence of running the executable without
the retail kernel's thread creation: the startup code is entered the way the
BIOS enters it, finishes its initialisation, and returns. Implementing GTE
maths, DMA and interrupts is what turns this into a running game loop rather
than a startup trace.
