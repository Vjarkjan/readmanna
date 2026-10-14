# Validation

Checked on Linux on 14 October 2026.

## Bicubic preprocessing and cached resizing

The current reader builds to `build/Readmanna.nds` (305,152 bytes). The build
still emits only the two installed-libnds qualifier warnings described below.

`python3 -m unittest discover -s tests -v` passes five tests, covering:

- Default bicubic pixels against Pillow's bicubic reference.
- Actual grit exports in both palette and RGB555 modes, retaining exactly four
  assets per page and the same 4,439,040-byte uncompressed RGB555 pixel budget.
  Every exported LZ10 stream is decoded and its dimensions checked.
- The reader's C++ resize kernel at all seven view sizes and four rotations,
  compared with an independent floating-point reference, with bounds sentinels
  and undefined-behavior sanitization. Exact-size paths preserve every pixel;
  bilinear results differ by at most one RGB555 channel step from the reference.
- Enhanced/exclusive DSi ROM header validation and rejection of invalid headers.

Python compilation and `git diff --check` pass. Both existing sample books
also pass `update_book_cfg.py build/sd_card --dry-run` without changes.

For a storage comparison, the original two-page `build/reading-demo.pdf` was
rasterized at 200 DPI to temporary PNGs, then exported with GUI defaults in
compact mode, changing only the filter:

| Filter | Page 1 | Page 2 | Total asset bytes |
| --- | ---: | ---: | ---: |
| Lanczos | 321,368 | 321,476 | 642,844 |
| Bicubic | 320,180 | 320,260 | 640,440 |

The bicubic sample is 0.37% smaller. Image count, dimensions, palettes and LZ10
format are unchanged, but a different filter can increase or decrease compressed
bytes on other images. No physical DSi timing or emulator rendering was measured;
the reduction in resize work and SD reads is established from the code paths,
not a claimed frame-rate benchmark.

## Initial repository validation

| Check | Result |
| --- | --- |
| `bash build_nds.sh` | Built `build/Readmanna.nds`, 303,104 bytes |
| ROM header | Unit code 3: DSi-exclusive |
| Python syntax and Bash syntax | Passed |
| `python3 -m unittest discover -s tests -v` | Two regression tests passed, covering six ROM unit-code cases |
| `pdf_importer.py` | Split an original two-page PDF into two image chapters |
| `pdf_to_book.py` | Exported that PDF as a two-page book with all four assets per page |
| Desktop GUI export | Exported two selected chapters and packaged the reader |
| `update_book_cfg.py --dry-run` | Validated the generated book manifests and page assets |

The GUI was run on a separate Xvfb display. The screenshots capture its actual
widgets and export output. Opening the file manager after export was suppressed
in the capture harness; the conversion and build used the normal app code.

The build emits two warnings from the installed libnds `videoGL.h` about
ignored type qualifiers. They did not prevent compilation or packaging.

The check found a GUI validation bug: the Makefile creates a DSi-exclusive
header, but the GUI accepted only DSi-enhanced headers. The validator now accepts
both valid DSi types and still rejects DS-only or invalid unit codes. The GUI
export was repeated successfully after that fix.

No new physical DSi run, SD-card resume test, or Windows executable packaging
was performed. The desktop screenshots are not screenshots of the DSi reader.

Useful commands for another checkout:

```bash
python3 -m unittest discover -s tests -v
python3 -m py_compile main_gui.py pdf_importer.py pdf_to_book.py update_book_cfg.py
bash -n build_nds.sh
bash build_nds.sh
python3 pdf_importer.py example.pdf --mode split --pages-per-chapter 1 --format png
python3 pdf_to_book.py example.pdf --name "Example"
python3 update_book_cfg.py build/sd_card --dry-run
python3 main_gui.py
```

Build output, sample input, and exported books remain outside Git. Only the
two selected screenshots are kept with the documentation.
