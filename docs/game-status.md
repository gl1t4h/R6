# Game status

## Completed

None.

## In progress

### Spider-Man (USA)

- **Platform:** PlayStation 1
- **Region:** USA
- **Status:** Interpreter execution runner
- **Progress:** 97%
- **Completed:** Local BIN/CUE verification, ISO9660 root discovery, and PS-X
  EXE header identification, plus a delay-slot-aware control-flow walk from
  the entry point and discovered direct calls, plus the first dependency-free
  R3000A instruction decoder and synthetic tests, plus a local-only generator
  that emits a small C++ translation prototype, plus native little-endian
  memory helpers for common byte/halfword/word loads and stores, plus
  delay-slot-aware branch/jump/call lowering, a platform service interface,
  and a PC-to-native-function dispatcher with a step budget. The generator
  now emits and registers functions reached through validated direct in-image
  calls and control-flow edges. Simple `lui`/`ori`/`addiu` constant propagation
  now resolves some indirect `jalr` and `jr` targets, including targets loaded
  from in-image constant tables. A local report tool now lists potential
  indirect `jr`/`jalr` sites without uploading executable data. The report now
  includes a CFG-filtered unresolved-site list. The native runtime now exposes
  controller, timer, GPU, disc-sector, audio, video, and BIOS service seams.
  The local Python extractor now reads Mode2/2352 sectors individually instead
  of one contiguous run, so extracted analysis copies are no longer shifted by
  304 bytes per sector; `tests/test_inspect_disc.py` covers this with a
  synthetic image. The interpreter now stops on the guest's own `BREAK`
  instead of falling through it, matching PsyQ startup code that ends with
  `BREAK` after `main` returns. The runner reports the recent guest PCs, warns
  when the guest stack leaves RAM, and accepts `--entry` and `--poke` for
  bring-up. The hardware map models the three root counters, DMA channel
  control with instant completion, and `I_STAT`/`I_MASK`. The disc controller
  serves sector reads and validates positions, and the guest now reaches its
  own frame loop. The interrupt controller now implements the real PS1
  acknowledgement polarity: writing a zero to `I_STAT` clears that source and
  writing one leaves it unchanged. That let libetc's dispatcher finish instead
  of retrying on an unacknowledged vblank: an 80M-instruction run delivers
  over 1,200 vblanks with two handler overruns, and the VSync counter advances.
  The disc controller now models the real register banking, interrupt codes
  (INT3 acknowledge, INT2 complete, INT1 per sector) and streaming handshake,
  and presents the 12-byte sector header (BCD address, mode, subheader) ahead
  of the 2048 user bytes. With the position check passing, the guest streams
  whole files: `sfx.vab` is printed, sector 390 (`CD.HED`) is read, and runs
  reach `CD.WAD` at LBA 397. A 150M-instruction run reads 172 sectors. Root
  counters now advance from an emulated cycle counter instead of the host
  clock, GPUSTAT reports interlace/field bits, and DMA interrupt/DMA4 completion
  plus SPU transfer-register stubs are in place. The C++ core and `r6-tests`
  compile and pass under GCC; the batch has not yet been validated through the
  documented Visual Studio build or an on-disc run. Both front ends now open
  CHD version 5 CD images directly (uncompressed, `cdzl` and `cdlz` hunks, and
  plain or Huffman-compressed maps), verified against synthetic fixtures
  produced by `chdman` 0.289 rather than a retail image. COP2 geometry is no longer
  only counted: RTPS/RTPT, NCLIP, AVSZ3/4, MVMVA and SQR compute with the
  documented saturation and FLAG behaviour, so transformed vertices, screen
  clipping and ordering-table Z now come out of the GTE. The lighting path
  (NCS, NCCS, NCDS and their triple forms NCT, NCCT, NCDT) computes IR values
  and colour-FIFO entries, including depth cueing; the colour commands that work
  from existing IR values are still counted per opcode and named in the
  `r6-run` report. See `docs/gte-scope.md`.
- **Next step:** Rerun Spider-Man on disc to confirm the emulated-cycle timers
  make `CdSync`/`CdRead` waits deterministic. After that, investigate why the
  loader parks (`0x8008C9C4`/`0x8008CA84`) after a Pause and
  capture when the first draw-environment and polygon packets appear. The GTE
  colour-from-IR commands and MDEC follow.
- **Playable native build:** No
- **Distributed game assets:** No

### Silent Hill (USA)

- **Platform:** PlayStation 1
- **Region:** USA
- **Status:** Interpreter execution runner (early boot) + native translation
  slice
- **Progress:** 10%
- **Completed:** Opens the retail CHD version 5 image directly (`cdlz`
  hunks, Huffman-compressed map, 32,765 hunks, 262,116 sectors) and discovers
  the ISO9660 volume and PS-X EXE (`SLUS_007.07`, 79,872 bytes at LBA 24,
  entry `0x80012f7c`). The CD controller now matches the driver's handshake
  closely enough to stream: interrupt types are reported as the type value
  (INT1/INT2/INT3), banks 2/3 +2/+3 stay audio volumes instead of clearing the
  interrupt mask, and the next sector arrives on a timer instead of inside the
  acknowledge, so the guest's handler returns between sectors. The kernel
  HookEntryInt path is exercised, and the CD dispatcher may legitimately run
  for a streaming read, so the handler-overrun guard is configurable
  (2,000,000 instructions by default; its test pins a small value). A
  130M-instruction run reads 1,402 sectors (LBA 102 through 1402) without
  loader retries, draws 217,138 GPU primitives (31,250 fills) and 91,379
  VRAM pixels, and stops with the loader paused at LBA 1403 and the display
  still disabled. The boot executable's startup is also translated one basic
  block at a time: `r6-native` executed 1,975 generated blocks, matching the
  interpreter's registers, HI/LO and all 2 MiB of RAM after every block, and
  stopped at the `InitHeap` BIOS entry `A(0x39)`.
