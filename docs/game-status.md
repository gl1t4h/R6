# Game status

## Completed

None.

## In progress

### Spider-Man (USA)

- **Platform:** PlayStation 1
- **Region:** USA
- **Tested with:** a local `.cue` beside a single-track MODE2/2352 `.bin`
  (729,736,224 bytes, one `INDEX 01 00:00:00`). The CHD image this section
  described before was replaced on 27 November 2026; nothing below comes from
  a CHD.
- **Status:** 0% - Not Started
- **Progress:** 0%
- **Completed:** None.
- **Where the port stands:** `pc\SpiderMan.exe` is recompiled native code with
  no interpreter linked in. It opens the `.cue` sheet, follows its `FILE` line
  to the `.bin`, boots `SLUS_008.75` (747,520 bytes at `0x80010000`, entry
  `0x8008739C`, 24,214 translated blocks) and runs the guest's own frame loop.
  A 20 M-cycle headless run reads 128 sectors and stops at LBA 128341, then
  issues only `GetStat`; the 1400-field run described below streams `LOGO.STR`
  and decodes it, and still submits only 13 GP0 commands, fills three areas and
  writes no VRAM pixels, so the window stays black.
- **What is actually missing:** the CD driver feeds a 48-slot sector ring
  (`0x800C1510` = `0x80148AD4`: a 32-byte header then 2016 bytes of data per
  slot, 2048 bytes each). When the stream stops, slots 0-35 hold
  `ATVILOGO.STR` sectors 128304-128334 and 128336-128340, read from the sector
  addresses the driver stamps into each slot; 128335 is the XA-ADPCM sector the
  drive filters because the guest asks for mode `0xE0` (bit 6). The reader's
  index (`0x800C151C` = 39) is set by the chunk-release routine `0x80087340`,
  not by sector arrivals.
  The pending sector interrupt is not what stops the stream. With `--cd-log`
  sector 128341 is presented and acknowledged (`ack 7 int=1`); the handler at
  `0x80085000` then tests `*(u32*)0x1F801098`, the DMA1 (MDEC output) channel
  control, and calls `CdInit` (`0x8008CBC4`) when bit 8 is clear. `CdInit`
  waits for the VSync counter `0x800B397C` to advance 960 ticks (the target is
  in `0x800C6394`). Until 27 November 2026 the port modelled no MDEC at all,
  so DMA1 never reached the state the player checks for.
  A run long enough to clear that wait (`--frames 1400`, 790 M cycles) moves on:
  the player pauses, seeks to LBA 280000 and streams `LOGO.STR`.
- **MDEC model:** `include/r6/mdec.h` now implements the decoder the movie
  player drives. It covers the command/parameter handshake (`0x1F801820`/
  `0x1F801824`, commands 1-3, the quant and scale tables), incremental
  run-length decode with the 10-bit code format and the zig-zag scan, the
  separable IDCT scaled so a DC-only block comes out flat at DC/8, and the
  4:2:0 macroblock in 15-bit, 24-bit and monochrome output modes. Both DMA
  directions run in the recompiled bus and the interpreter: channel 0 feeds
  compressed words in, channel 1 writes the decoded blocks to RAM, and channel
  1 also transfers from the decoder's request line when it is left armed in
  request mode. The decode emits every macroblock whose bits have arrived
  instead of waiting for the last parameter word, because the player feeds a
  few words at a time and waits for the pixels before sending more.
  With that in place the same 1400-field run streams **202** sectors and stops
  at LBA 280061 (61 sectors into `LOGO.STR`), and the decoder reports **1,440
  macroblocks**, 16,608 words in and 276,480 words out (exactly 192 words -
  768 bytes of 24-bit pixels - per macroblock, so the player does read every
  decoded block back). `depth 2` says the stream is 24-bit output, not 15-bit.
  The frame is still black: 13 GP0 commands, 3 fills, 0 VRAM pixels, display
  last programmed off by GP1(03) = `0x03000000`, and the dumped frame is black
  apart from the 312 pixels of the FPS overlay. The player stops requesting
  sectors 240 macroblocks into the fifth frame and then only polls `GetlocL`,
  so the decoded blocks are never converted or uploaded to VRAM. Nothing was
  found in a 2 MiB snapshot that looks like a decoded frame, which fits the
  player waiting on something after the last transfer rather than a wrong
  pixel layout.
- **CD model fix:** a status command issued while a sector was still in the
  data FIFO used to clear that FIFO and overwrite the flag register, which
  would have dropped the sector's data-ready interrupt. It now keeps both and
  reports the command's own answer once the sector is acknowledged. `r6-tests`
  passes and the measured stall is unchanged.
- **How the driver collects a sector:** the movie's CD interrupt handler reads
  a sector in two DMA3 transfers, both in request mode (`CHCR = 0x11000000`),
  and publishes it through a 48-slot ring at `[0x800C1510]` (`0x80148AD4`) whose
  slots are 32 bytes and whose per-slot type halfword is at offset 0:
  1. `DMA3 8 words` - the sector's 32-byte chunk header into slot
     `[0x800C1514]` (`base + index*32`). The drive's data FIFO is laid out as the
     12-byte position/subheader plus the 2048 user bytes, so the header the
     guest reads is exactly the first 32 bytes of the stream record.
  2. It validates the slot: type `0x0160` (the record size), the demux id from
     the top half (`(word >> 10) & 0x1F` against `[0x800C14E8]`), the slice
     index (`word1 & 0xFFFF` against `[0x800C1504]`) and the frame number
     (`word2 & 0xFFFF` against `[0x800C14F0]`). A mismatch resets the stream.
  3. On a new frame (`slice == 0`) it wraps the ring: it copies the header just
     read into slot 0 and sets `[0x800C1514] = 0`, because slot 0 is the frame's
     header slot. This only happens when `lh 0(slot0)` reads 0, i.e. the
     previous frame's header slot has been consumed by the decoder.
  4. `DMA3 504 words` - the 2016-byte payload into
     `[0x800C150C] + slice*2048 + 32`.
  5. It publishes by storing 3 into the slot's type halfword, incrementing
     `[0x800C1514]`, and calling `0x8008DB44` to wake the decoder.
  A trace of one sector measures this exactly: `D3 ... words=8` into
  `0x80148EF4`, then `D3 ... words=504` into `0x801594B4` 408 cycles later, for
  79 sectors in the movie.
- **What actually stops the movie:** the header the drive delivers is correct -
  descriptor 39 in a snapshot holds the disc's bytes for sector 128384 verbatim
  (28 of 32 bytes; the other two fields are the game's own timestamp and type).
  What stops it is the driver's own flow control. Sector 128384 is the first
  slice of a new frame, and the wrap in step 3 found slot 0 still holding the
  previous frame, so the handler cleared the slot's type, set state 9 in
  `[0x800B1000]` and returned without reading the 2016-byte payload. The drive
  is then left holding that payload (`fifo=2016`, `acked=1`, nothing armed),
  and the model this note was written against would not present the next
  sector until it was drained, so the driver waited for another data-ready
  interrupt that nothing raised. The drive now gives up an acknowledged sector
  after one sector time and presents the next one (*Streaming ahead of the
  guest's drain* in the Silent Hill section); Spider-Man has not been
  re-measured against that model. The trigger is still on the consumer side -
  the decoder had not released the frame header slot - not in the sector data
  or the drive's registers.
- **CD model fixes for that path:** a status, parameter or read command no
  longer discards a sector whose bytes are still in the FIFO just because its
  interrupt has already been acknowledged, and a command's own answer takes the
  flag register first while the sector's data-ready interrupt is reported once
  that answer is acknowledged. Both are covered by `r6-tests`
  (`cdrom_status_command_keeps_the_pending_sector`,
  `cdrom_command_after_ack_keeps_the_sector`). A third change - keeping the
  data-ready interrupt asserted whenever uncollected bytes remain - made the
  guest jump to `0x00000000` after 15 fields and was reverted.
- **Drive timing is not the cause:** timing each sector at the drive's real
  2x rate (225792 cycles instead of 4000) was tried and rejected. The same
  refusal happens, 30 sectors into the movie instead of 79, with the payload not
  read at all (`fifo=2060`) and 227 macroblocks decoded instead of 2400. The
  constant is back at 4000, the value the rest of these numbers were measured
  with. Re-measured over 3000 fields, the 2x rate reads `1004 sector(s), last
  129225` - past the movie's own 171 sectors - while decoding 227 macroblocks:
  the guest gives up on the movie inside frame 1 and goes on to read whatever
  follows it, so it no longer hangs but never plays the stream either. Slower
  pacing does not fix the movie, because the guest's converter then starts on a
  frame whose slices are still arriving and grinds through the `0x33333333`
  pre-fill; faster pacing fills the ring. Both sides of that window are now
  measured.
- **Where the ring stops, measured slot by slot:** the ring is 48 slots of 32
  bytes at `0x80148AD4`, and every header read from it matches the disc image
  byte for byte (sector 128304 holds `0160 8001 0000 000A 0001 ...`: type
  `0x0160`, slice index 0 at +4, ten slices at +6, frame 1 at +8), with the
  write index advancing one slot per sector. **Publication runs once per
  sector**: the epilogue at `0x800858B0` stores 3 into the slot's type (payload
  staged), increments `[0x800C1514]`, and calls `0x8008DB44`, which sets that
  same slot to type 2 and moves `[0x800C1518]`; a trace prints `PUB p=0 w=1`,
  `p=1 w=2` and so on, one every ~6,000 cycles. The consequence matters: the
  consumer sees a frame as ready on its **first** slice, not its last. The
  reader gives up at a frame boundary when the slot it is about to reuse still
  holds a type: `0x800850E8`-`0x8008512C` stores state 4, and the new-frame wrap
  at `0x80085628`-`0x80085648` zeroes slot 0's type and stores state 9. In the
  4000-cycle trace that is sector 128385: the write index sits at 39 and the
  consumer index has just wrapped to 0, so the slot the wrap needs is one the
  player has already claimed. The ring is therefore full of frames the player
  has not consumed, and the bail is the reader's own back-pressure, not a
  misread header; the handshake timings are in the next note.
- **Why the release lands after the reader has bailed (measured):** the payload
  pool behind the ring is four frames deep, not two buffers. Releases come in
  order for payload index 0, 10, 20, 30 and then 0 again, and the reader's
  write index wraps at 40, so ten slots are reused four frames later. The
  release is the same handshake the reader waits on: `0x800872AC` only clears a
  frame's slots when its head slot reads state 4, which the consumer
  (`0x80086B10`) sets when the player claims that frame, and `[0x800C151C]`
  moves only by a frame's slice count inside the release - the consumer never
  advances on its own. The player needs 550,000-800,000 cycles per frame
  (releases at 6,386,556, 6,949,326, 7,489,000, 8,053,500 and 8,617,936 in a
  1400-field run) while the reader stages ten sectors in ~60,000 (a publish
  every 6,000). Trace of that run: publishes reach `p=39 w=39 c=0`, the
  new-frame wrap at 6,864,564 finds slot 0 in state 4 (`NEWF ... s0=4`), the
  reader bails with state 9 at 6,864,578, and the release that frees that slot
  lands 85,000 cycles later at 6,949,326 (`idx=0 ... frame=5`). After the bail
  the drive holds sector 128385's 2,016 bytes (`fifo=2016`, `acked=1`, nothing
  armed, `reading=1`) and presents nothing more - no publish occurs after
  6,864,968 in the whole run, and the player only drains the four frames still
  staged (releases at 7,489,000, 8,053,500, 8,617,936) before stopping at 2,400
  macroblocks.
