import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parents[1] / "tools"))
from mips_cfg import discover_reachable_functions


def jal(pc: int, target: int) -> int:
    del pc
    return 0x0C000000 | ((target & 0x0FFFFFFF) >> 2)


class MipsCfgTests(unittest.TestCase):
    def test_discovers_direct_call_target(self):
        load = 0x80010000
        payload = bytearray(0x40)
        struct.pack_into("<I", payload, 0x00, jal(load, load + 0x10))
        struct.pack_into("<I", payload, 0x04, 0)  # delay slot
        struct.pack_into("<I", payload, 0x08, 0x03E00008)  # jr $ra
        struct.pack_into("<I", payload, 0x0C, 0)
        struct.pack_into("<I", payload, 0x10, 0x03E00008)
        struct.pack_into("<I", payload, 0x14, 0)

        entries = discover_reachable_functions(
            bytes(payload), load, load, max_functions=8
        )
        self.assertEqual(entries[:2], [load, load + 0x10])

    def test_discovers_constant_indirect_call_target(self):
        load = 0x80010000
        payload = bytearray(0x40)
        struct.pack_into("<I", payload, 0x00, 0x3C088001)  # lui $t0, 0x8001
        struct.pack_into("<I", payload, 0x04, 0x35080020)  # ori $t0, $t0, 0x20
        struct.pack_into("<I", payload, 0x08, 0x0100F809)  # jalr $ra, $t0
        struct.pack_into("<I", payload, 0x0C, 0)
        struct.pack_into("<I", payload, 0x20, 0x03E00008)
        struct.pack_into("<I", payload, 0x24, 0)

        entries = discover_reachable_functions(
            bytes(payload), load, load, max_functions=8
        )
        self.assertEqual(entries[:2], [load, load + 0x20])


if __name__ == "__main__":
    unittest.main()
