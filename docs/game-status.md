# Game status

## Completed

None.

## In progress

### Spider-Man (USA)

- **Platform:** PlayStation 1
- **Region:** USA
- **Status:** Native control flow
- **Progress:** 35%
- **Completed:** Local BIN/CUE verification, ISO9660 root discovery, and PS-X
  EXE header identification, plus a delay-slot-aware control-flow walk from
  the entry point and discovered direct calls, plus the first dependency-free
  R3000A instruction decoder and synthetic tests, plus a local-only generator
  that emits a small C++ translation prototype, plus native little-endian
  memory helpers for common byte/halfword/word loads and stores, plus
  delay-slot-aware branch/jump/call lowering and a platform service interface.
- **Next step:** Add generated function dispatch and PlayStation hardware
  service implementations.
- **Playable native build:** No
- **Distributed game assets:** No

Progress percentages are intentionally conservative and will only change when
there is a reproducible milestone.