- **Frame-granular delivery (implemented and measured):** `CdromDevice` now
  serves a streamed record's slices back to back and arms the next sector with
  `kFrameGapCycles` after that record's last slice. The record header is parsed
  from the sector's user data (`0x0160`/`0x0161`, slice at +4, count at +6), so
  only STR records are affected and ordinary file reads keep the 4,000-cycle
  cadence. Two bases were measured. On the drain-gated device a 700,000-cycle
  gap decodes 80 frames (24,000 macroblocks) and then stops with the drive
  holding 129,105's payload, so the gap has to cover the largest frame rather
  than the first; at 1,500,000 that base reaches `MDEC: 52320 macroblock(s)`,
  `CD: 4587 sector(s), last 10644`, `GPU: 44522 VRAM pixel(s)`. On the current
  streaming base the same 1,500,000-cycle gap gives `MDEC: 21057 macroblock(s),
  523680 word(s) in, 4042944 word(s) out`, `CD: 8402 sector(s), last 10645`
  with `reading=0` and `fifo=0`, and `GPU: 34626 VRAM pixel(s)` - against
  `MDEC: 1200 macroblock(s)`, 481,607 sectors to LBA 609,814 and **0** VRAM
  pixels with the streaming model alone. The stream at 128,304 is only 31
  sectors and frames 1-10 (the record type changes at 128,335), so frame 9 is
  inside it and now stages; the run then plays on into the following streams
  and the guest stops the drive itself at LBA 10,645 instead of running the
  head over the whole image. **Swept on the streaming base** (each value run to
  completion over 12,000 fields, and each value repeats exactly, so the runs
  are deterministic): 50k-300k never reach the video (0-3,600 macroblocks, 0
  video pixels); 400k-590k decode part of a stream and stall mid-chain (last
  sector 160,000-416,000); 600k and 620k complete (last 10,645, `reading=0`,
  15,138 and 15,510 VRAM pixels); 640k-660k stall again; and 680k, 700k, 800k,
  900k, 1M, 1.2M and 1.5M all complete. Completion is therefore not monotonic
  in the gap: there is a completing window near 600k-620k and a wide plateau
  from 680k up. `kFrameGapCycles` is 700,000: completion at 600k/620k is only
  a 40k-wide window, so the constant sits on the wide plateau instead.
- **How a movie frame reaches VRAM, and what was dropping it:** the player does
  issue the upload. Its `LoadImage` at `0x80082EF4` is called 2,657 times in a
  2,600-field run, from the queue flush at `0x800835D0`, with the two MDEC field
  buffers as the source: 20 strips per frame of 24x240 at VRAM x = 0, 24, ...
  456 and y = 0 or 256 - a frame per display page. What the strip routine does
  not do is send the pixels from the CPU. It writes the GP0(0xA0) header, the
  copy rect and the first two words, then programs **DMA2 in request mode**
  (`MADR`/`BCR`/`CHCR` at `0x1F8010A0`/`A4`/`A8`, `CHCR = 0x01000201`,
  `BCR = (180 << 16) | 16` - 2,880 words == 24x240 at 15-bit). The port executed
  DMA2 only for linked-list mode, so every burst was dropped and the 0xA0 packet
  never completed; the display kept showing whatever else was in VRAM, which is
  how the 24-pixel-wide texture strips at (0,0) came to be on screen.
  `run_dma2_request()` now forwards the burst to GP0 in both the recompiled bus
  and the interpreter, and a trace of the upload shows `IMG 0,0 24x240 words=2880` for
  each strip: the movie now covers the screen and the atlas stripes are gone.
- **What the screen actually shows now: black.** The 24x240 uploads carry
  `nzw=0` - every pixel word zero - and a RAM snapshot at 1,400 fields has
  `0x80160ADC` and `0x801637E4` reading 5,760 zero halfwords out of 5,760. That
  is the right answer for this stream: the reference decoder reports `300 of 300
  macroblock(s), 0 damaged, 21664 of 21696 bit(s) used` and **0 lit pixels**, so
  the frames the player is showing really are black. The dumped frame is black
  apart from the port's own 312-pixel `60 FPS` overlay and a 9,899-pixel patch
  of noise at (120,96)-(319,183) - 3,949 distinct colours and no structure, so
  not a decoded frame. **No picture appears**, and the reason is no longer the upload: the run halts at 3,299
  fields with `no translated block at 0x80189B3C` (identical before this change,
  so it is not caused by it). The guest jumps into code it loaded into RAM at run
  time - the bytes there are valid MIPS, writing state at `0x80199B64` and
  `0x801A1B74` - which the static recompilation has no block for. The
  content-bearing stream the disc also carries (`286,228`: `241 of 300
  macroblock(s)`, ~63,000 lit pixels) is read before that halt but its frames are
  never displayed.
- **Drive-side release re-tested and rejected:** letting a sector the guest
  acknowledged but never drained expire after three sector times does fire, and
  it is worse - the reader loses a slice, resynchronises and re-reads the disc:
  1400 fields then read 54,730 sectors (last 182,944) for 1,800 macroblocks
  instead of 2,400. It was reverted. The model now releases such a sector after
  one sector time instead of three, and the Silent Hill stream resynchronises
  the same way: the sector index runs to about 175,112 and then restarts at
  about 143,549, with the frame still black (*Streaming ahead of the guest's
  drain*).
- **STR route (`tools/str-frame.cpp`):** the demux and the decoder both work
  now, and the tool writes a real picture. It walks the image from a given LBA,
  groups video records into frames by the record header's frame number, skips XA
  audio sectors, trims each frame to its coded size and decodes it, with no
  console, drive or interrupt involved. The extraction was already verified:
  guest RAM at `0x801490D4` holds the same bytes the tool takes out of the image
  (2,710 for frame 1). What the record headers say: `320x240`, 10 slices per
  frame, coded size 2,712 bytes at frame 1 rising to 13,460 by frame 91, and no
  MPEG start codes anywhere in the payload.
- **The stream's real format, pinned against a reference decoder:** a static
  ffmpeg build decodes this stream through its `psxstr` demuxer, which emits
  *MDEC* packets, not MPEG-1 video, and `libavcodec/mdec.c` decodes those. The
  format is intra coding bit-packed in the MPEG-1 style:
  - every 16-bit unit is byte-swapped, then bits are read most significant first;
  - 4 bytes of preamble, then a 16-bit quantiser scale (`1` here) and a 16-bit
    version (`2`, which means the 10-bit absolute DC form, not the differential
    one);
  - macroblocks are coded **column major** - down a column before moving right -
    and each carries six blocks in the order Cr, Cb, Y0, Y1, Y2, Y3, with the
    four luma quadrants TL, TR, BL, BR;
  - a block starts with a signed 10-bit DC whose value is simply **doubled**
    (`DC = 2 * value`, no offset; the picture is centred by adding 128 after the
    inverse transform), then MPEG-1 run/level codes with one sign bit each,
    dequantised as `(level * qscale * matrix[raster]) >> 3`, with the run
    counting from the first AC coefficient (scan index 1), `10` as end of block
    and `000001` as escape.
  That is what the port's `MdecDevice` was missing: it reads 16-bit run/level
  words and continues until the scan passes 63, which cannot parse this bit
  stream. Feeding the STR payload to it gave 150 blocks out of 1,200 and flat or
  checkerboard images; the bit-packed reader gives
  300 of 300 macroblocks, no damaged blocks, and uses 99.9% of each frame's bits.
