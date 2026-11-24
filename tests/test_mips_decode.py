import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parents[1] / "tools"))
from mips_decode import decode_word


class MipsDecodeTests(unittest.TestCase):
    def test_nop(self):
        decoded = decode_word(0x00000000, 0x80010000)
        self.assertEqual(decoded["text"], "nop")
        self.assertFalse(decoded["control"])

    def test_jr_return_has_delay_slot(self):
        decoded = decode_word(0x03E00008, 0x80010000)
        self.assertEqual(decoded["kind"], "return")
        self.assertTrue(decoded["delay_slot"])

    def test_jal_target(self):
        pc = 0x80010000
        target = 0x80012000
        encoded_target = (target & 0x0FFFFFFF) >> 2
        decoded = decode_word(0x0C000000 | encoded_target, pc)
        self.assertEqual(decoded["kind"], "call")
        self.assertEqual(decoded["target"], target)

    def test_branch_target(self):
        pc = 0x80010000
        decoded = decode_word(0x10000002, pc)
        self.assertEqual(decoded["target"], 0x8001000C)
        self.assertTrue(decoded["delay_slot"])

    def test_load_format(self):
        decoded = decode_word(0x8D280010, 0x80010000)
        self.assertEqual(decoded["text"], "lw $t0, 16($t1)")


if __name__ == "__main__":
    unittest.main()
