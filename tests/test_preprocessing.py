import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from PIL import Image, ImageDraw, ImageOps

import main_gui as pipeline
from update_book_cfg import _validate_asset_header


ROOT = Path(__file__).resolve().parents[1]


class PreprocessingTests(unittest.TestCase):
    def test_default_filter_matches_bicubic_reference(self):
        image = Image.new('RGB', (73, 111), 'white')
        draw = ImageDraw.Draw(image)
        draw.line((0, 0, 72, 110), fill='black', width=3)
        settings = pipeline.PreprocessSettings(enhancement='None')
        self.assertEqual(settings.resampling, 'Bicubic')
        expected = ImageOps.fit(image, (32, 48), method=Image.Resampling.BICUBIC)
        settings = pipeline.PreprocessSettings(enhancement='None', fit_mode='Fill both screens')
        actual = pipeline.render_page_canvas(image, (32, 48), settings)
        self.assertEqual(actual.tobytes(), expected.tobytes())

    @unittest.skipUnless(shutil.which('g++'), 'host C++ compiler unavailable')
    def test_reader_resize_pixels_and_rotations(self):
        with tempfile.TemporaryDirectory() as work:
            binary = Path(work) / 'resize-test'
            subprocess.run([
                'g++', '-std=c++11', '-O2', '-Wall', '-Wextra',
                '-fsanitize=undefined', '-fno-sanitize-recover=all',
                '-I', str(ROOT / 'include'),
                str(ROOT / 'tests/resize_rgb555_test.cpp'), '-o', str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)

    def test_export_keeps_four_assets_and_original_pixel_budget(self):
        grit = shutil.which('grit') or '/opt/devkitpro/tools/bin/grit'
        if not Path(grit).is_file():
            self.skipTest('grit unavailable')
        original_grit = pipeline.GRIT_EXECUTABLE_PATH
        pipeline.GRIT_EXECUTABLE_PATH = grit
        self.addCleanup(setattr, pipeline, 'GRIT_EXECUTABLE_PATH', original_grit)
        dimensions = {'full': (960, 1440), 'preview': (256, 384),
                      'zoom480': (480, 720), 'zoom512': (512, 768)}
        self.assertEqual(pipeline.RAW_PAGE_ASSET_SIZE_BYTES, 4439040)
        with tempfile.TemporaryDirectory() as work:
            source = Path(work) / 'source.png'
            image = Image.new('RGB', (400, 600), 'white')
            draw = ImageDraw.Draw(image)
            draw.text((20, 30), 'Bicubic reading sample', fill='black')
            draw.ellipse((30, 100, 360, 470), outline='navy', width=5)
            image.save(source)
            for storage in pipeline.STORAGE_MODES:
                with self.subTest(storage=storage):
                    folder = Path(work) / storage
                    folder.mkdir()
                    sizes = pipeline.process_image(source, folder, 1,
                        pipeline.PreprocessSettings(storage=storage))
                    assets = sorted(folder.iterdir())
                    self.assertEqual(len(assets), 4)
                    self.assertEqual(sum(sizes), sum(p.stat().st_size for p in assets))
                    for role, (width, height) in dimensions.items():
                        asset = folder / f'page001_{role}.dsm'
                        _validate_asset_header(asset, role)
                        content = asset.read_bytes()
                        compact = content[:8] == pipeline.ASSET_MAGIC_PALETTE8
                        offset = 520 if compact else 8
                        raw = pipeline.decompress_lz10_payload(content[offset:],
                            width * height * (1 if compact else 2))
                        self.assertEqual(len(raw), width * height * (1 if compact else 2))


if __name__ == '__main__':
    unittest.main()
