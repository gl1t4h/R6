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
- **Tested with:** a local `.cue` beside a single-track MODE2/2352 `.bin`
  (616,494,480 bytes, one `INDEX 01 00:00:00`). The CHD reader is covered by
  synthetic fixtures and by the open check below, but every measurement in
  this section comes from the `.bin`/`.cue` pair.
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
  VRAM pixels, and stops with the loader paused at LBA 1403. That run's
  "display off" was the polarity bug described below, not the guest. The
  boot executable's startup is also translated one basic block at a time:
  `r6-native` executed 1,975 generated blocks, matching the
  interpreter's registers, HI/LO and all 2 MiB of RAM after every block, and
  stopped at the `InitHeap` BIOS entry `A(0x39)`.
- **Display path:** GP1(03h) bit 0 is DMSK, where 0 enables the display and
  1 disables it. The software GPU had this inverted, so Silent Hill's
  per-frame `0x03000000` write (743 calls from `0x80095014`, plus the boot
  call at `0x80010878`) was recorded as "display off". The model now honors
  the documented polarity, and GPUSTAT bit 23 is set only while the display
  is disabled. A 250M-instruction run reports `display on at 0,32`, but the
  displayed 320x240 area is still empty: the primitive count is frozen at
  217,138 from about 200M on, and the guest sits in main state
  `0x800BCCBC=2` / substate `0x800BCCC0=1` at `0x800C9A84`, polling
  `0x800C9874`, which reports the sound slot machine idle. The CD is
  idle after a Pause at LBA
  17846; its last reads are small chunks of `SILENT.` (3 to 45 sectors),
  and each DMA3 payload lands in the game's own buffers (`0x801E3600`,
  `0x801E4600`), so sector delivery itself works. The
  earlier claim that the fade rate (`0x800B8FEC`) was never written came
  from reading the wrong address: the store is sign-extended to
  `0x800A8FEC`, which holds `0x51`, and the fade machine at `0x800BCD0C`
  completes its `3->4->5` and `6->7->0` cycles.
- **Instruction timing:** The interpreter advanced the machine clock one
  cycle per instruction, so a 564,480-cycle frame held 565k instructions
  instead of the roughly 300k that compiled code fits. libetc's VSync
  wait counts 32,768 iterations of a fourteen-instruction loop at
  `0x800132AC`, which expired inside a frame on every call: the guest
  printed `VSync: timeout` 439 times in a 250M-instruction run, once per
  frame, and returned before the vblank arrived. The clock now charges
  `kCyclesPerInstruction` (2) per instruction in both the interpreter and
  the generated native blocks, so the per-block differential still
  compares equal cycles. After the change a 250M run prints no timeout
  and reaches 884 vblanks, and a 1.2B run reaches 4,245 vblanks (about 70
  seconds of game time) with zero timeouts. The frame is still black: the
  primitive count stays at 217,138 and the guest still waits in main
  state 2 / substate 1.
- **Movie path:** Silent Hill never reaches MDEC, so a decoder is not the
  missing piece yet. A 250M-instruction run writes neither
  `0x1F801820`/`0x1F801824` nor the MDEC DMA channels (`0x1F801080`,
  `0x1F801090`), its MDEC status reads back zero, and a RAM scan finds no
  code that loads those registers directly or through `lui`/`ori` pairs.
  The scattered CD reads are resource chunks, not a movie feeder.
- **Loaded image:** A 130M-instruction `--vblank` run with `--dump-snapshot`
  captured the resident image. The loader's call to `0x80032EE0` is the real
  entry; it calls the resource startup at `0x8002E630`, then the resident
  runtime. The persistent 4,384-block event-aware translation matched 200,000
  executed blocks on GPRs, HI/LO, COP0, hardware registers, GTE, scratchpad,
  BIOS bookkeeping, an ordered 326,004-event VBlank/pad/DMA/interrupt trace
  and all 2 MiB of RAM. The native bus takes the same COP0 exception path, including
  HookEntryInt and ReturnFromException. One native bus and one oracle interpreter
  keep COP0, hardware, GTE/scratchpad and BIOS state across block boundaries. The run advances to `0x800154CC`;
  the earlier reset-per-block 3,316-block slice parked at `0x80013130`.
  The startup trace still does not execute the dispatcher at `0x80087360`.
  That dispatcher indexes the six-entry table at `0x8002AB54` from
  `[0x800B9FD0]`; index 3 (`0x8002AB60`) calls `0x80087448`, so this is a mode
  handler, not a constructor. A targeted mode-3 checkpoint matched and a
  watch read `0x8002AB60` exactly once. Native DMA2/3/4 channels now execute
  their transfers, and the per-block differential compares full hardware
  registers, GTE registers, scratchpad and BIOS bookkeeping, not just cycles.
