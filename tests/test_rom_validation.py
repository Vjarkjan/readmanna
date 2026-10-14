"""The GUI must accept the DSi-exclusive ROM built by our Makefile."""
from pathlib import Path
import tempfile
import unittest

from main_gui import NDS_HEADER_SIZE, NDS_UNIT_CODE_OFFSET, validate_built_rom


class RomValidationTests(unittest.TestCase):
    def validate_unit_code(self, unit_code):
        with tempfile.TemporaryDirectory() as directory:
            rom = Path(directory) / 'reader.nds'
            header = bytearray(NDS_HEADER_SIZE)
            header[NDS_UNIT_CODE_OFFSET] = unit_code
            rom.write_bytes(header)
            validate_built_rom(rom)

    def test_accepts_enhanced_and_exclusive_dsi_headers(self):
        for code in (0x02, 0x03):
            with self.subTest(unit_code=code):
                self.validate_unit_code(code)

    def test_rejects_ds_only_and_invalid_headers(self):
        for code in (0x00, 0x01, 0x04, 0xFF):
            with self.subTest(unit_code=code):
                with self.assertRaisesRegex(RuntimeError, 'not marked'):
                    self.validate_unit_code(code)


if __name__ == '__main__':
    unittest.main()