- **How the tool's frames compare with the reference:** frame 1 decodes to a
  luma mean of 0.0 against ffmpeg's 0.0 (a black frame); frame 61 gives a mean of
  4.5 against 4.7 with a luma mean absolute error of 1.76; frame 91 gives 9.8
  against 10.2 with 3.53. The residual is in bright detail, and it is ffmpeg's
  side that is weaker there: it logs `ac-tex damaged` on frames in this range,
  where its own parse runs past the end of a block, while this decoder consumes
  every frame's bits without provoking one. Swept and rejected while pinning it:
  AC scan starting at 0 (luma error 31.6 instead of 1.8), raster instead of
  zigzag scan order (3.88 against 3.56), an inverted sign bit, a different
  dequant rounding, and AC amplitude scaled 0.5 to 1.25 (error moves by 0.06, so
  amplitude was never the problem).
- **What the movie actually shows:** a black screen for the first second or so,
  then pale blue-white shafts of light sweeping in from the left and the right
  and converging, and by about frame 130 the **ACTIVISION** wordmark in blue on
  black, with the logo's own glow behind it; the picture then fades back down
  before the next scene. Frames are visibly 8x8 blocky in the bright shafts - a
  flat-colour, low-bitrate encode - which is how the stream is, not a decode
  fault.
- **Start here:** `pc\str-frame.exe "Spider-Man (USA).bin" 128304 130 logo.bmp`
  writes that wordmark frame; a `.bmp` path gives a picture Windows opens as it
  is, a `.ppm` path gives the plain bytes. The arguments are the LBA to start
  scanning at, the frame index from there (zero based), the output file, and
  optionally a coded width and height. The `work/` folder holds the ffmpeg build
  and the reference YUV dumps; neither is committed.
- **What the player actually hands the decoder, settled:** the DMA0 buffer at
  `0x800FDAC8` is 1,824 words (7,296 bytes) and holds exactly 1,800 code words
  followed by 24 `FE00FE00` padding words. All 1,800 are one repeated six-word
  macroblock - `0400 0400 0600 0600 0600 0600` - so chroma DC 0 in the Cr and Cb
  words and luma DC -512 in the four Y words, with `FE00` in the high half of
  every word as the code that takes the scan past coefficient 63. Decoded, that
  is uniform luma -128 (pixel 0) and chroma 0 (pixel 128): a black frame, the
  same picture ffmpeg reports for frame 1 (luma mean 0.0, chroma 128).
  This is a **valid 16-bit MDEC stream of a black frame, not a fill pattern**,
  and three things rule out a blind fill: the length lands exactly on 300
  macroblocks of six blocks, the two chroma words differ from the four luma ones
  (a fill cannot make that distinction), and every block is terminated where the
  code format requires it. The old reader's 300 macroblocks were therefore a
  correct decode, and the earlier note here calling them a fill pattern was
  wrong.
- **Consequence for the port:** because the player feeds 16-bit code words, that
  is the reader `MdecDevice` has to have, and the bit-packed STR reader went
  back to `tools/str-frame.cpp` where the disc's own format belongs (the native
  movie path). With the code-word reader restored the device decodes 2,400
  macroblocks (8 black frames of 300) and 460,800 words, which is 192 words per
  macroblock - the 24-bit size the player asks for.
- **Where the movie stands:** `MDEC: 2400 macroblock(s), 14,656 word(s) in,
  460800 word(s) out, depth 2`; `GPU: primitives=13 vram_pixels=0 bounds=none`;
  the 320x240 shot has 312 non-black pixels, only the port's own "60 FPS"
  overlay, and a mean channel value of 1.04. Decoding is not the blocker: the
  player never uploads the decoded frames to VRAM, and it stops asking for more
  video at 171 sectors (last 128385), which is the same ring-slot refusal
  recorded above.
- **The buffer is a real conversion, not a template:** the writes come from the
  game's own converter at `0x8002A3A8`-`0x8002A7D4`. It holds a 160-bit window of
  the staged payload (`lw` of five words at `0x8002A3AC`, advancing `0x14` a
  step), byte-swaps the halves, and emits one 16-bit code word per block:
  `0x8002A478` stores the DC word (`0x0400` chroma, `0x0600` luma) and
  `0x8002A454` stores the `0xFE00` end-of-block. A write watch over the 1,400
  fields counts 4,800 `0x0400`, 9,600 `0x0600` and 14,784 `0xFE00` stores into
  the buffer - one DC plus one EOB for each of 8 x 1,800 blocks - and the same
  watch catches it reading the payload at `0x8002A3A8`/`0x8002A4C0` while it
  writes. There is a pre-fill as well: `0x800653F0` writes `0x33333333` over the
  whole buffer three times, and one CD DMA of exactly 1,824 words (7,296 bytes,
  the whole buffer) lands there once; the conversion overwrites it word by word,
  which is why the decoded stream shows none of it.
- **Why the eight buffers are identical:** because the eight source frames are.
  The reference decode gives the same digest for frames 1-10
  (`b47ba8839f8f`), so an identical code stream is the right output for eight
  identical black frames, not a sign of a template.
- **Why nothing reaches VRAM:** the run issues 31 GP0 commands in 1,400 fields
  and not one is an upload - no `0xA0`. The image contains exactly two
  `0xA0000000` (GP0 upload) sites, both in the display code at `0x80082FF8` and
  `0x8008302C`, and neither is reached. The decoder side did its job: DMA1 moved
  2,880 words per transfer, 20 transfers per frame, into `0x80160ADC` and
  `0x801637E4` (the two field buffers), so 57,600 words per frame are sitting in
  main RAM. What stops is the player's own loop: it polls the stream ring
  (`0x8002B430` -> `0x8002B3CC` -> `0x80086B10`), then calls the CD library for
  `GetStat` (`0x80086CA8` with command `1`, no parameter, result buffer on its
  own stack) and tests bit `0x10` of the first response byte. That bit is the
  drive's shell-open flag, so `0x8002AF84` into the wait and teardown at
  `0x8002B18C`/`0x8002B1FC` is an abort path - and it is never taken. All eight
  polls answer `22 00 01 ..`: motor on (`0x02`) plus reading (`0x20`), with bit
  `0x10` and the seek/id error bits clear, so the player keeps looping. Nothing
  in this software stack sets bit `0x10`, and bit `0x02` is never cleared; the
  drive reports healthy at every single poll.
- **What the movie actually stops on:** the ninth frame's VLC conversion. The
  player starts iteration 9, calls the ring poll, and never returns. PC samples
  every 100,000 cycles from cycle 9,100,000 to 12,000,000 all land inside the
  converter at `0x8002A3A8`-`0x8002A7D4`, with `ra=0x8002A424`: the source
  pointer walks `0x8015C588` -> `0x8016A018` and the output pointer `0x800FE0CC`
  -> `0x8011CC76`, while the 160-bit source window reads `33333333 33333333
  33333333 33333333` - the `0x800653F0` pre-fill, not a frame. That buffer was
  never given frame 9's bitstream, so no `FE00` end-of-block ever arrives and
  the loop is still grinding through the fill three million cycles later. The
  device log's final entries are `GetStat` polls at LBA 128385 with that sector
  left unread, so the reader stops at the same frame boundary. The subsystem at
  fault is therefore the CD-to-ring handoff - frame 9's payload is never staged
  - not the drive status and not the decoder.
- **So the upload is not missing, it is never asked for.** The decoder, the
  DMA1 delivery and the ring all behave for the eight frames that arrive; the
  ninth never gets data, and the player hangs in the converter long before it
  could ask the GPU for anything. The reader leaves LBA 128385 unread because
  its own back-pressure bails at the frame boundary first: slot 0 still held
  the previous frame's type, so it zeroes that slot, records state 9 in
  `[0x800B1000]` and returns without staging the sector. Frame 9's payload is
  never staged for that reason, and it cannot be released from the drive side -
  the   release has to come from the player consuming frame 8, which it does
  90,000 cycles too late. **Before frame-granular delivery the movie did not
  play past eight frames** - 2,400 macroblocks at 4,000 cycles per sector, 227
  at the real 2x rate; the delivery change below takes it to 52,320.
- **Where this route dead-ends:** the 1400-field run submits 13 GP0
  primitives but uploads no pixels, because the video never reaches the display
  stage. (The 2,400 macroblocks it reports are eight black frames of 300,
  decoded correctly from the code words the player feeds it; what never happens
  is the upload, and the reason is the flag above, not the decoder.) Every
  further CD/register change is chasing a refusal the guest makes on purpose
  while it is waiting for its own decoder. The two ways forward are (a) work the
  player's frame pipeline instead - the consumer that clears the frame header
  slot, which is where the refusal comes from - or (b) bypass the PS1 CD
  subsystem for the movie: the streams are ordinary STR records, so a native
  port can demux `ATVILOGO.STR`/`LOGO.STR` from the disc image and feed the
  existing MDEC model directly, which is also the shorter road to a first frame.
  The `0x1F801098` test passes while the movie runs: the whole 1400-field run
  issues two `CdInit` sequences (boot and movie start) instead of resetting the
  drive every field.
- **Why the port stops at 3,299 fields: the engine module.** The guest builds the
  name `shell.bin`, rewrites its extension to `shell.rel`, opens and loads it
  (`0x8001BAA4`-`0x8001BAF4`: `0x80064B3C` open, `0x800651C8` allocate,
  `0x80064D28` load, `0x80064F3C` close, `0x8001BF58` relocate) and then calls its
  entry through the pointer the loader stored: `jalr ra, v0` at `0x8001BB18`, with
  `ra = 0x8001BB20` and `v0 = 0x80189B3C`. `shell.rel` is not an ISO file - the
  ISO holds only `SYSTEM.CNF`, `SLUS_008.75`, `CD.HED` (12,525 bytes at LBA 390),
  `CD.WAD` (63,213,568 bytes at LBA 397) and `CINEMAS/*.STR`. `CD.HED` is the
  archive index and names 314 members (`*.rel`, `*.bin`, `*.psx`, `*.vab`), so the
  module is a member of `CD.WAD`. In RAM it starts at `0x80189A88` with a packer
  header `HIER` (`0x24`, `0x20000`, `0x40000`, ...) and runs `0x1B9C4` bytes to
  `0x801A544C`; none of its code bytes occur anywhere in the disc image, so the
  loader decompresses it into RAM and the entry exists only at run time.