- **Next step:** The state-2/substate-1 wait is still the gate, and the
  timing fix shows it is not frame pacing: it lasts for as long as the
  game runs. `0x800C9874` returns 1 because `0x80033548` reports every
  slot at `0x800B5508` idle, so `0x800C9A84` never leaves substate 1. The
  request ring at `0x80022C98` is drained (write index `0x80023098` = 14,
  read index `0x800230A8` = 15, so `0x80010E68` reports nothing pending),
  the two requests substate 0 posted through `0x80010F68` (IDs 460 and
  1070 into `0x801E3600`/`0x801E4600`) went through the consumer at
  `0x80011260` and retired, and the sound system's own state is idle
  (`0x800B5480` = 1 enabled, `0x800B5600` = 1). No CD read was requested
  after the Pause at LBA 17846.
  The type-2 request machine at `0x800114C4` never runs at all: its state
  word `0x800230B0` stays 0, and all 472 writes to it in a 250M run come
  from the queue-empty path at `0x8001130C`, while `0x800230A0` (15
  completions) has passed `0x80023098` (14 pushes). The two stream
  buffers the tick consumes, `0x801E09E0` and `0x801E1430`, are both left
  in state 3, the active-stream byte `0x800B97E0` is 0 and its countdown
  `0x800B97DC` is 0, so the sequencer is armed but has nothing queued.
  Trace the callback each ring entry carries (its first word is a code
  pointer such as `0x8001B128`) and find why the job it starts never
  reaches a slot.
  A byte-range store watch (`--watch-range`) narrows this further. The
  sequencer's two buffers receive no payload at all: their headers are
  written by the tick's own setup loop (`0x800336CC` to `0x80033824`:
  state 3, sub-state 4, two zero bytes), and the only store inside a body
  is a 32-bit zero at `+0xC` from `0x800336F4`. The buffers hold 3 and 4
  nonzero bytes out of 2,640. The active-stream byte `0x800B97E0` and its
  countdown `0x800B97DC` are written once, by the clear loop at
  `0x80010B0C`, and never again. The two payload buffers the game did
  fill are healthy (`0x801E3600` and `0x801E4600` each hold 4,072 of
  4,096 bytes, with the same content), so the load worked and nothing
  hands the data on. The earlier claim that the type-2 machine never runs
  is weaker than it read: every write to `0x800230B0` comes from the
  queue-empty path, which also clears the state, so a final zero is not
  proof it never ran. The SPU is not idle either: a 250M run stores 1,490
  times into `0x1F801C00`-`0x1F801E80` and 48 times into the DMA4
  registers, leaving SPUCNT at `0xC080` and the transfer address at
  `0x7F42` (`0x3FA10` in SPU RAM), so sample uploads do happen.
  The feeder has to arrive as data, not as an immediate: no code contains
  `0x801E09E0` or `0x801E1430`. The record handed to `0x80011630` turns out
  to be an SPU-RAM allocation request, not a hand-off: it converts the
  record's first word into an SPU address and passes it to the allocator
  `0x80011D04`, which stores the block at record offset `+0x1C`.
  The countdown that would make the tick report work is never armed from
  outside. `0x800B97DC` has six store sites in the whole image, all inside
  the tick `0x80033548`: `0x800335D0` and `0x800335DC` arm 24 or 22,
  `0x80033D70` and `0x80033D78` arm 1 or 24, and `0x80033DA0` decrements.
  Every arming site sits inside the per-slot loop gated by the voice mask
  `0x800BCD34`, and that mask is only written at `0x80033624` by the mask
  builder `0x8002E898`. A range watch over `0x800B97D0`-`0x800B9810`
  records 16 stores, all from the BSS clear at `0x80010B0C`: the voice
  table pointer `0x800B97E4`, the stream index `0x800B97D6`, the active
  byte `0x800B97E0` and the countdown all stay zero, so the tick leaves
  through `0x80034070` with `sp+60 == 0`, returns 0, and `0x800C9AB0`
  spins. The two calls that could start a voice, `0x8002E94C` at
  `0x800C9B8C` and `0x8002E990` at `0x800C9BA4`, are in substate 3 of that
  same state machine. The buffers it waits on are already loaded: both
  payload buffers are written by the CD driver copy loop at `0x80015C88`
  and hold 4,072 bytes of their 4,096. They hold identical bytes, which is
  the next thing to check, since two different requests filling different
  buffers with the same content suggests the driver copied the same sectors
  twice.
  The black frame is not a dump bug. `--dump-vram` and the new content
  box show all 91,379 non-zero pixels at `x >= 704`, the area the game
  uploads textures to, plus a 256x64 strip at `y = 448`; the 320x240 page
  and the display window `(0,32)`-`(320,272)` are entirely zero. The
  217,138 primitives were submitted inside draw bounds `0,32`-`639,479`,
  so they did land where the display looks and still wrote no colour: the
  guest is clearing to black rather than drawing a scene. A renderer
  change cannot produce a frame while that is true.

- **The fills are the guest's own black clears** (`R6_TRACE_FILL=1` prints
  each GP0(02h) fill with its position, size, colour and the current draw
  area). The first fill clears `0,0` 640x511; after that the guest alternates
  `pos=00200000 size=00E00140 color=000000` and
  `pos=01000000 size=00E00140 color=000000`, i.e. 320x224 clears at `(0,32)`
  and `(0,256)` — its two display pages — with a black colour every time. No
  geometry is submitted into either page; the 91,379 non-zero pixels are
  texture or palette data at `x >= 704`. So the black frame is what the guest
  asked for, and neither the renderer nor the display-area registers are the
  gate.

- **Where the main thread actually waits:** the voice-mask builder
  `0x8002E898` has exactly one call site in the resident image, `0x800335F0`,
  inside the sound tick `0x80033548` itself, and the mask store at
  `0x80033624` is that same tick's output. The tick builds the mask from the
  eight slot flags at `0x800B5508` (28 bytes per slot, word 0), and the
  wait at `0x800C9AB0` goes `task_wait 0x800C9874` -> tick; a zero tick
  return makes `task_wait` return 1, so the caller loops after a VSync.
- **Tracing the voice path:** `--watch-pc` over 250M steps shows the tick
  runs 795 times and the wait path `0x800C9AB0` 373 times, while the two calls
  that can start a slot, `0x8002E94C` and `0x8002E990`, execute **zero**
  times. `--watch-write` on three of the eight slot flags records only zeros,
  from the BSS clear at `0x80010B0C` and the slot reset at
  `0x8002E674`/`0x8002E70C`, and all eight flags read back zero at 250M
  steps; no slot is ever activated. The starters sit in substates 2/3 of the
  same state machine that waits in substate 1, so the voice path cannot be
  what unblocks it.
- **Who writes the stream task status byte (+4):** the sound tick, and only
  the sound tick. A static scan finds every immediate reference to
  `0x801E09E0`/`0x801E1430` inside `0x80033548`-`0x80034070`, and every
  load/store of the
  current-record pointer `0x800BCD2C` inside the tick or `task_wait`; no CD
  handler or DMA path touches the status. `--watch-write` on
  `0x801E09E4`/`0x801E1434` confirms it at runtime: bytes +5..+7 are cleared
  and +4 is stamped 3 at `0x80033E20` (795 times) and `0x80033824` (1,590
  times, once per record per tick). The two records are 2,640 bytes apart
  (`slot*2640 + 0x801E09E0`), so they are slots 0 and 1.
- **Where 4 and 8 would come from:** the same tick writes the completion
  statuses, inside the per-slot branch: `0x80033898` (+4 = 4) after
  `jal 0x8002EA78` returns 0 in a state other than 5; `0x80033878` (+4 = 5)
  when state == 5; `0x800338B0` (+4 = 10) when that helper returns nonzero;
  `0x80033A94` (+4 = 8) when the slot's stream word at `0($a0)` is > 0; and
  `0x80033AD8` (+4 = 9) in state 2 when the play word is 0. All of them sit
  after the mask test `srlv mask, slot; andi 3; beq 0`, and over 250M steps
  `0x80033898`, `0x800338B0`, `0x80033A80` (entry to that block),
  `0x80033A94` and `0x80033AD8` execute **zero** times while the two
  "write 3" sites run 2,385 times. The byte is therefore not stuck because a
  transfer failed: it is re-stamped 3 every frame before the branch that
  would advance it.
- **Why it stays 3:** the mask at `0x800BCD34` is built at `0x80033624` from
  the eight slot flags at `0x800B5508` (28 bytes per slot, word 0). With no
  flag set the mask is zero, so the tick takes the fast path at
  `0x80033E20` that re-arms the current record to 3, then at `0x80033F00`
  compares the status only against 8, 9 and 10 (`0x80033E88`-`0x80033EA4`);
  3 falls through to `0x80034070` and the tick returns 0. `task_wait` then
  gets a 0 tick return plus a record status it does not decode (it handles
  0/4/8) and returns 1, so the caller loops after a VSync. Both stream
  buffers do hold data (4,072 non-zero bytes each, shared head), so this is
  not a lost transfer: the handshake is gated on the voice slot, and only
  the starters `0x8002E94C`/`0x8002E990` set the flags that build the mask.
