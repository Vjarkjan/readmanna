# Readmanna

Readmanna is a DSi-only dual-screen comic and document reader. One page is
displayed across a logical 256 x 384 viewport, with the upper half on
the top LCD and the lower half on the touchscreen LCD.

Books are stored as external files on the SD card. The reusable reader ROM is
about 250 KiB, so launching a 300 MiB book no longer requires the loader to
scan a 300 MiB NitroFS ROM. Page textures use Nintendo LZ10 compression and are
expanded only after the selected page is read. At minimum zoom, the reader
loads only the small filtered preview; the detailed texture is loaded when you
zoom in.

## Export one PDF directly

For the shortest workflow, give `pdf_to_book.py` one PDF:

```sh
./pdf_to_book.py "/path/to/My Book.pdf"
```

It derives the book name from the PDF filename and writes the finished book to:

```text
build/sd_card/readmanna/books/my-book/
```

The whole PDF becomes one internal chapter named `0001`; no source chapter
folders need to be prepared. Each page is streamed through a temporary JPEG
and immediately converted to the reader's compressed `.dsm` detail,
prefiltered 512 x 768 zoom, and preview assets. The defaults are JPEG quality
92, Bicubic resizing, Balanced
enhancement, fit-entire-page framing, white padding, and Compact 256-color
storage. Temporary JPEG files are removed, so the finished book contains only
`book.cfg` and the processed reader assets.

To write directly to a mounted SD card instead, pass its root:

```sh
./pdf_to_book.py "/path/to/My Book.pdf" \
    --sd-root "/run/media/$USER/DSI_SD"
```

The GUI's **PDF → SD Book** button runs this same one-step export into
`build/sd_card`. Exporting the same normalized PDF name atomically replaces
only that book. This operation does not compile or replace `Readmanna.nds`.

## Controls

| Control | Action |
|---|---|
| Touch drag | Move the page directly |
| D-pad | Pan the view; the page moves opposite the pressed direction |
| X / Y | Next / previous page, crossing chapter boundaries |
| L | Rotate the image view 90 degrees clockwise |
| R | Rotate the image view 90 degrees counterclockwise |
| SELECT | Zoom in |
| START | Zoom out |
| START + SELECT | Open the book picker; B cancels |
| A | Reset to minimum zoom and center the page |
| B | Open the chapter-local page-number dialog |

The page can move outside the viewport. Areas beyond it are white, and motion
stops only when the page reaches the opposite viewport boundary.
The D-pad axes rotate with the image view, while touch dragging continues to
move the page directly under the finger.

## Export a book

Run `main_gui.py`, enter a **Book name**, choose the chapters and preprocessing
options, then click **Export Book + Reader**. The result is:

```text
build/sd_card/
├── Readmanna.nds
└── readmanna/
    └── books/
        └── example-book/
            ├── book.cfg
            ├── 0010/
            │   ├── page001_full.dsm
            │   ├── page001_zoom480.dsm
            │   ├── page001_zoom512.dsm
            │   └── page001_preview.dsm
            └── ...
```

Copy both `Readmanna.nds` and the `readmanna` folder to the root of the DSi
SD card. Launch `Readmanna.nds` in DSi mode. If one valid book is present it
opens automatically; if several are present, choose one with the D-pad and A.

Exporting another name adds another book. Exporting the same normalized name
replaces only that book. The GUI stages and validates all pages before exposing
the replacement, and preserves the previous book and reader if export fails.
Re-exporting the same book also carries its last page, rotation, and zoom into
the replacement `book.cfg`. If the replacement has fewer pages, only the page
number is clamped; the saved rotation and zoom are retained.

The compressed format uses the `DS_MANGAMAN_BOOK_V3_ASSET` manifest. Books made
with an older raw or LZ-only format must be exported again with the current
GUI; the reader rejects them instead of interpreting incompatible pixels.

Each `book.cfg` also contains a fixed-width resume entry such as:

```text
last_position=0010:0000000025:1:0512
```

The fields are chapter folder, page, clockwise quarter-turns, and crop width.
Thus the example resumes chapter `0010`, page 25, rotated 90 degrees clockwise,
at the stable `512 x 768` zoom. The crop width is stored instead of the array
index, so inserting another zoom level does not change a saved view. Page,
rotation, zoom, and A-button zoom resets update this entry on the SD card.

Legacy V3 records containing only chapter and page remain valid and open at 0
degrees with minimum zoom. Their next save appends the extended fixed-width
record without rewriting the chapter list.