- **What executing it would take:** the port has no dynamic-code path. The
  dispatcher maps PC to a pre-translated function and the port links no
  interpreter, so any PC outside the 24,214 translated EXE blocks is a hard stop
  - which is what `no translated block at 0x80189B3C` reports. Three routes,
  cheapest first: (a) an interpreter fallback using the existing
  `MipsInterpreter`, which already takes the same `Memory&` and
  `PlatformServices&` the native port uses; on a dispatcher miss, hand it the CPU
  state and run until the PC is back inside a translated block. (b) translate
  blocks on demand at run time, adapting `tools/r6-recomp.cpp`. (c) translate the
  decompressed module image offline at its observed load address - cheapest, but
  it hardcodes an allocator result and has to be redone for each of the archive
  members loaded this way. (a) is the route that generalises.
- **Why the movie stops: the drive runs ahead and the reader rejects the
  stream.** Instrumented with cycle-stamped traces of ring-slot stores, the ring
  control globals, MDEC commands and every sector the drive presents:
  the second movie is set up at cycle 854,061,748 (ring base moves to
  `0x80166ADC`) and is streamed with one continuous `ReadS` (`0x1B`) issued at
  854,102,018 - the last non-`Getstat` CD command of the run. The port presents
  sectors on the drive's own cadence and, by the design of that model, threw
  away any the guest had not read: a sector was expired one sector time after
  its acknowledgement and the next one was written over it. The guest's
  software convert path costs 22.3M instructions, so while it decoded frame 61
  the drive presented ~19 sectors it never collected: frame 61 slices 2-8, frame
  62 slices 0-8 and frame 63 slice 0. The next sector the reader did collect
  carried frame 63 while it expected frame 61, so the check at `0x800853DC`
  (slice, header +4) and `0x800853F8` (frame, header +8) failed and the reject
  path at `0x80085404` ran at cycle 899,498,596: it resets the expectations to
  frame 0 / slice 0 (`0x800C14F0`, `0x800C1504`), zeroes the ring-slot headers
  from the read index to the write index through `0x8008DB04` (a 32-byte-stride
  store loop over the ring, not a seek - it touches no CD register), sets the
  player status `0x800B1000` to 6 and returns. That path contains no Setloc and
  no ReadS: it is the game's own terminal "stream error" exit, so a drive that
  has taken a sector away cannot be recovered by patching the reader - the fix
  has to be not taking the sector away.
  It never recovers - the last CD command byte is a `Getstat` at 899,807,404, and
  after that only register selects and request bits are written, so no read is
  re-issued and no slot is ever published again. From then to the end of the run
  the guest sits in the movie poll (`0x8002B430`), the claim (`0x80086B10`) and
  the player loop pages - 275M instructions - with zero frames: convert
  (`0x8002A338`) and release (`0x800872AC`) last run at 899,359,216 / 899,359,266,
  the MDEC is never armed again (frozen at 12,681 macroblocks, 259,776 words in),
  and nothing new is uploaded. The drive keeps "streaming `L1M1.STR`" only because
  of that run-ahead: 6,785 sectors, from LBA 280,621 (`LOGO.STR` frame 63) through
  288,374 (`L1M1.STR` frame 616), were presented and discarded unread.
- **Fixed: the drive holds uncollected sectors and re-offers them.**
  `CdromDevice::presentation_allowed()` no longer lets an unread sector expire:
  the next sector enters the FIFO only after every byte of the current one has
  been collected, and it is never replaced. Holding alone deadlocked the guest,
  because its data-ready handler (`0x80085000`) returns with status 4 when the
  ring slot at its write index is still marked busy - it only looks again on the
  next data-ready interrupt, and a quiet drive never sends one. A held sector is
  therefore re-armed every `kReofferCycles` (50,000) with the FIFO rewound to its
  first byte, so a repeated header read is harmless and the handler retries the
  same sector: nothing is overwritten and nothing wedges. The inter-frame gap is
  1,300,000 cycles, measured rather than guessed: at 1,200 fields a 700,000
  gap stalls after 64 decoded frames, 1,000,000 after 100, and 1,300,000
  completes; 1,600,000 also completes but reaches the same point later.
  Measured with the shipped values (`--frames 1600`, halt at 1,320 fields):
  **52,320 macroblocks decoded = 174 whole frames** (was 12,681 = 42), 1,287,040
  words in and 10,045,440 out, 4,587 sectors read, the intro finishes, `LOGO.STR`
  streams to LBA 282,140 (the first sector of `L1M1.STR` is 282,216), and the run
  then goes on to load the engine module out of `CD.WAD` (last LBA 10,644) and
  halts on dynamic code at `0x8014D5AC`. The movie is no longer where the run
  stops; the dynamic-code gap is.
- **Does the re-offer path ever fire, or does it only spin?** Traced over a
  950-field run it fires **7 times, all in the first 2.6 M cycles**, each with
  the FIFO still at offset 0 (sectors 17, 19, 23, 397, 128,304, 23 and 468 at
  cycles 1,251,542 to 2,566,664). The drive is not stalled at any of them: the
  stream continues past each, reaches 4,020 sectors by 940 fields, and no
  re-offer is issued once the movie is running. It is therefore not a spin loop
  over a sector the guest keeps declining - it is what lets the boot-time
  declines retry at all - and it stays, alongside the hold rule.
- **Fixed: the three-colour stripe pattern was the display mode, not the
  upload.** The player programs GP1(08) with `display_mode = 17` (`0x11`: 320
  wide plus bit 4, the 24-bit framebuffer bit) and double-buffers between
  display areas (0,0) and (0,256) - both observed in the VRAM dumps at 700 and
  750 fields. `SoftwareGpu::display_frame()` ignored that register and always
  decoded 15-bit pairs, so a 24-bit buffer whose bytes are `1C 08 0A` repeated
  came out as three VRAM words repeating across every line; those three colours
  accounted for about 56,700 of the 61,329 non-black pixels at 950 fields.
  `display_frame()` now decodes three bytes per pixel (one VRAM row, 2048 bytes,
  per 960-byte line) whenever `display_mode_ & 0x10` is set, and leaves the
  15-bit path untouched. A 940-field capture with the corrected decoder
  (45,120 macroblocks, LBA 281,890) has **58,603 non-black pixels in 4,651
  colours**, no repeating cycle: a dark red field with a picture-shaped mass in
  the middle instead of stripes.
- **The row layout the uploads describe.** A frame arrives as **20** GP0
  uploads of 24x240 at x = 0, 24, ..., 456 (the same 20 again at y = 256 for
  the back buffer, and 20 of 24x192 at y = 24 and 280 for the atlas), not 13.
  A 24-pixel strip writes 24 VRAM pixels, which is 48 bytes of the line, so
  20 x 48 = **960 bytes per line**, contiguous: the 700-field VRAM dump has
  every 24-pixel slot written across its whole 48 bytes (8-12 nonzero words in
  the second half of each slot on row 120) and no 12-word gaps anywhere. 960
  bytes is exactly 320 pixels x 3 bytes - the width GP1(08) reports with bit 4
  set - and because the two buffers sit 256 rows apart with 240 lines each, one
  frame line occupies one VRAM row: **stride 2048 bytes**. That is what the
  decoder implements. The 624-byte reading (13 strips of 24 bytes) is not what
  the guest issues: a 24-pixel strip is 48 bytes and there are 20 of them.
- **What is on screen is still not a picture.** That 940-field frame is not the
  pale-blue converging shafts or the ACTIVISION wordmark the reference decoder
  shows for `LOGO.STR` frames: pixel-for-pixel it matches no reference frame
  (mean absolute error 33/255 against every frame tried from 30 to 150), and a
  second capture at 700 fields looks the same way. The frame does reach the
  full 960-byte line (row 120 of the dump carries data in slots 1-16 of 20, the
  rest of the line dark), so this is not a width or placement problem: what the
  guest converts into that buffer is what is wrong.
- **The block the decoder hands the player is not the stream's picture.**
  Frame 151 of `LOGO.STR` (the frame on screen at 940 fields), decoded by
  `MdecDevice` in its 24-bit mode after the MPEG-1 intra matrix is fed, compared
  with `str-frame`'s decode of the same frame: channel correlation **0.00**
  (0.002 at frame 60), and unchanged under every byte order tried for the
  payload (0123, 1032, 3210, 2301). Of the 230,400 output bytes 115,120 clamp to
  0 and 58,768 to 255; the reference frame's luma is mean 8.4, standard
  deviation 25.2, while the device picture is mean 97, deviation 113. The device
  also finds 225 of the 300 macroblocks and spends 20 words per macroblock where
  the stream holds 15. The cause is the compressed-data model, not anything
  downstream of it: `MdecDevice::decode_block` reads a fixed 6-bit run / 10-bit
  level out of every 16-bit half, with a per-block quantiser scale in the top
  six bits, while the MDEC stream is MPEG-1 intra coding, as libavcodec's
  `mdec.c` implements it and `tools/str-frame.cpp` reproduces to 1.9 luma MAE:
  byte-swapped 16-bit units, four preamble bytes then a frame-wide `qscale` and
  `version`, MPEG-1 run/level VLC with a per-coefficient sign bit and `000001`
  escape, and six blocks per macroblock ordered Cr, Cb, Y0-Y3. Since the picture
  never exists in the device's output, the 24-bit byte order and the guest's
  convert cannot be what makes the frame wrong: no permutation, stride or offset
  of that output reproduces the reference frame, and the VRAM dump matches
  neither the reference (best channel correlation 0.32 at 700 fields, 0.25 at
  950) nor the device's own output (0.12 at best).