- **The substate-1 gate, traced:** the boot state machine is `0x800C99A4`
  with base `0x800BC728` (state +0x594, substate +0x598, sub-substate +0x59C)
  and dispatch table `0x800C9580` (`0` -> `0x800C9A34`, `1` -> `0x800C9A84`,
  `2` -> `0x800C9AE4`, `3` -> `0x800C9B10`, `4` -> `0x800C9B18`,
  `5` -> `0x800C9B30`). Handler 1 returns to the dispatcher unless the sound
  module's phase word `0x800BCD0C` is exactly 1, then loops on
  `substate = task_wait()` (the store at `0x800C9ABC` is the `jal` delay slot)
  until substate >= 2 (`slti`/`beq` at `0x800C9AD0`).
- **Why substate 1 cannot reach 2 in this image:** `task_wait` returns 1
  whenever the tick `0x80033548` returns 0 (`0x800C9884`-`0x800C9890`), and
  only decodes the records when the tick is nonzero: both statuses 0 -> 2
  (`0x800C98D4`), either 4 -> 3 (`0x800C9904`), either 8 -> 4
  (`0x800C991C`). The tick returns 0 on every path: it has a single `jr $ra`
  (`0x800340D8`), the only branch into the epilogue body is `0x800340A4`
  (`j 0x800340B0`), both branches to the shared return (`0x80033F10`,
  `0x80033F30`) sit after `sw $zero, 60($sp)` at `0x80033F00`, and the
  return-1 switch at `0x80034078` needs that local in 1..25. The mask block
  is only entered at `0x80033F38` from `0x80033F20`, i.e. after the zeroing,
  and its loop back-edge is `0x80034024 -> 0x80033F64`, also after it; the
  only stores to `60($sp)` are `0x80033588` and `0x8003366C` (both 1) before
  that point and `0x80033F00` itself. One path needs care: the status
  dispatch table `0x80025178` does point at the epilogue, `6 -> 0x800340A4`
  (return 1) and `7 -> 0x800340AC` (return 0), but it is dead code: the
  guards at `0x80033788` (`beq a2, s5` with `s5 = 3`) and `0x80033790`
  (`sltiu a2, 6`) divert `a2 == 3` to `0x80033850` and any `a2 >= 6` to
  `0x80033828`, so only indices 0-2, 4 and 5 reach `jr $v0`. Measured after
  rebuilding the tree over 150M steps: tick 441 calls, `0x800340A8` (return 1) 0
  times, `0x800340AC` (return 0) 441 times, `0x800C98D4` 0 times, loop check
  `0x800C9AD0` 18 times, substate `0x800BCCC0` still 1. So no record status
  can ever cause the transition here, and the two starters stay unreachable;
  status 3 is a symptom of the re-arm, not the gate.
- **The phase machine, traced:** `0x800BCD0C` is the sound module's phase
  word (0..15). `0x8003260C` dispatches on it through the table at
  `0x80024CA4` and is called once per frame from `0x800330C4`, `0x800C9780`
  and the boot dispatcher `0x800C9D74`; the handlers advance 0 -> 1 -> 2 ->
  ... -> 7 -> 0. Phase 0 is the restart handler at `0x80032834`: it clears
  the track handle `0x800B5C30` and the fixed-point position
  `0x800B8E94` (`0x80032838`, `0x80032844`) and then stores 1 with
  `sw $v0, 0x800BCD0C` at **`0x80032850`** (the jump is at `0x8003284C`, so
  the store is its delay slot). Phase 1 is the boot machine's handler
  `0x800C9A84`, which tests that same word and then waits. Phases 2-7 are
  the advance handlers `0x800326A4`/`0x80032730`/`0x80032760` (position
  decrement and clamp) and the start/stop paths `0x800C9678` (sets the
  handle to `0x333` and phase 6) and `0x800C970C` (sets phase 3).
- **Which write enters the wait, measured:** `0x80032834` and therefore
  `0x80032850` run 3 times over 150M steps, while handler 1 `0x800C9A84`
  runs 78 times (once per frame while the phase stays 1) and the updater
  `0x8003260C` 423 times. `0x800B5C30` cycles `0x333` <-> 0: the boot
  machine sets `0x333` at `0x800C9A4C`, `0x800C9684` and `0x800C9710`, and
  the phase-0 handler clears it at `0x80032838` immediately before asking
  for phase 1. So the machine enters handler 1 with the track already
  discarded, nothing armed for the driver, and the driver's tick returning
  0 — the wait is asked to complete a start whose track handle was just
  cleared.
- **Shortest path from the phase-0 restart to a frame:** phase 0
  (`0x80032834`, clears `0x800B5C30`) -> phase 1 (`0x80032850`) -> handler 1
  (`0x800C9A84`) -> the `task_wait` loop -> substates 2, 3, 4 -> handler 5
  (`0x800C9B30`, whose sub-substate loop calls the tick at `0x800C9C10`) ->
  handler 6 (`0x800C9C50`: phase = 3, track handle = `0x333`, substate 7) ->
  handler 7 (`0x800C9CA0`: waits for `phase & 7 == 5`, then calls
  `0x800333CC`, `0x80032428(320, 0)`, `0x8008EF20(0)` and `0x8008F048`, and
  hands off with main state 3 at `0x800C9D60`). That handoff is the first
  code in the boot that would put a scene on the display.
- **Where that path dies:** handler 1 needs `task_wait() >= 2`, which needs
  the tick to return nonzero, which the loaded image cannot do, so handlers
  5-7 never run. Measured: at 60M steps main state is still 1 (written at
  `0x80032EA4`) and the dumped display frame is **0 non-black pixels of
  76,800**; by 150M main state 2 has been entered at `0x800C973C` and handler
  1 `0x800C9A84` runs 78 times, all of them looping at `0x800C9ABC`.
  The tick `0x80033548` is not in the 79,872-byte boot EXE (which covers
  `0x80010000`-`0x80023800` exclusive), so it arrives in a disc overlay;
  given a retail title must boot, the one change most likely to produce a
  frame is to make the loaded sound overlay install a tick that can report
  work. That overlay's bytes should be verified against the disc before any
  further modelling, since a constant-0 tick is not plausible retail code.
  The two queued reads are not code (460/1,070 bytes of high-entropy data
  with a `0x00000944` header), so they do not install it.
- **The smallest real gap is that handoff:** the phase-0 restart drops
  `0x800B5C30` and requests phase 1, and the only code that could arm a
  voice slot for it (the starters at substate 5) sits behind the same wait.
  The gap is therefore the sound module's segment handoff — the phase-1
  request needs to carry the stream it just discarded — not SPU voices or
  the record status. The tick itself is not self-modified: the only writes
  to `0x80033548`/`0x80033F00` in 150M steps are the boot clear at
  `0x80010B0C`, so the constant-0 return is static code, not a patch we
  missed.