- **Loaded image:** A 130M-instruction `--vblank` run with `--dump-snapshot`
  captured the resident image. The loader's call to `0x80032EE0` is the real
  entry; it calls the resource startup at `0x8002E630`, then the resident
  runtime. The persistent 4,361-block translation matched 200,000 executed
  blocks on GPRs, HI/LO, COP0, hardware registers, GTE, scratchpad, BIOS
  bookkeeping and all 2 MiB of RAM. One native bus and one oracle interpreter
  keep COP0, hardware, GTE/scratchpad and BIOS state across block boundaries. The run advances to `0x800154CC`;
  the earlier reset-per-block 3,316-block slice parked at `0x80013130`.
  The startup trace still does not execute the dispatcher at `0x80087360`.
  That dispatcher indexes the six-entry table at `0x8002AB54` from
  `[0x800B9FD0]`; index 3 (`0x8002AB60`) calls `0x80087448`, so this is a mode
  handler, not a constructor. A targeted mode-3 checkpoint matched and a
  watch read `0x8002AB60` exactly once. Native DMA2/3/4 channels now execute
  their transfers, and the per-block differential compares full hardware
  registers, GTE registers, scratchpad and BIOS bookkeeping, not just cycles.
- **Next step:** Add the remaining device seams, starting with the movie path,
  before chasing the frame loop. MDEC only becomes relevant if the captured
  native path reaches it.
- **Playable native build:** No
- **Distributed game assets:** No

### Air Hockey (USA)

- **Platform:** PlayStation 1
- **Region:** USA
- **Status:** Interpreter execution runner (early boot)
- **Progress:** 10%
- **Completed:** Opens the retail CHD version 5 image directly (8,361 hunks,
  66,888 sectors, `cdlz`/`cdzl` codecs, Huffman-compressed map). The ISO9660
  volume is discovered and the PS-X EXE identified (`SLUS_014.67`, 280,576
  bytes at LBA 28, entry `0x80010d54`). A 40M-instruction on-disc run now
  delivers 71 interrupts (70 vertical blanks and one CD interrupt) with no
  pending vectors; the guest TTY prints `ResetGraph:jtb=80051bc0` and
  `CD_init:addr=80052100` through four `B(0x35)` writes. The CD trace shows
  the same correct `GetStat` handshake as Silent Hill: INT3, flag `0x04`,
  eight response bytes, ack `07h`, interrupt enable `07h`. No guest CD
  sectors have been read and no GPU primitives submitted; the run ends in a
  TIM1/GPUSTAT wait loop at the step budget.
- **Next step:** Rerun with a larger step budget (120M-200M instructions) to
  see whether the loop is a timed init delay, and use `--dump-range` on the
  loop PCs if it does not move; then follow CD init to its first sector
  reads and capture the first draw-environment and polygon packets.
- **Playable native build:** No
- **Distributed game assets:** No

Progress percentages are intentionally conservative and will only change when
there is a reproducible milestone.

## What a playable build still requires

Texturing closes the rendering gap, but it does not make the game run. The
current pipeline generates native code for a small instruction subset and
stops at anything it does not understand, so there is no complete native
program to launch yet. A playable Spider-Man build still needs:

1. **GTE geometry** — the coordinate commands (RTPS/RTPT, NCLIP, AVSZ3/4,
   MVMVA, SQR) and the lighting commands (NCS, NCCS, NCDS and the triple forms
   NCT, NCCT, NCDT) now compute, so vertex transforms, projections, culling and
   per-vertex colour are real. The colour commands that work from existing IR
   values are still counted only. See `docs/gte-scope.md`.
2. **Complete boot path** — the retail kernel supplies the loader, exception
   handlers and thread scheduler the executable expects to find at 0xA0/0xB0/
   0xC0. The native kernel layer covers TTY/exit/events only, so a recompiled
   program still has to start itself the way `_start` expects.
3. **CD streaming and DMA** — GPU DMA linked lists (DMA2) and CD-ROM sectors
   work, and the loader now streams whole files (`sfx.vab`, `CD.HED`,
   `CD.WAD`), but the emulated-cycle timer has not yet been confirmed to
   remove the occasional `CdSync` timeouts on disc. VBlank/IRQ delivery runs
   through the guest's own dispatcher and returns.
4. **Audio and video output** — SPU/XA playback and a real window presenting the
   display area instead of an in-memory frame.
5. **Input protocol** — the pad serial protocol, not just a raw key state.

Until those exist, `r6-run` (which boots the guest's own executable),
`r6-diagnostic` and `r6-tests` are the runnable artifacts.
The renderer is a software rasterizer; there is no GPU or shader pipeline, and
adding one would not change whether the game runs.

## Running it today

`r6-player.exe` (built from `src/player_main.cpp`, also
`build\r6-player.exe`) is a
double-clickable windowed player. It finds your own `.cue`/`.bin`/`.chd`, boots
the disc's executable and runs it at roughly six million instructions a second
with a live status line. It does not render the game yet: the vblank handler
and VSync callbacks run and the loader streams files, but the main loop has
not submitted a scene. See `docs/recomp-findings.md` for the current blocker.

The window title shows instruction count, primitives and display state; the
window body shows those counters plus how many disc sectors have been read.