- **What the player waits on after a decoded frame (measured).** Two
  handshakes sit in the frame loop, one before the next decode and one before
  each output burst, and the port already satisfies both. After a frame's
  command and DMA0 burst (`0x1F801820 = 0x32000FC0`: opcode 1, depth 2, 4,032
  parameter words; DMA0 `MADR = 0x800FDAC8`, `BCR = 0x007E0020` = 126 x 32
  words, `CHCR = 0x01000201`) the next decode step at `0x80085CE8` calls
  `0x80085DF0`, which reads `*(u32*)0x1F801824` and spins while bit 29
  (command busy) is set, up to `0x100000` iterations, then reports through
  `0x8009606C`. Before each output burst `0x80085D78` calls `0x80085E84`,
  which reads `*(u32*)0x1F801098` and spins while bit 24 (DMA start/busy) is
  set, then reports through `0x8009607C`; its caller at `0x80085D80` then
  starts the burst (`BCR = 0x00480020` = 72 x 32 words,
  `CHCR = 0x01000200`). A register probe over 940 fields reads back `0x8484FFFF` and
  `0x00000200` on the **first** poll of each loop, so neither loop turns and
  neither timeout message is printed. What the run does do is start the
  MDEC-out channel at least **8,192** times, while the 8,663,040 decoded
  words amount to only 3,760 bursts of 2,304 words, so a large share of the
  starts cannot have found a full burst ready: those find the decoder's
  output FIFO empty and, because `run_dma1_words` always writes
  the full requested count while `MdecDevice::read_word` returns 0 once the
  FIFO is empty, they write zeros over the target buffer and clear the
  channel at once, instead of leaving it busy until the requested words
  exist. No
  post-decode handshake is what keeps the picture wrong. Once they pass, the
  frame is the guest's own work: the hot blocks are its converter
  (`0x8002A478`/`0x8002A444`, 4,480,332 and 4,480,331 executions) and its
  24x240 strip uploader (`0x80084DC8`/`0x80084D80`, 3,757,728 each), with the
  movie poll `0x8002B430` and the ring claim `0x80086B10` at 1,655,156. The
  addresses above come from the current `translation.cpp`; the linear
  disassembly under `work/` predates the disc image replaced on 27 November
  2026 and decodes different instructions at these addresses.
- **First field where the movie is on screen (measured).** A 1000-field run
  with a per-field dump of the display page puts the first non-black display
  field at **503**. Every field up to 492 is a black page, 492-502 are also
  black, and 503 is the first field whose page carries the movie: `--frames
  503` ends with 32,936,477 blocks, 17 GP0 primitives, 24,103 VRAM pixels,
  2,160 sectors (last LBA 280,030), 720 macroblocks (2.4 frames of 300) and
  8,397 non-black display pixels with a mean channel value of 1.6 - the
  stream's opening fade, two frames into `LOGO.STR`. The earlier 700-field
  capture is therefore the 69th decoded frame, not the first: it reads 3,000
  sectors (last 280,870) and 20,640 macroblocks, and its display page has
  content in rows 104-183 only (19,297 non-black pixels out of 76,800).
  Comparing dumped display pages against `str-frame` decodes of the same
  stream puts the best match on the reference's black opening frames (mean
  absolute error 0.5 at field 503 rising to 4.6 by field 520), so the page is
  carrying the movie's own picture at this point, not boot content.
- **What the decoder is actually fed (measured).** The DMA0 source at
  `0x800FDAC8` in the 700- and 950-field RAM snapshots holds 16-bit run-level
  words, not an MPEG-1 bitstream: a DCT header word (quantiser scale in bits
  15-10, signed DC in bits 9-0) followed by run/level words and a `0xFE00`
  end-of-block, e.g. `0400`/`0600` pairs for the black opening frames and
  `0424`/`07F0`/`063C` for the fade at 950 fields. That is the format
  psx-spx documents under *MDEC Data Format*, and `MdecDevice::decode_block`
  follows its `rl_decode_block` (quantiser from the header word, `FE00` as
  padding or end of block, zig-zag assignment, `(level*qt[k]*qscale+4)/8`).
  The MPEG-1/Huffman layer above it is decoded in software - by the game's own
  routine at `0x8002A3A8` (which reads five-word windows and packs run/level
  words) and by `tools/str-frame.cpp` - so an MPEG-1 bit reader inside
  `MdecDevice` would misparse the words the player sends it. The picture fault
  is therefore not the block reader's format. What is still open is the
  placement: each strip upload is 2,880 words (11,520 bytes, 5,760 16-bit
  pixels - 15-bit sizing for a 24x240 rect), 20 strips make 230,400 bytes, the
  size of one 320x240 24-bit frame, and the page is read as 24-bit because
  GP1(08) carries bit 4 - yet at 700 fields the frame only fills display rows
  104-183.

- **Can the content movie at `286,228` reach the screen?** `286,228` is inside
  `/CINEMAS/L1M1.STR` (LBA 282,216), and the reference decoder shows real content
  there (`241 of 300 macroblock(s)`, ~63,000 lit pixels). The decoder is no
  longer idle - it decodes 174 frames and the drive reaches the first sector of
  `L1M1.STR` - but the run leaves the movie for the engine module before that
  frame is read, and the frames that do reach VRAM are the corrupted picture
  described above, not a reference frame. The conversion the guest performs on
  the decoded output and the dynamic-code halt stand between here and a visible
  `L1M1` frame.
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
- **Why the scene never activates:** `0x8008F398` (sets `0x800AFDEC`,
  installs the 1602 payload into `0x800C4850`/`0x800BC728`, starts the
  scene) has one caller, `0x80034CD4`, and that call is gated on
  `*(0x800BC258) == 0x20`. `0x800BC258` is set to `0x20` only at
  `0x8003ABD4`, inside the sound scene-start function `0x8003AB28`. Neither
  that function nor its dispatcher `0x8003AA4C` executes at all in 400M
  steps (`--watch-pc` reports 0 for both), so the gate can never open. The
  sound module is registered in the function table at `0x800A9798`
  (`0x8003AB28`) and `0x800A97AC` (`0x8003AA4C`), and whatever should call
  through that table does not.
- **The 400M stall, found:** the frame body `0x800C9D6C` runs 254 times,
  then the guest spins at `0x80093550` (8,689,077 executions).
  `0x800934EC` is a non-blocking poll on the 32-byte stream pool at
  `*(0x800C49B8)` = `0x801A8040`, indexed by `*(0x800C49A4)` = 0; every
  entry is in **state 3**, flags `0x8001`. The poll only advances on state
  1/2, so a 2,000-retry loop in the RAM trampoline at `0x801E331C` (return
  address `0x801E3340`) always fails; the caller repeats it and the boot
  machine never gets another frame. This is the same "stream left in state
  3" defect recorded earlier, now tied directly to the frozen frame loop.
  `R6_TRACE_LOOP=1` prints the retry caller's return address.
- **The sound-voice hypothesis was wrong:** the completion stores in the
  sound tick (`0x80033898`, `0x800338B0`, `0x80033A80`, `0x80033A94`,
  `0x80033AD8`) are gated on the 15 voice records at `0x800B5508` being
  free, but at 400M every one of those records is already empty (`+4` = 0).
  Voices never held the stream. The stream records at `0x801A8040` stay in
  state 3 because nothing in the streaming module (`0x80093xxx`) moves them
  to state 2, the only state the poll `0x800934EC` accepts; every other
  state returns 1, so the 2,000-retry loop at `0x801E331C` gives up and the
  frame loop never runs again.
- **SPU voice model:** the SPU now has 512 KiB of sound RAM filled by DMA4,
  the 24-voice register file, key on/off, ENDX (cleared by key-on, set at a
  LOOP-END block), the current-volume register, and IRQ9 at the IRQ address
  once SPUCNT bits 15 and 6 enable it. At 400M it is exercised: 745 register
  writes, 512 DMA words, 48 key-ons, 48 key-offs, 0 voice ends, 0 IRQs, and
  48 of 48 walks found an end flag (first start `0x1000`, 28 samples). The
  driver keys each voice off long before it would end and never polls ENDX,
  so the model changes none of the boot state; the stream stall is upstream
  of the sound module.
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

What the window shows is therefore black: three black clears and a set of
triangles clipped to one pixel produce no visible content, and no loading
screen is reached either — the guest never submits one. The pipeline that would
deliver level data is waiting for a read that is never requested, so the honest
description is "boots and stalls", not "shows a loading screen". Changing the drive's `GetStat` "reading" bit to mean "a transfer is
in flight" was tried and left the trace byte-identical, so it was reverted
rather than kept as an unproven hardware change.

## Where the 128-sector stream stops

With the drain trace enabled (`--cd-log`, every sector now logs `drained N` as
the FIFO empties) the stream is unambiguous. Sectors 128214-128341 arrive in the
normal order: `sector N loaded`, `ack 7 int=1`, `drained N`, next sector. The
last one breaks the pattern:

```
459 sector 128341 loaded
460 ack 7 int=1
    (no drained 128341, no sector 128342)
```

So the guest acknowledges the last sector interrupt and never performs the DMA
that would empty the FIFO, which is exactly its chunk boundary: 128 sectors is
256 KiB. The loader is not starved and the drive is not slow; the guest stops
collecting on purpose.

What it then needs never happens:

- the stage entries 0-35 are populated (entry 33: `80010000 00090003 00000004
  00000A98 00F00140 38000720 00020001 02133328`), while entry 36 onwards is
  still allocator fill (`33333333`) because nothing wrote it;
- the current stage index is 36, so the consumer polls entry 36, which never
  reaches state 1 or 2;
- the CD driver's state word at `0x800B1000` is 10, written once at cycle
  5,128,886 by `0x800856D0`, the "start a chunk" path;
- the chunk descriptor at `0x800BF0C8` (note: `lw v0,-3896(v0)` with
  `v0=0x800C0000` addresses `0x800BF0C8`, not `0x800C0F38`) is `0x80148F54`,
  which is `0x80148AD4 + 32*36` — the stage entry the loader has not filled. Its
  contents are still the allocator fill pattern, so the entry sits at state 0.

The chunk-complete path (`0x800850C8`) is what sets that descriptor, from
`[0x800C1510] + 32*[0x800C1514]`, and it then takes the `state == 0` branch at
`0x80085134`. That branch is the next-chunk setup: it writes the chunk length
**128** (`0x8008516C`) and the DMA mode word `0x00020943` (`0x80085180`). So the
code that would request the next 128-sector transfer exists and is on the path
the run reaches; whether it is entered at the stall is the next thing to
measure, with `--watch-change 800B0FC8` (the DMA mode word) as the marker.

Also tried and reverted: treating an acknowledged-but-undrained sector as the
end of the read. The guest acks before draining on every sector (sector 16 shows
it immediately), so that rule stopped the boot after one sector.

## Stage entry layout

The entries are 32 bytes each and are written by the guest itself: each sector
arrives as an 8-word DMA (the 32-byte header, into the entry) followed by a
504-word DMA (the 2016-byte payload, into a data buffer). The layout, from the
code that reads and writes it:

| offset | field | evidence |
| --- | --- | --- |
| `0x00` | `u16` state: 0 empty, 1 ready, 2 released, 4 consumed | compared against 1 and 2 at `0x80086B2C`/`0x80086B6C`, set to 4 at `0x80086B80`, cleared at `0x80086B50` |
| `0x02` | `u16` constant `0x8001` | every populated entry |
| `0x04` | `u16` slot inside the group, wrapping at the group size | counts 0-9 then wraps |
| `0x06` | `u16` entries in the group | `lhu 6(...)` at `0x800872FC` |
| `0x08` | `u32` group number, 1-based | increments every 10 entries |
| `0x0C` | `u32` constant `0x00000A98` | every populated entry |
| `0x10` | `u32` constant `0x00F00140` | every populated entry |
| `0x14` | `u32` constant `0x38000720` | every populated entry |
| `0x18` | `u32` constant `0x00020001` | every populated entry |
| `0x1C` | `u32` group/slot tag | increases with the group |

An allocated but unfilled entry starts with a zero halfword and keeps the
allocator fill from `+4`, which is how `work/diag/dsstate.py` tells unwritten
entries from consumed ones. The earlier reading of `0x80010000` as a data
pointer was wrong: that word is the state halfword followed by the constant.

## CD stream: DMA3 completion order

The stream's producer is the tick at `0x800935CC`. It reads each sector as an
8-word header DMA followed by a 504-word payload DMA, both on channel 3, then
writes state 3 and increments the produced count at `0x800C499C`. The publish
callback `0x8009327C` is registered with `DMACallback(3, ...)`; it
unconditionally stores state 2 at `record[commit]`, sets
`commit = produced` (`0x800C49A0`), clears the group-end byte `0x800C4994` and
calls the record's reader hook. There is no `commit < produced` guard in it.

Two emulation errors kept the records in state 3:

- `run_dma3_transfer` only completed the channel when the old streaming slot
  `0x800B4394` was set. The CD stream instead enables channel 3 through
  `DMACallback`, which sets DICR bits 23 and 19 (`0x00880000`), so the callback
  never ran. The completion now also requires those DICR enables.
- Both transfers use the same CHCR value, but only the 504-word payload may
  raise the publish interrupt. The 8-word header completes before the tick has
  checked the block counter and written state 3, so a header callback saw
  `commit == produced`, published the next entry, and the player consumed a
  record whose data had not been read. The tick then rejected the following
  header (`group 3, block 1` against expected counter 9) and production stopped
  at 18 records while the consumer index ran to 27 and read past the 23-entry
  pool. Channel 3 now schedules its completion after the CHCR store and only
  the payload-sized transfer raises the DMA interrupt.

The completion is delivered at the instruction where it happens rather than at
the 1024-instruction interrupt poll, and its 16-cycle delay places it inside
the tick that started it: the callback is then taken in the same post-state-3
window, with `produced == commit + 1`, and publishes exactly the new record.

With both fixes a 400M-instruction run (`R6_FORCE_SOUND_TICK=1`) executes 35
state-3 writes instead of 18. Groups 1 and 2 complete, a new segment starts at
group 3, all nine group-3 records are consumed and eight of group 4 are
published, and the final counters are `produced = commit = 17`,
`consume = 9` in the new segment. The scene gate `0x800BC258` is still 0 and
the frame body count is still 254. This removes the first CD-stream deadlock,
but the new segment then stalls at record 17 (next section), and the scene is
still closed; the sound module that stores `0x20` there remains the blocker.

## CD stream: record 17 and the null libcd table

A 600M-instruction run with `--trace-cd`, `--watch-write` on
`0x800C4980`-`0x800C498C`, `0x800C49A8`-`0x800C49B0`, `0x800C499C` and
`0x800BFF3C`-`0x800BFF84`, and `--watch-pc` on `0x80093604`/`0x80093650`
shows where the new segment's producer stops. The same trace contains 1624
channel-3 DMA transfers and 92 CD commands, so the null table cannot be the
whole explanation.

### What the run shows

- The ring is not full: `produced`, `commit` and `consume` are all 17, the
  capacity at `0x800C49BC` is 23, and slot 17's header sits at `0x801A8260`.
- The last read the drive receives is `ReadS` (command `1B`) at LBA 133147,
  issued after `Setmode E0`; it delivers sectors up to 133167. Sector 133166
  is an XA payload the drive skips while decoding, so 133167 is the last one
  presented. It is loaded and acknowledged (`sector 133167 loaded`, `ack 7
  int=1`) but never drained, and the log has no further `Setloc`/`ReadS`. No
  read for group 4 block 8 or group 5 block 0 is requested in the run.
- On the DMA side the last pair is the header and 504-word payload for sector
  133166 (payload into `0x801B0120`, the last ring slot). After it there is a
  single 8-word header transfer into `0x801A8260`, slot 17's header, and the
  504-word payload that should follow it is never started.
- The pump tick `0x800935CC` reaches the status gate at
  `0x800935F8`-`0x80093610` 37 times after that point. The gate dereferences
  `[0x800BFF5C]` and tests bit `0x0100`; the pointer is zero for the whole
  run, so the test reads address 0 and always takes the `0x80093650` branch.
- In the 600M dump `0x800BFF3C`-`0x800BFF6C` is zero, and the watch log
  records only the boot BSS clear at `0x80010B0C` writing `0x800BFF5C`. The
  setter `0x8009204C` and the init entry `0x80092068` never execute, and no
  `jal`, jump-table word or data word in the loaded image names them.

### Why the CD traffic still works

The apparent contradiction is resolved by a second, fully initialised copy of
the same driver in the main EXE at `0x80015xxx`. Its table at
`0x8002297C`-`0x800229C0` holds `0x1F801800`-`0x1F801803`, the CD-delay and
COM_DELAY registers at `0x1F801018`/`0x1F801020`, DPCR `0x1F8010F0` and DMA3
`0x1F8010B0`-`0x1F8010B8`. Its command byte store is `0x800153A0` through
`[0x80022980]`, the main-driver poll sites appear in the log
(`0x800147D4`, `0x80015084`, `0x80015DFC`), and its CHCR store `0x80015C88`
is the PC on 1552 of the 1624 DMA3 runs. The commands are issued by this copy,
and it is the only copy whose status and interrupt registers are valid.

The overlay's own DMA needs no table. Its transfer routine at `0x80093F14`
reads CHCR at `0x80093F4C` and writes MADR3/BCR3/CHCR3 through directly
encoded `0x1F801080`-`0x1F801088` addresses (`0x80094018`-`0x8009408C`). Its
72 DMA3 runs are the header/payload pairs ending at sector 133166 plus the
final 8-word header. The null table affects its status and interrupt
registers, not those transfers.

The null table does explain the overlay pump's status read: the gate cannot
see a response, so the tick takes the `0x80093650` path and returns through
its `0x80014120` call without reaching the command setup at `0x80093700`.
That setup is dormant; had it run, its stores through the null pointers
would have left nonzero data at RAM address 0, and the dump is zero there.
The command, parameter and DICR/DPCR stores in this copy are therefore not
the traffic that reached the drive.

### What is still hypothesis

The resident image is loaded in one encrypted block at about 40.7 million
cycles. `0x80010AD0(dst=0x80024B60, src=0x8010A600, count=0x000A4B00)`
decrypts 673,536 bytes from the CD buffer into `0x80024B60`-`0x800C9660`,
which covers the overlay at `0x80090000` and the table at `0x800AFE80`.
The watch log shows 18 stores into `0x800BFF3C`-`0x800BFF84`, all from the
decrypt loop at `0x80010B0C`; the stored values are zero. `0x800BFF3C` is
BSS in the source image, so the loader does not skip a populated table: it
zero-fills the slot once and no later writer exists.

The earlier copy hypothesis is not supported. No instruction computes
`0x800AFE80`; the only reads of that data are `[0x800AFE90]` at
`0x800922A4` and `[0x800AFE94]` at `0x8009230C`. The table is a static
descriptor with signature `0x0043529B`, not a `0x0043539B` module table,
and it is used directly, not copied to `0x800BFF3C`.

