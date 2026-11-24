# Game status

## Completed

None.

## In progress

### Spider-Man (USA)

- **Platform:** PlayStation 1
- **Region:** USA
- **Status:** Windows I/O backend
- **Progress:** 85%
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
- **Next step:** Implement GPU command decoding and BIOS service behavior with
  tests.
- **Playable native build:** No
- **Distributed game assets:** No

Progress percentages are intentionally conservative and will only change when
there is a reproducible milestone.
