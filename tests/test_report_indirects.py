import json
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


class IndirectReportTests(unittest.TestCase):
    def test_reports_indirect_instruction(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "sample.exe"
            data = bytearray(2048 + 8)
            data[:8] = b"PS-X EXE"
            struct.pack_into("<I", data, 0x10, 0x80010000)
            struct.pack_into("<I", data, 0x18, 0x80010000)
            struct.pack_into("<I", data, 0x1C, 8)
            struct.pack_into("<I", data, 2048, 0x01200008)  # jr $t1
            path.write_bytes(data)

            result = subprocess.run(
                [
                    sys.executable,
                    str(Path(__file__).parents[1] / "tools" / "report-indirects.py"),
                    str(path),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            report = json.loads(result.stdout)
            self.assertEqual(report["potential_indirect_site_count"], 1)
            self.assertEqual(report["reported_sites"][0]["status"],
                             "requires_contextual_resolution")
            self.assertEqual(report["reachable_unresolved_site_count"], 1)


if __name__ == "__main__":
    unittest.main()