The module's own init candidates are dead. `0x80092020`, `0x80092068` and
`0x8009204C` have no callers and no data or jump-table pointers.
`0x80092068` initializes BIOS kernel callbacks, registers the pad hooks,
and sets `0x800AFE8C`, but never touches `0x800AFE80` or
`0x800BFF3C`-`0x800BFF6C`. Calling it would not populate the table, so it
cannot be the copy init. The pump is instead reached through the callback
slot `0x800226BC`, which calls `0x800935CC`; the game also calls
`0x800935CC` directly from `0x801E3098`. That slot is patched by the CD
stream-start path, not by an init: at 60M instructions it still holds the
boot value `0x80013F60`, and by 300M it holds `0x80093170`. The store is at
`0x80014164` in setter `0x80014154`, called from `0x80093148` inside the
status handler `0x800930EC`. The handler is entered from `0x801E3268` in
the stream-start helper `0x801E31CC` with status `0x1E0`; it registers
`0x8009327C` at `0x800225F0`, sets `0x800231E8`, and issues CD command 27
(`ReadS`).

This path does not build the register table. In the 300M run the only write
to `0x800BFF3C` is still the loader's zero-fill at `0x80010B0C`; the table
stays zero after the pump is installed. The CD driver init at `0x8009C708`
does run and fills its own object at `0x800C7230`, but it never touches
`0x800BFF3C`. The stream module's init entry `0x80092068` is unreferenced
and also does not write the table. No later instruction writes
`0x800BFF3C`; the only accesses are the pump's reads at `0x800935F8`,
`0x80093704`-`0x8009377C`, and `0x80094054`.

The builder should run before `0x800930EC` installs `0x80093170` into
`0x800226BC` (before `0x80093148`), or in the pump's first-use init around
`0x800936F4`, before the command setup at `0x80093700`. It should fill
`0x800BFF3C`, `0x800BFF44`-`0x800BFF50`, and `0x800BFF5C` from the hardware
descriptor fields at `0x800AFE80` (`0x1F801040`, `0x1F801070`, and
`0x1F801800`-`0x1F801803`). No such routine exists in the loaded image:
the dead init `0x80092068` is not it, and the loader applies no fixup.
This is an absent initialization, not a skipped call to an existing
routine.

The observed fact is that the status gate cannot see a response and the
504-word payload after the slot-17 header never starts. Whether populating
the overlay table would restart production is untested.
### The 2026-11-27 bring-up probes

Two probes were added so the findings above can be retested without editing
the binary. `--poke-at 0xADDR` defers the `--poke` list until the guest
first reaches that PC, because the 673,536-byte pack decrypt at `0x80010AD0`
runs at about 40.7M cycles and zeroes every BSS slot a load-time poke had
touched. The switch is off unless asked for and changes nothing when unused.

Populating what the pump expects is not enough. With the descriptor copied
into the slot the pump reads (`0x8009336C`, 18 words from `0x800AFF3C`,
poked before first use) production still stopped at 17 against 9 consumed
and the frame body never ran. Pointing the pump's status word
(`0x800BFF5C`) at a scratch word holding `0x00000100` let the pump run 19
times and production reached 18/18/18, yet `0x8008F398`, `0x8003AB28` and
`0x8003AA4C` were still never entered and `0x800BC258` stayed zero. The
table is needed for the pump to see its own status but it is not the gate
the frame loop waits on.

### Sector framing is not the stall

