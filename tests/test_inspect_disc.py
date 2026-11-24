import importlib.util
import struct
import tempfile
import unittest
from pathlib import Path

RAW = 2352
USER_OFFSET = 24
USER_SIZE = 2048
HEADER_SIZE = 0x800
PAYLOAD_SIZE = 100
FILE_SIZE = HEADER_SIZE + PAYLOAD_SIZE  # fits in two raw sectors

TOOL = Path(__file__).parents[1] / "tools" / "inspect-ps1-disc.py"


def load_tool():
    spec = importlib.util.spec_from_file_location("inspect_ps1_disc", TOOL)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def make_header() -> bytes:
    header = bytearray(HEADER_SIZE)
    header[0:8] = b"PS-X EXE"
    struct.pack_into("<I", header, 0x10, 0x80010000)
    struct.pack_into("<I", header, 0x18, 0x80010000)
    struct.pack_into("<I", header, 0x1C, PAYLOAD_SIZE)
    return bytes(header)


def make_payload() -> bytes:
    return bytes((index * 7 + 3) & 0xFF for index in range(PAYLOAD_SIZE))


def build_image(path: Path) -> None:
    """Write a synthetic Mode2/2352 image holding one two-sector EXE."""
    image = bytearray(RAW * 20)

    def write_sector(sector: int, user: bytes) -> None:
        start = sector * RAW
        # Sync, header and subheader are deliberately non-zero so a contiguous
        # read would splice them into the extracted file.
        image[start : start + 12] = b"\x00\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\x00"
        image[start + 12 : start + 24] = b"\xde\xad\xbe\xef" * 3
        image[start + 24 : start + 24 + USER_SIZE] = user
        image[start + 24 + USER_SIZE : start + RAW] = b"\xcc" * (RAW - 24 - USER_SIZE)

    pvd = bytearray(USER_SIZE)
    pvd[0] = 1
    pvd[1:6] = b"CD001"
    pvd[156] = 34
    struct.pack_into("<I", pvd, 158, 17)
    struct.pack_into("<I", pvd, 166, USER_SIZE)
    write_sector(16, bytes(pvd))

    record = bytearray(34)
    record[0] = 34
    struct.pack_into("<I", record, 2, 18)
    struct.pack_into("<I", record, 10, FILE_SIZE)
    record[25] = 0
    record[32] = 12
    record[33:45] = b"SLUS_008.75"
    directory = bytearray(USER_SIZE)
    directory[0:34] = record
    write_sector(17, bytes(directory))

    header = make_header()
    write_sector(18, header[:USER_SIZE])
    payload = make_payload()
    tail = header[USER_SIZE:] + payload
    write_sector(19, tail + bytes(USER_SIZE - len(tail)))

    path.write_bytes(bytes(image))


class DiscReadTests(unittest.TestCase):
    def test_sector_read_returns_only_user_data(self):
        tool = load_tool()
        with tempfile.TemporaryDirectory() as directory:
            image = Path(directory) / "fixture.bin"
            build_image(image)
            with image.open("rb") as handle:
                user = tool.read_user_sector(handle, 19)
                self.assertEqual(len(user), USER_SIZE)
                self.assertEqual(user[:PAYLOAD_SIZE], make_payload())

    def test_multi_sector_read_joins_user_areas(self):
        tool = load_tool()
        with tempfile.TemporaryDirectory() as directory:
            image = Path(directory) / "fixture.bin"
            build_image(image)
            with image.open("rb") as handle:
                joined = tool.read_user_sectors(handle, 18, 2)
            self.assertEqual(len(joined), USER_SIZE * 2)
            expected = make_header() + make_payload()
            self.assertEqual(joined[: len(expected)], expected)
            self.assertEqual(joined[len(expected) :], bytes(USER_SIZE * 2 - len(expected)))

    def test_executable_extraction_is_exact(self):
        tool = load_tool()
        with tempfile.TemporaryDirectory() as directory:
            image = Path(directory) / "fixture.bin"
            build_image(image)
            with image.open("rb") as handle:
                data = tool.read_executable(
                    handle, {"extent": 18, "size": FILE_SIZE}
                )
            self.assertEqual(len(data), FILE_SIZE)
            self.assertEqual(data, make_header() + make_payload())


if __name__ == "__main__":
    unittest.main()