- **Kernel jump tables:** `GetB0Table` and `GetC0Table` used to return 0, so
  libcd's hook installer treated address 0 as the BIOS `CdRead` body: it wrote
  its five-word trampoline to `0x28` and stored `0x3C` in the `0xDFFC`
  dispatch cell, and the B0 side wrote its three-word stub to `0x9C8`. Both
  calls now hand out a modelled table in low RAM. B0 sits at `0x80000200`
  with one thunk per vector (`li $t1, N; jr $0xB0`), C0 at `0x80000380`, and
  C0 entry 6 names the CD entry table at `0x80000400`, whose `+0x70`/`+0x74`
  halves rebuild the `CdRead` body address `0x80001100`. B0 entry `0x5B`
  names the kernel block at `0x80001000`, which libcd patches at `+0x884`
  and `+0x9C8`; the block and the `CdRead` body are ordinary guest RAM, and
  the body reserves its `+0x28` patch window and a `+0x3C` resume entry. The
  body has no CD read behind it, so both entries return 0. Re-running the
  traced boot, the hook installs at `0x80001128`, `0xDFFC` holds
  `0x8000113C`, and the B0 stub lands at `0x800019C8` inside the modelled
  block. The old patch sites `0x28` and `0x3C` stay zero, and everything
  else is unchanged (682 sectors, 0 unhandled vectors, same CD counters).
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

## Test machine

The instruction rates, vblank counts and timing numbers quoted above were
measured on one desktop, built Release with MSVC:

- **CPU:** Intel Core i5-14400F
- **Memory:** 16 GB
- **GPU:** NVIDIA GeForce RTX 3050
- **OS:** Windows 11 Pro 10.0.26300

A 250M-step Silent Hill run takes about 45 seconds there. The Silent Hill
runs used a `.cue`/`.bin` pair rather than a CHD; the Spider-Man notes above
still come from CHD images.

## Running it today

`r6-player.exe` (built from `src/player_main.cpp`, also
`build\r6-player.exe`) is a
double-clickable windowed player. It finds your own `.cue`/`.bin`/`.chd`,
validates the ISO9660 descriptor before accepting an auto-discovered image,
boots the disc's executable and runs it at roughly six million instructions a second
with a live status line. It does not render the game yet: the vblank handler
and VSync callbacks run and the loader streams files, but the main loop has
not submitted a scene. See `docs/recomp-findings.md` for the current blocker.

The window title shows instruction count, primitives and display state; the
window body shows those counters plus how many disc sectors have been read.

## Native port (Spider-Man)

- **Executable:** `pc\SpiderMan.exe`, built from `src/port_main.cpp` plus the
  locally generated translation unit. No interpreter is linked into it.
  `tools/build-port-msvc.cmd <disc> <out-dir>` reproduces the build with the
  Visual Studio toolchain alone; the CMake targets do the same.
- **Translation:** `tools/r6-recomp` reads `spiderman.chd` directly, extracts
  `SLUS_008.75` (747,520 bytes at `0x80010000`, entry `0x8008739C`) and emits
  24,214 blocks / 146,599 instructions in the same ABI as
  `generate-native-stub.py`.
- **Presentation:** OpenGL 3.3, `shaders/psx.vert` and `psx.frag`, loaded from
  disk at startup. Frames per second is shown in the window title and as a
  5x7 bitmap overlay in the top-left of the presented frame; the overlay is
  drawn into a copy and never touches guest VRAM. Measured 40-60 FPS at 60 Hz
  frame pacing (1 ms timer resolution).
- **Differential:** `r6-native --compare` now matches the interpreter for
  3,000,000 blocks (registers, HI/LO, COP0, hardware, GTE, scratchpad, BIOS,
  event trace, all 2 MiB of RAM). The run previously stopped at block 18,552
  with SR `0x00000401` on the native side and `0x00000405` on the oracle. The
  cause was `syscall`: the guest uses libetc's critical-section API, where
  `syscall 1` masks interrupts and reports the previous state and `syscall 2`
  restores them (`SR |= 0x405`). The interpreter implements that, but
  `r6-recomp` emitted the instruction as a no-op, so the native run never set
  IEp. `NativeBus::syscall` now mirrors the interpreter and the translator
  emits it.
- **Runtime:** 120 fields (4,883,925 blocks, 67.7M cycles) with no
  recompilation miss, 145 interrupts delivered and 0 handler overruns, ending
  at `0x8008BAA0` with a vblank still pending, but the guest reads
  only 6 disc sectors (LBA 16-19, then `COMPILED.XA` at 66454) and then issues
  `CdInit` (Init/Demute/GetStat) every frame. It submits 0 GP0 commands and
  0 DMA2 lists, so VRAM stays black. Fixing `syscall` did not change this,
  which rules out the translated code as the cause.
- **Interpreter comparison:** the same disc on `r6-run --vblank --hot` reads
  141 sectors and parks at `0x8008CA8C` with hot PCs `0x8008CA7C`,
  `0x8008CA8C`, `0x8008CB4C` - the CD loader wait already recorded below. The
  port still stalls earlier, with a different sector sequence (16-19 then
  66454 instead of 390-397).

- **Resume entries:** every instruction now has a dispatch entry. A block is
  emitted as `body_XXXXXXXX(cpu, bus, entry)` with a `switch` that jumps to a
  `resume_XXXXXXXX` label at instruction N, each instruction in its own scope;
  `block_XXXXXXXX` is a two-argument wrapper for entry 0 and `enter_XXXXXXXX`
  wraps the interior entries, so an interrupt taken mid-block can set EPC to
  the instruction that did not run and the dispatcher resumes exactly there.
  The port now uses per-instruction interrupt delivery like the interpreter;
  `no translated block at 0x8008D0AC` no longer occurs. Instruction bodies are
  not duplicated, so the translation stays one copy of the code.

- **Where the port and the reference part company:** with matching interrupt
  policy, the port and `r6-run` produce identical BIOS call counts through
  1,220,000 cycles (7 at 400,000, 43 at 900,000, 51 at 1,220,000). They first
  differ at 1,240,000 cycles (55 calls against 54), and after that they
  separate completely: at 2,000,000 cycles the port has 88 calls and the
  interpreter 583, because the interpreter is in the CD driver reading sectors
  390-397 while the port is waiting. The port spends its time in the loop at
  `0x80084D80` (36,456 hits by 2,000,000 cycles): it decrements a stack
  counter and re-reads `0x800B397C`, branching back while
  `*(0x800B397C) < a0`, so it waits for a guest counter the vblank path should
  advance. It also spins in the copy loop at `0x800653F0`. That first
  divergent function is the next thing to fix; 3,600 fields still end at
  `0x8008BAA0` with 145,658,377 blocks, 3,727 interrupts, six sectors and
  `I_STAT` 1 left pending.