`CdromDevice::load_sector` presents a sector as 12 header bytes followed by
the 2048 user bytes. A consumer that stops short of 2060 bytes does not park
the drive any more: the next sector is armed when one is presented, and an
acknowledged sector left unread for a whole sector time is overwritten (see
*Streaming ahead of the guest's drain*). The stream pump itself does not
stop short either: it reads the header through the data port (four bytes at
`0x80093778`, eight at `0x800937A0`), then DMAs 8 words and 504 words, which
is 2060 bytes exactly. The payload half of the sector is issued from the
sound module (`0x8004DD08`, `0x8004DD58`, `0x8004DDE8` call the payload
routine at `0x80093D00`), so the pump stops after a header only when the
sound module stops calling it - which is the frozen retry loop at
`0x801E331C`, not a drive accounting bug.

### What the guest draws

`R6_TRACE_PRIM` shows the frame body's whole submission: a texture page
select (`GP0 0xE1`), three `0x64` textured rectangles with modulation
`0x808080` and sizes `0x0100`/`0x0080`, and a black `0x02` fill of the
320x224 page, repeated every frame. The rectangles sample the 8-bit page at
`x=960` with the CLUT word carried in the packet (`0x7830`), which points
at VRAM the game never fills: its atlases land at `(768,0)`, `(896,0)` and
`(704,256)`, and its palette at `(0,496)`. Every sampled texel is zero, and
PS1 texture blending treats texel zero as transparent, so the page is left
untouched. The black frame is the clear, not a rasteriser fault.

The open defect is therefore the sound phase machine: the frame body draws
a black clear because the scene that would give it colour never starts.

### The main-state table and the sound scene start

`0x800A977C` is the main-state dispatch table. The frame driver loads
`0x800BCCBC` (the state word, offset `+0x594` of `0x800BC728`) at
`0x80033070`, indexes that table at `0x80033078` and calls the entry at
`0x80033088`, once per driver iteration. Its entries are:

| state | handler |
|---|---|
| 0 | `0x80032D1C` |
| 1 | `0x800C95AC` |
| 2 | `0x800C99A4` (the boot state machine) |
| 6 | `0x801E279C` (cinema player) |
| 7 | `0x8003AB28` (sound scene start) |
| 10 | `0x800348E8` |
| 11 | `0x80038BD4` |
| 12 | `0x8003AA4C` |

So `0x8003AB28` is entered whenever the main state is exactly 7, and only
then. The stores to `+0x594` that can produce 7 are `0x800343C0` (inside the
scene-start routine `0x80034264`), `0x801E27DC` (the tail of the cinema
handler), `0x8003ADF8`, `0x8003AF58` and `0x8003B188` (sound sub-states) and
`0x801E2A48`. The driver reaches `0x80034264` only when `0x80034108`
returns 2 (`beq` at `0x80032FA8` / `0x800330AC`, call at `0x800330B4`).

`0x80034108` returns 2 in three cases: `*(0x800BC264) & 2` is set and the
frame counter `0x800A9768` has passed 1801; the pad block reaches
`0x0F09`/`0x9008`; or `*(0x800BC264) & 0x100` is set. Earlier notes gave
this word as `0x800CC264`; the displacement `-15772` from `0x800C0000`
makes it `0x800BC264` (and the scene gate `0x800BC258`), 64 KiB lower.

Nothing ever writes those words. A 420M-step run with
`--watch-write 0x800BC264 --watch-write 0x800BC258` records exactly one
store to each: the BSS clear at `0x80010B0C`. Both stay zero, so
`0x80034108` answers 0 on all 124 calls and `0x800330B4`, `0x80034264`
and `0x800343C0` never execute.

### Where the cinema path stops

The one path that can set state 7 on its own is the state-6 handler
`0x801E279C`. It calls `0x801E2AA4` with file id 2054, which is the cinema
player: it points a 320x240 page at `0x801A2600` (`0x801E2BC8`), opens the
stream, and then reads one frame per iteration through the 2000x2000 retry
loop at `0x801E3298`/`0x801E331C` on the 32-byte ring at `0x801A8040`.
Only after that call returns does `0x801E27DC` set state 7. In a 420M-step
run the ring is healthy but slow: entries 9-16 are ready, 35 frames have
been published, and the pump `0x80093170`/`0x800935CC` has run 37 times,
about one chunk per 11M cycles, so the player never reaches its end.

Seeding the guest's own flag proves the rest of the chain. With
`R6_FORCE_SOUND_TICK=1 --poke 0x800BC264=0x100 --poke-at 0x80032EE0`
(the poke has to land after the boot clear at `0x80010B0C`), 420M-500M
steps give: `0x80034108` returns 2, the driver calls `0x80034264`,
`0x800343C0` sets the state to 7, `0x8003AB28` runs 1013 times,
`0x8003ABD4` sets `0x800BC258` to `0x20`, and the state stays 7. The scene
install still does not happen: `0x80034CD4` (the only caller of
`0x8008F398`) needs the file-1618 stream to arrive, and the frame is still
**0 of 76,800** non-black pixels.

The remaining defect is disc throughput on this path. Over 420M cycles the
controller's data-ready callback `0x80093100` runs twice and the pump 37
times, while the loader path reads hundreds of sectors in the same budget,
so the cinema stream is starved rather than mis-addressed. That starvation
is what keeps `0x801E27DC`, and with it `0x8003AB28`, unreachable in an
unmodified run.

### Snapshots do not carry the timer mode

A resumed `--state` run spins for ever in the "wait until two reads agree"
loop at `0x80013130`, because the snapshot holds the CPU and RAM but not
the timer mode: live, T1 is `0x148` and that loop exits 6010 times out of
6380 entries, while a resumed map reports mode 0 and every read of
`0x1F801110` differs. Use a fresh run for anything VSync-paced.

### After the modelled-MDEC build

Rebuilt on top of `fc6e9e3`/`e54f65a`, the same two 500M-step runs give the
same outcome. Unmodified: main state 6, `0x800BC258` zero, the pump
`0x80093170` 22 times, 19 frames published, 13,007,512 failed polls of
`0x800934EC`, and 0 of 76,800 non-black pixels. With
`--poke 0x800BC264=0x100 --poke-at 0x80032EE0`: main state 7, the gate
`0x800BC258` = `0x20`, `0x8003AB28` runs 1013 times, but only its sub-state
`0x8003AC88` is entered - the install case `0x80034CA8`, which lives in the
state-10 handler `0x800348E8` and calls `0x8008F398`, never runs, and the
frame is still 0 of 76,800. The modelled MDEC does not by itself finish the
cinema, so the stream still has to be fixed for state 10 to be reachable.

### The cinema pump is a per-read callback

`0x80093170` is not called per frame. The CD interrupt handler `0x80093100`
(installed in the interrupt table at `0x800224B0`) registers it at
`0x800226BC`, libcd's read callback, and libcd's read loop runs it from the
`jalr` at `0x80015E40` once a read reaches DataEnd. The sibling
`0x8009327C` at `0x800225F0` is the channel-3 DMA callback; the helper
`0x80093F14` is the generic DMA-channel routine the pump uses for its
8-word header and 504-word payload transfers. So one pump equals one
finished `CdRead`, which is why 500M steps give 22 pumps and 19 frames.

Disc throughput is not the limit. During the file load the drive hands over
39,360 sectors in 300M cycles (about 7,600 cycles each). In the cutscene the
player abandons its read instead: the CD event tail repeats
Setloc/SeekL 133148, Setmode `0xE0`, ReadS, one sector, GetStat,
Setmode `0x80`, Setloc, SeekL - 3 header DMAs at `0x8009408C` in 400M
cycles, each cleared from the ring entry at `0x801A8040`. Only reads that
run to DataEnd reach the callback, and our controller never raised INT4,
so a read could only end on a timeout.

The starvation itself was the sector-retention bug fixed in `ad26042`: the
drive discarded a sector whose interrupt had been acknowledged while its
bytes were still in the FIFO, so nothing was left to drain and no next
sector was armed. The drive arms its next sector when it presents one now,
not when the guest drains the fifo; the measurements below predate that
change. Rebuilt on top of the retention fix, they were taken with
`--watch-args` printing the callers above. Every retry re-read LBA 133148,
whose user data starts `60 01` (the `0x160` halfword it validates) while the
raw sector starts `00 FF FF .. 29 37 23 02`, so the record it expected and
the sector it was handed had to be reconciled.

### The record check passes; what stopped the stream at record 19

These measurements were taken before the drive kept streaming past an
uncollected sector: the record check they describe still holds, the stall
they report does not.

Watching the ring entry the cutscene player fills shows it is not the
`0x160` check that stops it. The 8-word header DMA (`0x8009408C`) writes
`0x80010160` into the entry, which is the sector's user data starting
`60 01 01 80`, so `lhu 0(entry) == 0x160` holds and the record is accepted
(`0x80093A10` clears the slot on the accept path).

With the drive still waiting for each drain, it streamed header and payload
back to back: 18 consecutive 8-word/504-word pairs between cycle 399696120
and 399809128, one every 6,100 cycles, which is about twice disc speed.
The sectors handed over are 133127 to 133148 with 133134 and 133142 skipped
as XA audio, so LBA 133148 itself is read and consumed.

What was left was the chunk boundary after those 18 frames. The next
header DMA wrote `0x50050400` and then `0x00504004` - three words into the
stream, not at a sector start - and was rejected at `0x80093990`. Every
184M cycles the player re-seeked to 133148 and retried until it got a clean
`0x80010160` again, so the ring advanced a frame at a time between long
stalls instead of streaming. Dropping the 12-byte sync remnant from
the FIFO when a driver has taken the user bytes makes the boot fail after
550 BIOS calls, so those bytes are needed and the misalignment has another
origin.

### The FIFO offset at each stream transfer

`R6_TRACE_STREAM` now prints the command, presented sector and FIFO offset
both before and after a transfer. The good frames read `sec=133144` to
`133146` with the offset at 12 before the 32-byte record header and 44
before the 2016-byte payload, so a sector is consumed exactly as
12 sync + 32 header + 2016 payload = 2060 bytes. The payloads were never
reading zeros; the earlier offsets were printed after the transfer.

At the boundary the attempt is still on `sec=133147` with offset 12, yet
the word it collects is `0x50050400`. That pattern is not the start of any
sector on the disc: it only occurs at user offset 32 of later sectors
(133199, 133569, ...), which is a payload start rather than a record start.
The drive was therefore presenting another sector over data the guest had
not collected, which is now the modelled behaviour (*Streaming ahead of the
guest's drain*), and every attempt after the eighteenth frame landed in the
middle of a record.

A guard that refused to replace a FIFO whose user bytes are still unread
was tried and dropped: the replace arrives through a new read command,
which clears the FIFO before the guard can see it, so it never fired and
changed nothing measurable. At that point the stream still stopped after 18
frames; with the sector timer armed at presentation it does not.

### The FIFO around every command

`R6_TRACE_CD_FIFO` prints the FIFO offset and size before and after
`execute_command`, and `present_sector`, together with the command, the
sector and whether a read is in progress. A read also prints the word the
next `read_word()` returns, so the bytes the guest is about to collect are
visible without a debugger.

At the frame-19 boundary the log reads:

```
present-out cmd=1B sec=133148 off=0    size=2060
SDp ... words=8 cmd=1B sec=133147 off=12 size=2060 w0=80010160
cmd-in  cmd=01 sec=133148 off=44 size=2060
```

The drive hands over sector 133147 at offset 12 with `0x80010160`, the same
word as every accepted record. Nothing is swapped underneath the guest at
that point, so the rejection is not an alignment fault in the drive.

What the log does show is the retry path: `GetStat` and `Setmode` keep the
sector by design, so the next header read lands at offset 56, 88 or 100 and
collects payload bytes (`0x218040A0`, `0x50050400`). Rewinding the FIFO on
a keeping command changed those offsets but not the outcome at the time -
18 payloads and a black frame either way - so it was dropped rather than
kept as an unmeasured model change.

### What decides a record is not usable yet

`R6_TRACE_PC_WINDOW=start:end` records every PC the guest executes inside a
cycle window. With `R6_FORCE_SOUND_TICK=1` and enough steps, the window
399,790,000-399,840,000 contains the boundary. That measurement withdraws
the earlier claim that `0x8009391C` never executes: it does run, four times
in that window. The zero hit count that suggested otherwise came from a run
without the sound tick, where the movie never reached this code.

The path measured for every record is:

```
0x8009408C  sw $v0, 0($a1)      ; CHCR write that starts the DMA (ch in $s0)
0x80093824  poll DMA, set up the next transfer
0x8009391C  entry[0] == 0x0160 ? then channel == [0x800C4990] ?
0x80093994  entry[2], entry[4] tests
0x80093A50  lhu  $v0, 4($a0)    ; entry[2]
0x80093A64  bne  $v0, $zero, 0x80093C9C
0x80093B3C  $v0 = [0x800C49BC] - [0x800C499C] - 1
0x80093B58  $v1 = lhu  6($a0)   ; entry[3] = 9 in the movie records
0x80093B60  sltu $v0, $v0, $v1  ; queued ahead < entry[3] ?
0x80093B64  beq  $v0, $zero, 0x80093C88
0x80093B80  sh   $v0(1), 0($a0) ; mark the entry
0x80093BD4  [0x800BFF84] = 8    ; status 8, no payload transfer
```

So the first halfword (`0x0160`) and the channel both pass for record
133147, as the earlier trace showed, and the branch that stops it is the
count at `0x80093B3C`: the player compares how many records are queued
ahead (`[0x800C49BC] - [0x800C499C] - 1`) against the record's own
`entry[3]` field (9 for these sectors). When the queue is shorter than
that, it marks the record, sets `[0x800C49B0]` and returns status 8
without starting the payload DMA.

The records are not rejected for their contents. The player wants nine
records of read-ahead, and at the time of that measurement the drive model
presented one sector at a time only after the guest drained the fifo, so the
queue could not grow past a couple of records and the eighteen that did play
were the ones consumed before the count first fell short.

### Streaming ahead of the guest's drain

The drive no longer waits for the guest to empty the data fifo before the
next sector arrives. `CdromDevice::present_sector` arms the following
sector's timer as soon as it has presented one, and `advance_cycles`
presents it when the fifo is empty or when the guest acknowledged the
data-ready interrupt and has left the sector uncollected for a whole sector
time. The write is a real overwrite of unread bytes, which is what a drive
does; a first sector, or one the guest has not acknowledged, is still never
overwritten.

Measured with `R6_FORCE_SOUND_TICK=1 R6_TRACE_STREAM=1 R6_TRACE_CD_FIFO=1`
and `--steps 500000000 --vblank`, the record count no longer stops at 18:

```
payload DMA3 transfers after cycle 400,000,000:   0 before, 116 after
present-out ... sec=133147 ...                     the boundary sector
present-out ... sec=133148 off=44 ...              presented with only the
                                                   header of 133147 collected
SDp ... sec=133154 ...                             headers keep being read
Sector index reached:                              ~175112, then restarted
                                                   at ~143549
```

Every one of those 116 payload transfers starts at fifo offset 44, so the
guest does collect the payload of sectors it accepts; the sectors it defers
are dropped and the stream moves past them instead of holding position.

The picture does not follow. A frame dumped at the moment the burst of
accepts ends (`--steps 250000000 --vblank`, cycle ~500,000,000) is
0 of 76,800 pixels non-black, and the frame at the end of the 500M-step run
(cycle ~999,800,000) is 0 of 76,800 as well. That run drew 91,379 filled
VRAM pixels with the display enabled, so the black display is not the drive
withholding sectors any more: the collected sectors reach the player, and
what is still missing is between the player's record buffer and the
displayed framebuffer.