## Update existing book configs

To refresh books that are already exported, run the maintenance script against
the SD-card root (or against `build/sd_card` when the path is omitted):

```sh
./update_book_cfg.py "/run/media/$USER/DSI_SD"
```

The script reads the existing detail/preview assets plus any optional
`zoom480` / `zoom512` assets, rebuilds the chapter page counts, preserves the newest valid
saved page/rotation/zoom, and
upgrades legacy resume entries to the current fixed-width format. It never
reprocesses or modifies page images. Each changed `book.cfg` is written through
an atomic same-directory replacement, while missing pages, incomplete asset
pairs, and incompatible formats are reported without touching that book.

Use `--dry-run` to validate and preview the changes first:

```sh
./update_book_cfg.py "/run/media/$USER/DSI_SD" --dry-run
```

Do not launch an older, hundreds-of-megabytes per-book ROM: those files still
contain embedded page data and retain the old long startup behavior.

## Image sources

Source chapters live below `assets/jpg_comic/` in four-digit folders:

```text
assets/jpg_comic/
├── 0010/
│   ├── 1.jpg
│   ├── 2.jpg
│   └── 10.jpg
└── 0020/
    └── 1.png
```

Filenames are naturally sorted, so `2.jpg` precedes `10.jpg`. The PDF importer
can create these chapter folders from a PDF.

The recommended settings for text and line art are **Fit entire page**,
**White**, **Lanczos**, **Crisp text / manga**, and **Compact 256 colors**.
Every page produces:

- a gently sharpened 960 x 1440 detailed texture;
- a host-preprocessed 512 x 768 reading texture;
- a separately filtered 256 x 384 minimum-zoom preview;
- an LZ10-compressed stream for each texture.

Compact mode quantizes each page to its own 256-color palette before lossless
LZ10 compression. Full-color mode instead stores every texture as RGB555.

Compressed size depends heavily on page contents and storage mode. The added
480 x 720 and 512 x 768 assets deliberately increase each exported book's
SD-card footprint in exchange for eliminating DSi-side resize work at those
middle zoom levels.
**Compact 256 colors** remains the recommended default for books and manga; it
uses a page-specific palette before lossless LZ10 compression, while **Full
color RGB555** keeps all 15-bit colors.

Four detailed zoom levels still build an exact-resolution, area-filtered
texture from the 960 x 1440 detail page when the zoom or page changes. The
480 x 720 and 512 x 768 caches are different: they are created on the host
and stored as `pageNNN_zoom480.dsm` and `pageNNN_zoom512.dsm`, so the DSi only
reads and BIOS-decompresses them instead of resizing the 960 x 1440 page.
In the current zoom geometry, the 480 x 720 cache corresponds to the 512 x 768
source-crop level, while the 512 x 768 cache corresponds to the 480 x 720
source-crop level. When a page is opened at either zoom, the reader can also
avoid loading the full 960 x 1440 asset unless another zoom later requires it.

Normal rendering reads one cached texel for each LCD pixel. When L or R changes
orientation, a 480 x 720 or 512 x 768 host texture is rotated once into the same runtime
cache; other zoom caches are built directly in the requested physical
rotation. Upright and quarter-turned views can therefore continue copying
contiguous visible row spans during drag or D-pad frames. Maximum zoom still
keeps the same apparent 3x magnification with its prefiltered 768 x 1152 runtime
cache.

## Requirements and building

- A Nintendo DSi running the application in DSi mode.
- devkitPro with the NDS development group (devkitARM, libnds, Calico, libfat,
  ndstool, and grit).
- Python 3 with the packages in `requirements.txt`; Linux also needs its Tk
  package, commonly `python3-tk`.

The GUI discovers `DEVKITPRO`, `/opt/devkitpro`, and standard Windows devkitPro
locations. Repository paths containing spaces or parentheses are supported.

To build only the reusable reader from Linux, without opening the GUI or
processing any book images:

```sh
./build_nds.sh
```

The script accepts `--clean` for a clean rebuild, discovers `DEVKITPRO` and
`DEVKITARM` (including the usual `/opt/devkitpro` installation), and writes
`build/Readmanna.nds`. It can be launched directly from Fish because its Bash
shebang selects the correct interpreter. The equivalent manual command is
`make --always-make TARGET=build/Readmanna NITRO_FILES=`.

The reader mounts the SD card with libfat and looks only below
`/readmanna/books`. It does not embed or scan the generated books during ROM
startup.