- **Where the CD loader leaves the reference:** the port and the interpreter
  issue the same CD commands through the boot sequence: Init, Demute, GetStat,
  Setloc 16, Setmode A0, ReadN (sectors 16-17), Setloc 18, ReadN (18-19).
  They part company on the next seek. The interpreter sends `Setloc 00:02:22`
  (sector 22, a subdirectory); the port sends `Setloc 14:48:04` (sector
  66454, a file inside `COMPILED.XA`), so it never reads `CD.HED` at 390 and
  stops at six sectors. Everything before that is identical: the same
  commands, the same sector bytes in RAM, and the same vblank counter
  (`0x800B397C` reaches 2 at 1.13M cycles in both). The state at 1.3M cycles
  differs in 56 words, including a file-position value at `0x800B3754`
  (0x1FF against 0x205, six bytes apart) and libcd state at `0x800B3DF0`, so
  the guest parses the directory from a position six bytes behind the
  reference and lands on the wrong entry. That six-byte skew is the bug to
  fix next, not the interrupt path: interrupts delivered at 1.3M cycles are
  16 in the port against 15 in the interpreter.

- **CdReadSync returns failure in the port only** (`--sync-trace`,
  `R6_TRACE_SYNC=1`; both log `0x8008A068` entry and the value of `v0` at
  `0x80086ABC`, where the guest runs `sltiu v0,v0,1` to turn a zero return
  into success). Every directory read in the port returns `v0=1`; every read
  in the interpreter returns `v0=0`. The variable behind it is libcd's
  read-complete flag at `0x800B1C60`: the port's CD handler sets it at
  1,245,478 cycles from block `0x80089BD0`, block `0x8008998C` clears it again
  at 1,254,668, and the sync samples it at 1,255,322 and finds zero, so it
  times out and reports failure; the interpreter still has `0x800B1C60 = 1`
  at the same cycle. This is not a missing interrupt: the port's `I_STAT` bit 2
  toggles throughout the read (`0x4` at 1,189,694, 1,199,154, 1,209,014, ...)
  and the handler runs. The failure is an ordering race between the sector
  handler/DMA and the sync's flag check, and it is what makes the game retry
  at 14:48:04 and overwrite `0x800C0CE0` before the walk.

