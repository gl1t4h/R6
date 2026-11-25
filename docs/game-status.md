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
  reach `CD.WAD` at LBA 397. A 150M-instruction run reads 172 sectors. The
  current working tree contains uncommitted groundwork for emulated-cycle root
  counters/vblank timing, GPUSTAT interlace/field bits, DMA interrupt
  register/DMA4 completion, and SPU transfer-register stubs; this batch has not
  yet been validated by a clean build/test pass or an on-disc run.
- **Next step:** Validate the emulated-cycle timer batch, then rerun Spider-Man
  to confirm `CdSync`/`CdRead` waits become deterministic. After that,
  investigate why the loader parks (`0x8008C9C4`/`0x8008CA84`) after a Pause and
  capture when the first draw-environment and polygon packets appear. GTE maths
  and MDEC follow.
- **Playable native build:** No
- **Distributed game assets:** No

Progress percentages are intentionally conservative and will only change when
there is a reproducible milestone.

## What a playable build still requires

Texturing closes the rendering gap, but it does not make the game run. The
current pipeline generates native code for a small instruction subset and
stops at anything it does not understand, so there is no complete native
program to launch yet. A playable Spider-Man build still needs:

1. **GTE geometry** — the interpreter covers the integer instruction set, but
   COP2/GTE commands are accepted without computing anything, so vertex
   transforms and projections produce nothing usable yet.
2. **Complete boot path** — the retail kernel supplies the loader, exception
   handlers and thread scheduler the executable expects to find at 0xA0/0xB0/
   0xC0. The native kernel layer covers TTY/exit/events only, so a recompiled
   program still has to start itself the way `_start` expects.
3. **CD streaming and DMA** — GPU DMA linked lists (DMA2) and CD-ROM sectors
   work, and the loader now streams whole files (`sfx.vab`, `CD.HED`,
   `CD.WAD`), but a host-clock-driven timer still causes occasional `CdSync`
   timeouts. VBlank/IRQ delivery runs through the guest's own dispatcher and
   returns.
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
double-clickable windowed player. It finds your own `.cue`/`.bin`, boots the
disc's executable and runs it at roughly six million instructions a second
with a live status line. It does not render the game yet: the vblank handler
and VSync callbacks run and the loader streams files, but the main loop has
not submitted a scene. See `docs/recomp-findings.md` for the current blocker.

The window title shows instruction count, primitives and display state; the
window body shows those counters plus how many disc sectors have been read.