- **The named ordering race cannot be verified (re-measured):** the
  addresses above cannot be the set and the clear. A 250M-step
  `r6-run --vblank` boot executes `0x8008A068`, `0x800899A0`, `0x80089AC8` and
  `0x800868B4` zero times (`R6_TRACE_SYNC=1` and `R6_TRACE_WALK=1` print
  nothing), and the same is true of the native port. Disassembling the resident
  image at those places shows a function prologue (`0x80089BD0`: `addu s2,a2`)
  and the iteration step of a callback walker (`0x8008998C`:
  `addu s0,s1`), not a store to `0x800B1C60`; a block-entry trace cannot say
  which instruction inside a block wrote the flag. The one ordering variable
  that really differs between the two runtimes is interrupt latency, so it was
  varied directly: `R6_IRQ_POLL=1` makes the interpreter deliver an interrupt
  after every instruction (the native port's policy) instead of every 1,024
  steps. At 250M steps the CD path is unchanged — 682 sectors and
  `status reads=3335 with response=603 response reads=534`, identical to the
  default poll — while a 5M-step run shows the knob working (418 BIOS calls
  against 416). The controller's sector rate was varied too:
  `R6_CD_SECTOR_DELAY=564480` (the real 1/75 s) reads 648 sectors, again with
  no `CdReadSync` failure. Those measurements cover the loader path the two
  runners share, where the port booted from the disc's EXE issues the same
  `Setloc 00:03:27` (LBA 102) as the interpreter and then stops at
  `0x800314EC` because the streamed overlay is not in its translation; that
  build never reaches the volume-descriptor or directory reads, so it says
  nothing about them. On the directory path the ordering problem is real but
  different from the one described above: the port reads the parsed table
  entry before it is set (see the lookup note earlier), and the bad extent
  follows from that, not from a lost `0x800B1C60` flag.

- **Debug knob: `R6_FORCE_SOUND_TICK=1`.** The interpreter rewrites the sound
  tick's return-0 stub (`0x800340AC`, `addu $v0,$zero,$zero`) to report 1,
  which is a debug-only override (the recompiled port does not honour it).
  It fires only once the sound overlay is loaded, because that address exists
  only after the load. Results: at **60M steps it does nothing** — main state
  is still 1 and the tick never runs, so the dumped frame is 0/76,800
  non-black. At 150M it moves the boot substate 1 -> 2 -> 4 -> 6
  (`0x800C9ABC`, `0x800C9C3C`) and handler 6 (`0x800C9C50`) starts counting
  `0x800B9FDC` toward its 181-frame wait (reached 100). At **400M** the counter
  is 714, handler 7 (`0x800C9CA0`) runs 77 times, `0x8008EF20` is called once
  and main state advances 2 -> 6 at `0x800C9D60`, so the tick really is the
  gate for the sound-init stage — but the display frame is still **0/76,800
  non-black**, so whatever comes after the handler-7 handoff has its own
  blocker and 400M steps do not reach the first scene. The knob is therefore
  a confirmed diagnosis of the sound wait, not a fix for the black frame.
- **Main state 6 and the scene-start routine, traced:** the boot machine
  sets main state 6 at `0x800C9D60` and clears the substate. `0x800BCCBC` is
  read only by sound-module code (`0x800335A0`, `0x800341A0`, `0x8003AA14`,
  `0x8003ACD8`, `0x8003B114`, `0x8003B4A4`), never by a renderer, so state 6
  is a sound-side stage rather than a draw stage. Handler 7's scene call is
  `0x8008EF20`, a five-phase stepper: it adds its argument to the counter
  `0x800AFDB8`, wraps it to 0..4, looks up an 8-byte entry in the table at
  `0x800AFDC4` (`{s16, s16, function}`), calls the function when it is not
  null, and publishes the entry's two halfwords to `0x800C4840`/
  `0x800C4844`. With a null entry it returns 1 immediately, so the caller
  sees "done" without drawing anything. (An earlier note put this table at
  `0x800BFDC4`; that is the render packet buffer, one 64 KiB bank too high.
  `0x800B0000 - 0x23C` is `0x800AFDC4`.)
- **What fills the table, traced:** exactly one boot pass, the LCG-XOR
  loader at `0x80010AD0` (`acc = (acc + 0x01309125) * 0x03A452F7`, then
  `word ^= acc`). `R6_TRACE_DECRYPT=1` shows its only two calls:
  `0x80010A20` returns to `0x80010A28` with dst `0x80024B60`, src
  `0x8010A600`, count `0x000A4B00` (674,560 bytes), and `0x80010A48` with
  dst `0x800C9578`, src `0x801E2600`, count `0x1000`. The table sits at
  offset `0x8B264` inside the big block, so it is part of the main data
  pack, not a separately loaded file. A `--watch-range 0x800AFD00:0x100`
  run over 400M steps counts 64 stores, all from the decrypt loop's store
  PC `0x80010B0C`; after boot the only write in the struct is the engine's
  state word `0x800AFDF0` from `0x8008F61C`. No code computes the table
  address as a store target.
- **Which disc file supplies it:** the pack is on disc at **LBA 207**,
  inside `SILENT.` (ISO extent LBA 100, 80,476,160 bytes; block offset
  219,136 in that file, 330 sectors). Decrypting the 2,048-byte user area
  of sectors 207..536 reproduces RAM byte for byte at `0x80024B60`,
  `0x800AFD00` and the table itself (`0x800AFDC4` = `0x0652_0642`). The
  shipped entries are `{0642,0652,0}`, `{0643,0653,0}`, `{063C,064C,0}`,
  `{0644,0654,0}`, `{063E,064E,0}`: the two halves are CD file IDs and the
  function words are zero **in the disc data**, so nothing failed to fill
  them. The stepper still publishes the pair on the null path, so the
  caller at `0x8008F080` can post loads for 1618 (and `0x8008F048` for
  1602) through `0x80010F68`.
- **Exact read command, traced:** `--trace-cd` with the new `cd=` field on
  every DMA3 entry shows the whole chain. libcd pauses at LBA 141, issues
  Setloc `00 04 57` (BCD MSF; absolute 357 becomes LBA 207 after the
  150-sector lead-in) at event 95, then ReadN at LBA 207 at event 97. The
  drive presents sectors 207..537 and the guest consumes 330 of them
  (207..536). For each sector one 3-word DMA copies the 12-byte header to
  `0x800224B0` and one 512-word DMA copies the 2,048-byte user area to
  `0x8010A600 + n*0x800` (all from `pc=0x80015C88`); the last lands at
  `0x801AEE00`. A Pause follows at LBA 538 (event 761), then
  `0x80010AD0` decrypts `0xA4B00` bytes from `0x8010A600` to `0x80024B60`
  at cycle 40,698,064 with `cdsectors=373` — 43 sectors read before the
  pack plus these 330. The second call copies `0x801E2600` to `0x800C9578`
  at cycle 44,410,196. The only earlier use of `0x8010A600` is a single
  sector at `cd=103`, overwritten by the pack.
- **The 1602 scene load completes:** a 400M-step run with
  `R6_FORCE_SOUND_TICK=1` and store watches on `0x800C4840`,
  `0x800C4844` and `0x800FDE00` shows the handoff publishing the pair once
  (`0x800C4840` = `0x642` at `0x8008F030`, `0x800C4844` = `0x652` at
  `0x8008F03C`) and then, as the very next watched event, the first fill of
  `0x800FDE00` (`0xD0002000` from the CD DMA store PC `0x80015C88`); 128
  stores land in the first 512 bytes. `0x8008F048` posts file 1602 there
  and the transfer retires, so this leg of the scene load is not the
  blocker. The ring's type-2 machine also runs at this stage: `0x800230B0`
  cycles 0 -> 1 at `0x80011528`, 2/3/4 at `0x80011618`, back to 0 at
  `0x800112E4`, 660 times in the run. The older note that `0x800114C4`
  never runs describes the boot before the handoff.
- **Still open:** the second half of the pair, file 1618, goes to the
  buffer whose pointer is `0x800AFDC0` (`0x800F5E00` at 400M) and is pumped
  by `0x8008F07C` from `0x80034B00`, not by the handoff. Whether that leg
  runs and what consumes `0x800FDE00` are the next questions.
- **New debug knob:** `R6_TRACE_DECRYPT=1` prints each `0x80010AD0` call as
  `dst`, `src`, `count` and `ra`. Interpreter-only, off by default.
- **Test target repair:** `tests/test_unaligned.cpp` was added by `f694247`
  but never listed in the `r6-tests` sources, so the target stopped linking
  (`run_unaligned_tests` unresolved). It is now compiled in and the suite
  passes again.
- **Resuming a captured state:** `r6-run` and `r6-port` accept
  `--state FILE` (an `R6S1` snapshot) plus `--entry ADDR`, matching
  `r6-native --snapshot`. Registers and RAM come from the snapshot and the
  disc still comes from the image, so a captured state can be traced without
  replaying the boot. A snapshot taken mid-transfer resumes into the guest's
  wait loop instead of continuing the read, because the controller's own
  state (sector position, armed transfer) is not part of the `R6S1` file;
  capture a state before a read is issued, not during one.

- **Ground truth from the disc closes the CD route.** A throw-away tool
  (`dump-sector.cpp`, built against `chd_image.cpp`) prints the header and the
  first 32 user bytes of any LBA straight from the image. The results:
  16 = `01 43 44 30 30 31 ...` (PVD, `CD001`, volume `PLAYSTATION`);
  18 = `01 00 16 00 00 00 01 00 00 00 07 00 2F F5 01 00 01 00 43 49 4E 45 4D
  41 53` (`CINEMAS`, the game's 24-byte record); 19 = the same record;
  22 = `30 00 16 00 ...` (an ISO9660 entry, length `0x30`, extent 22);
  390 = `sfxv.vab`, not `CD.HED`. The port's `0x800C0CE0` after its second
  transfer matches LBA 18 exactly, and the interpreter's buffer at its walk
  matches LBA 22 exactly: the two runs never held the same sector at the same
  time, because the interpreter reads 22 before walking while the port reads
  66454. Every byte either runner received is correct for the LBA it asked
  for, so there is no delivery divergence to fix -- the CD handshake is a dead
  end.

- **Translated execution does not diverge (differential harness).**
  `r6-native` built from `native_main.cpp` + the generated translation; five
  interpreter snapshots were compared against the translated run block by
  block (registers, HI/LO, COP0, hardware, GTE, scratchpad, BIOS, event trace,
  whole 2 MiB RAM): 300k blocks from 590k and 605k steps (which spans both CD
  transfers) and 200k blocks from 644k, 648k and 652k steps -- all matched,
  no divergence. The translated code is therefore not what makes the two runs
  differ.
- **The third `Setloc` operand, traced in both.** `PS` lines at `0x8008711C`
  (`CdIntToPos`) and `0x80086CA8` (`CdControl`) plus `RC` lines at the read
  entry `0x80086A6C` (call site, arguments, buffered record bytes). The first
  two reads agree exactly (`a1=0x10`, `a1=0x12`, same site `0x800864A4` then
  `0x80086540`). The third comes from the same site in both, `0x80086838`, but
  the operand differs: interpreter `a1=0x00000016` (sector 22), port
  `a1=0x000C0016`. Sector 22 is present in the low half; the high half carries
  `0x000C`. The buffer at `0x800C0CE0` also differs: interpreter
  `01 00 16 00 00 00 01 00` (byte-for-byte LBA 18) against port
  `01 00 00 16 00 00 00 00`, and after the first transfer interpreter
  `01 43 44 30 30 31 01 00` (the PVD) against port `00 01 00 43 00 44 00 30`
  -- the port's delivered bytes sit in every other byte with the upper byte of
  each pair zeroed, while the payload the controller hands out and the disc
  itself are both already verified correct.

- **DMA3 lands correctly; the earlier lane claim is retracted.** `W3` lines
  (behind `R6_TRACE_DMA3`) print every destination word the channel-3 transfer
  writes. For sector 16: `000C0CE0 <- 30444301` (`01 43 44 30`, `CD001`),
  `000C0CE4 <- 00013130`; for sector 18: `000C0CE0 <- 00160001` (`01 00 16 00`),
  `000C0CE4 <- 00010000`, `000C0CEC <- 0001F52F`, `000C0CF0 <- 49430001`
  (`01 00 43 49`). Destination address and byte order both match the disc, so
  there is no halfword-lane defect -- the buffer state seen at the read entry
  earlier was a different moment in the transfer.
- **The buffer is byte-exact where it matters.** RAM taken straight from
  `--dump-state` snapshots at 1.26M, 1.28M and 1.29M cycles reads
  `01 43 44 30 30 31 01 00 ...` (the PVD), then `01 00 16 00 00 00 01 00 ...`
  (LBA 18) -- identical to the interpreter. The `0C 0C ...` content appears
  only after the third read, which fetches sector 66454, so `0x0C` is that
  junk sector's payload and a symptom, not a cause.
- **The bad extent comes from a table, not the buffer.** The read is issued at
  `0x80086830` after `0x80086820 addu a1,a1,v0` / `0x80086824 lw a1,-2372(a1)`,
  i.e. a lookup in the parsed directory table (`LK` trace). The interpreter's
  lookup resolves to `0x800BF714` holding `2, 1, 0x0001F52F, "CINEMAS"`; by
  1.40-1.50M cycles both runners hold that same four-field record, so the
  parse itself agrees -- the port reads the entry while it is still unset.

- **Table range watch (`R6_TRACE_TABLE`).** Every access to
  `0x800BF6E0-0x800BF760` is logged with the block that made it. The port
  writes the whole range from `0x800873AC` (startup), then the parse at
  `0x800865C8` reads `0x800BF6E8` twice and `0x800BF714` twice and writes the
  record back into `0x800BF714`/`0x800BF70C`/`0x800BF710`; later reads come
  from `0x80086760` and `0x80086804`. Only six reads and 42 writes occur, so
  the values seen by the loader are in place well before the third read --
  the ordering problem is therefore not a missing parse but a timing offset:
  the port reaches each CD command roughly 3.3k cycles earlier than the
  interpreter per read (first `Setloc` at 1,210,646 vs 1,213,964; second
  1,255,384 vs 1,265,532; third 1,291,250 vs 1,305,026).

- **Value watch finds the entry (`R6_TRACE_VALUE`).** Logging the block that
  loads a given value shows the bad operand is loaded from `0x800BF6E8`, not
  from `0x800BF714`: `VALR 800BF6E8 = 000C0016 pc=800865C8` and again from
  `pc=80086804`. Snapshots date the write: the whole table (`0x800BF6E0`) is
  still zero at 1.26M cycles and reads `01 00 00 00 01 00 00 00 16 00 0C 00`
  at 1.5M, written by the block at `0x800873AC`. The interpreter holds
  `... 16 00 00 00` at the same entry -- one halfword apart. So `0x000C0016`
  is a packed pair: the correct low half `0x0016` merged with a junk high half
  `0x000C` (the filler byte of the sector-66454 payload), not a mistimed read.

- **Reference-emulator oracle: blocked on BIOS, and the one run was a
  different title.** DuckStation 0.1-11894 is installed on the test machine,
  but no retail PS1 BIOS exists there. With the MIT-licensed OpenBIOS (from
  the libreboot mirror, signature `OpenBIOS` at `0x78`) it booted a disc it
  identified as `Game: Spider-Man | SLUS-00875 | HASH-8604F3220A66CDB6`,
  `disc region: NTSC-U`, `Kernel initialized`. That is **not this title**: the
  image used here is Silent Hill (USA) and its `SYSTEM.CNF` boots
  `SLUS_007.07`, so the session is a different-title run and none of its
  output can serve as the CD command/INT reference for Silent Hill. That run
  also stops in the OpenBIOS shell anyway, because DuckStation fast-boots by
  patching a retail BIOS image and OpenBIOS reports
  `FastBootPatch::Unsupported`. Booting the extracted PS-X EXE directly
  (`-exe`, via a new local extractor) powers the machine off without kernel
  init. A retail BIOS dump *and* a Silent Hill image are therefore both
  required before an emulator run can supply the reference.

- **Root cause found and fixed: the translator's `lwl` was wrong.** The
  write-value watch named block `0x800865C8` (the directory-record copy) and the
  generated code showed its `lwl` emission taking the alignment-0 formula for
  every case: `(rt & 0xFFFFFF) | (aligned << 24)` where the alignment-1 case
  needs `(rt & 0xFFFF) | (aligned << 16)`, and the alignment-3 case must take the
  whole word. Stale bits of `rt` (here `0x0C` from a previous record) therefore
  survived into the extent, producing `0x000C0016` instead of `0x00000016`,
  which sent the third read to sector 66454. `lwr`, `swl` and `swr` were checked
  and are correct. After fixing `tools/r6-recomp.cpp` and regenerating:
  `TBLW 800BF6E8 = 00000016`, the third `Setloc` operand is `0x16` (MSF
  `00:02:22`), the walk parses **eight** records (`n=0..7` plus terminator),
  and a 3M-cycle run streams 90 sectors, last 469 (was 6 sectors, last 66454);
  a 20M-cycle run streams 128 sectors, last 128342. The frame is still empty
  apart from the frame-rate overlay (312 white pixels).
- **Why the differential harness missed it:** the translated run and the
  interpreter start from the same snapshot, so they carry the *same* stale
  register value into the block and both produce the same wrong result. A bug
  of this shape is invisible to an oracle that shares the input state; only the
  live two-run comparison exposed it.

- **The one-ahead position fix that was tried and rejected:** the drain trace
  suggested the drive was reporting one sector past the one the guest held
  (`cd5C` 17/19/66455 against the interpreter's 16/18/22), so `present_sector`
  was changed to advance `sector_` only when the next sector is actually
  delivered (`delivered_in_read_`, reset by ReadN), with `load_sector` no
  longer bumping it and the XA-skip path advancing on its own. After a
  `build-full.cmd` rebuild the result was unchanged: the same six sectors, the
  same `Setloc 14:48:04`, and the walk still reading three `0x0C` records, with
  the `0x80089AC8` trace byte-identical to before. That disproves the premise:
  `cd5C` is the guest's own copy counter, not the drive's position. The change
  was reverted.

- **The ordering fix that was tried and rejected:** `CdromDevice` handed the
  next sector over after 4,000 cycles, roughly 140x faster than a real drive,
  so the following sector arrived inside the guest's per-sector handshake.
  Setting `kSectorDelayCycles` to one field (564,480 cycles, the real 1/75 s
  sector rate) makes the boot sequence correct in rate but changes nothing
  about the failure: `CdReadSync` still returns 1 and the seek is still
  14:48:04. The sync's exit path is `0x8008A130` -> `lw v0,0(s2)` ->
  `0x8008A138 beq v0,zero,0x8008A14C`: when libcd's read-complete flag
  (`0x800B1C60`) is zero it takes the failure return, and the error branch at
  `0x8008A0E8` never runs. The flag is cleared by the guest's own drain path
  (`0x8008DA60` -> `0x8008996C`), so it is not something the device can guard;
  the device change was reverted.

- **The walk, captured in both runners** (`--walk-trace` on the port,
  `R6_TRACE_WALK=1` on the interpreter; both print `s0`, the record counter
  and the 24 bytes at `s0` for each iteration of `0x800868B4`):
  the interpreter walks eight real records out of `0x800C0CE0` with lengths
  `0x30, 0x38, 0x38, 0x36, 0x3C, 0x3C, 0x3C`, advancing
  `0x800C0CE0 -> 0x800C0D10 -> 0x800C0D40 -> 0x800C0D78 -> ...` (LBA 22 and
  size `0x800` in the first record) and stops on a zero length byte, which is
  the end of the directory. The port walks three times with length byte `0x0C`
  (`0x800C0CE0 -> 0x800C0CEC -> 0x800C0CF8`) over a buffer that reads
  `0C 0C 0C 0C ... 00 00`, i.e. the payload of the bogus sector-66454 read,
  not the directory. So the port's search does not stop in the middle of a
  good directory: the directory sector has already been overwritten before the
  walk starts, because the port issues the 14:48:04 read *first* and walks
  afterwards, while the interpreter walks straight after the 18/19 read.

- **Build trap: header changes were silently discarded.** `link2.cmd`-style
  relinks compiled the port sources against the current headers but linked a
  *prebuilt* `SpiderMan_translation.obj` produced from much older ones. The
  bus and the CD device are header-only classes, so both object files carry
  COMDAT copies of `NativeBus::write32` and `CdromDevice::load_sector`, and the
  linker kept the stale copy from the translation object. Every change made in
  `native_runtime.h` or `cdrom_device.h` therefore ran the old code while
  appearing to be applied — including all earlier attempts to trace the CD
  path, which printed nothing. Always regenerate the translation with
  `tools/build-port-msvc.cmd` (or compile `translation.cpp` in the same `cl`
  invocation as the port sources) when a header changes.

- **The CD path delivers the directory sector intact.** With a regenerated
  translation the DMA3 trace shows the guest's own reads landing correctly:
  a 3-word transfer of the 12-byte header to `0x800B382C` and a 512-word
  transfer of the 2048-byte payload to `0x800C0CE0`. For the volume descriptor
  the payload starts `01 43 44 30` (`"CD001"`) and the header reads
  `00 02 16 02`; for the directory the header reads `00 02 18 02` and the
  payload starts `01 00 16 00`, the game's own 24-byte record format
  (`u16 1, u32 22, u32 1, u16 7, u32 0x1F52F, u16 1, "CINEMAS"`). The device
  is not dropping or corrupting the sector, so the wrong seek is not a CD
  delivery bug: the interpreter issues the same first DMA pair with the same
  destinations, and diverges later in guest parsing.

- **The directory walk that picks the wrong file:** the guest's file search
  walks records with `lbu v0,0(s0)` at `0x800868B4`; `beq v0,zero,0x800869F0`
  at `0x800868BC` ends the walk, `lbu`/`addu s0,s0,v0` at `0x800869C4-0x800869CC`
  advances by the record's own length byte, and `slti v0,s2,64` at `0x800869D0`
  caps it at 64 records. The buffer it walks is `0x800C0CE0`, filled by the
  read issued at `0x80086A6C` (`Setloc` from `a1`, `CdRead(a0, 0x800C0CE0, 0x80)`,
  `CdReadSync`). In the port the parsed-name table at `0x800BF0E8` only ever
  receives `"."` and `".."`: `--watch-change 800BF128` records no write at all
  for the third slot, while the interpreter fills `0x800BF118` with `CD.HED;1`,
  `CD.WAD;1` and `COMPILED.XA`. By 1.295M cycles the port's buffer holds one
  24-byte record (`01 00 | 16 00 00 00 | 01 00 00 00 | 07 00 | 2F F5 01 00 |
  01 00 | "CINEMAS"`, the same shape as the game's own table at `0x80093A19`)
  instead of the raw 2048-byte directory sector, and a block at `0x8008D8CC`
  later writes `0x0C0C0C0C` over it — the guest is taking its read-failure
  fallback. The BIOS traces agree on the first 79 kernel calls and first part
  company at call 80, where the port runs `A(0x18)` strncmp against
  `"CD.HED;1"` (result -1, no match) and the interpreter runs `A(0x2A)` memcpy
  filling its table. So the wrong `Setloc` is a symptom: the sector data never
  reaches the guest's directory buffer intact.

- **Coverage rules learned:** code pointers stored in tables (the 13-entry mode
  table at `0x800B47C4`), `jalr` return continuations and ordinary function
  prologues all reach code that no call-site scan sees. They are seeded
  automatically. Interrupts are delivered per instruction in the port, and
  every instruction has its own dispatch entry, so EPC is always resumable.

## Gouraud geometry and the loading screen

The software GPU now rasterises gouraud-shaded polygons (`0x30`-`0x33` and the
four-point `0x38`-`0x3B` form), interpolating each of the three colours across
the span with barycentric weights. Before this the game's nine shaded draws per
run were decoded, counted and dropped; the run now reports `unsupported=0` and
`primitives=13` instead of `primitives=4` with nine unsupported commands.

That is not yet a picture, and the measurements say why:

- `R6_TRACE_FILL=1` shows the only fills the guest issues are black
  (`FILL pos=00000000 size=01FF0200 color=000000` and two 2-pixel bars). Black
  fills write zero-valued pixels, so `filled_vram_pixels()` stays at 0 even
  though the writes happen.
- The drawing area is still `(0,0)-(0,0)`, one pixel: the guest never issued a
  `0xE3`/`0xE4` pair, so any polygon is clipped to a single point.
- The guest is parked in its stage consumer: the retry loop at `0x8002AD30`
  calls `0x8002B430` (pop a ready stage) and decrements an 8M-iteration budget,
  with a `GetStat` health check at `0x8002AF60` that tests status bit `0x10`.
  All 48 stage entries stay at state 0 and the read stream stops after 128
  sectors (LBA 128342), so nothing refills the queue.

So the loading screen is what a real console would show at this point: the
pipeline that would deliver level data is waiting for a read that is never
requested. Changing the drive's `GetStat` "reading" bit to mean "a transfer is
in flight" was tried and left the trace byte-identical, so it was reverted
rather than kept as an unproven hardware change.
