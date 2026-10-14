#!/usr/bin/env python3
"""Export one PDF directly as a processed Readmanna SD-card book.

The complete PDF is always stored as one internal chapter (0001). Pages are
rasterized to a temporary JPEG one at a time, then passed through the same V3
asset pipeline used by main_gui.py. The temporary JPEG is removed immediately
after its detail, prefiltered 480x720/512x768, and preview assets have been created.
"""

from __future__ import annotations

import argparse
import importlib
import os
import shutil
import sys
import tempfile
from dataclasses import dataclass
from typing import Callable, Optional

import pymupdf


if getattr(sys, "frozen", False):
    BASE_DIR = os.path.dirname(sys.executable)
else:
    BASE_DIR = os.path.dirname(os.path.abspath(__file__))

DEFAULT_SD_ROOT = os.path.join(BASE_DIR, "build", "sd_card")
DEFAULT_DPI = 200
DEFAULT_JPEG_QUALITY = 92
SINGLE_CHAPTER = "0001"


class PdfBookExportError(RuntimeError):
    """An export error whose message can be shown directly to the user."""


@dataclass(frozen=True)
class PdfBookExportResult:
    book_title: str
    book_id: str
    book_path: str
    page_count: int
    asset_bytes: int


def _load_pipeline():
    """Loads the shared V3 asset pipeline without starting the Tk GUI."""
    try:
        pipeline = importlib.import_module("main_gui")
    except Exception as exc:
        raise PdfBookExportError(
            f"Could not load the Readmanna preprocessing pipeline: {exc}"
        ) from exc

    if not pipeline.verify_toolchain_infrastructure(interactive=False):
        raise PdfBookExportError(
            "devkitPro with grit was not found. Set DEVKITPRO (normally "
            "/opt/devkitpro) and install the NDS development group."
        )
    return pipeline


def _validate_generated_chapter(chapter_dir: str, page_count: int) -> None:
    for page_number in range(1, page_count + 1):
        for asset_role in ("full", "preview", "zoom480", "zoom512"):
            asset_path = os.path.join(
                chapter_dir,
                f"page{page_number:03d}_{asset_role}.dsm",
            )
            if not os.path.isfile(asset_path) or os.path.getsize(asset_path) <= 12:
                raise PdfBookExportError(
                    f"Page {page_number} is missing its {asset_role} asset."
                )


def _existing_book_ids(books_root: str, pipeline) -> list[str]:
    if not os.path.isdir(books_root):
        return []
    return [
        entry.name
        for entry in os.scandir(books_root)
        if entry.is_dir()
        and pipeline.is_safe_book_folder(entry.name)
        and os.path.isfile(os.path.join(entry.path, "book.cfg"))
    ]


def export_pdf_to_sd_book(
    pdf_path: str,
    sd_root: str = DEFAULT_SD_ROOT,
    book_name: Optional[str] = None,
    dpi: int = DEFAULT_DPI,
    jpeg_quality: int = DEFAULT_JPEG_QUALITY,
    progress_cb: Optional[Callable[[int, int, str], None]] = None,
) -> PdfBookExportResult:
    """Converts a PDF directly into ``sd_root/readmanna/books``.

    Defaults are intentionally fixed for the one-step workflow:

    - temporary JPEG source pages;
    - Bicubic resizing;
    - Balanced enhancement;
    - Fit-entire-page framing with white padding;
    - compact 256-color V3 assets;
    - one internal chapter named ``0001``.

    Exporting the same normalized PDF/book name atomically replaces that book.
    Other books and the reader ROM are never modified.
    """
    pdf_path = os.path.abspath(os.path.expanduser(pdf_path))
    sd_root = os.path.abspath(os.path.expanduser(sd_root))
    if not os.path.isfile(pdf_path):
        raise PdfBookExportError(f"PDF file not found: {pdf_path}")
    if dpi < 72 or dpi > 600:
        raise PdfBookExportError("DPI must be between 72 and 600.")
    if jpeg_quality < 1 or jpeg_quality > 100:
        raise PdfBookExportError("JPEG quality must be between 1 and 100.")

    pipeline = _load_pipeline()
    title_source = book_name or os.path.splitext(os.path.basename(pdf_path))[0]
    try:
        book_title = pipeline.normalize_book_title(title_source)
        book_id = pipeline.make_book_id(book_title)
    except ValueError as exc:
        raise PdfBookExportError(str(exc)) from exc

    settings = pipeline.PreprocessSettings(
        rotation="No rotation",
        fit_mode="Fit entire page",
        background="White",
        resampling="Bicubic",
        enhancement="Balanced",
        storage="Compact 256 colors",
    )
    pipeline.validate_preprocess_settings(settings)

    books_root = os.path.join(sd_root, "readmanna", "books")
    staging_parent = os.path.join(books_root, ".pdf-book-staging")
    destination_book = os.path.join(books_root, book_id)
    preserved_resume_state = pipeline.read_book_resume_state(destination_book)
    staging_dir = None
    jpeg_work_dir = None
    document = None
    total_asset_bytes = 0
    try:
        try:
            os.makedirs(books_root, exist_ok=True)
            existing_books = _existing_book_ids(books_root, pipeline)
            if (
                book_id not in existing_books
                and len(existing_books) >= pipeline.MAX_BOOKS
            ):
                raise PdfBookExportError(
                    f"The destination already contains {pipeline.MAX_BOOKS} books, "
                    "which is the reader's maximum."
                )

            os.makedirs(staging_parent, exist_ok=True)
            staging_dir = tempfile.mkdtemp(
                prefix=f"{book_id}-",
                dir=staging_parent,
            )
            jpeg_work_dir = tempfile.mkdtemp(
                prefix="jpeg-pages-",
                dir=staging_parent,
            )
            chapter_dir = os.path.join(staging_dir, SINGLE_CHAPTER)
            os.makedirs(chapter_dir, exist_ok=False)
        except PdfBookExportError:
            raise
        except OSError as exc:
            raise PdfBookExportError(
                f"Could not prepare the SD-card destination: {exc}"
            ) from exc

        try:
            document = pymupdf.open(pdf_path)
        except Exception as exc:
            raise PdfBookExportError(f"Could not open the PDF: {exc}") from exc

        if not document.is_pdf:
            raise PdfBookExportError("The selected input is not a PDF document.")
        if document.needs_pass:
            raise PdfBookExportError("Password-protected PDFs are not supported.")
        if document.page_count < 1:
            raise PdfBookExportError("The PDF contains no pages.")

        page_count = document.page_count
        for page_number in range(1, page_count + 1):
            jpeg_path = os.path.join(jpeg_work_dir, "current-page.jpg")
            try:
                page = document.load_page(page_number - 1)
                pixmap = page.get_pixmap(
                    dpi=dpi,
                    colorspace=pymupdf.csRGB,
                    alpha=False,
                )
                image = pixmap.pil_image().convert("RGB")
                try:
                    image.save(
                        jpeg_path,
                        format="JPEG",
                        quality=jpeg_quality,
                        subsampling=0,
                        optimize=False,
                    )
                finally:
                    image.close()

                full_bytes, preview_bytes, zoom480_bytes, zoom512_bytes = pipeline.process_image(
                    jpeg_path,
                    chapter_dir,
                    page_number,
                    settings,
                )
                total_asset_bytes += (
                    full_bytes + preview_bytes + zoom480_bytes + zoom512_bytes
                )
            except PdfBookExportError:
                raise
            except Exception as exc:
                raise PdfBookExportError(
                    f"PDF page {page_number}/{page_count} failed: {exc}"
                ) from exc
            finally:
                if os.path.exists(jpeg_path):
                    os.unlink(jpeg_path)

            if progress_cb:
                progress_cb(
                    page_number,
                    page_count,
                    f"Processed PDF page {page_number}/{page_count}",
                )

        pipeline.write_book_manifest(
            staging_dir,
            book_title,
            [(SINGLE_CHAPTER, page_count)],
            resume_state=preserved_resume_state,
        )
        _validate_generated_chapter(chapter_dir, page_count)

        backup_dir = pipeline.promote_staging_directory(
            staging_dir,
            destination_book,
        )
        staging_dir = None
        if backup_dir and os.path.exists(backup_dir):
            try:
                shutil.rmtree(backup_dir)
            except OSError:
                # The new book is already complete and active. A hidden backup
                # is safer than failing after a successful atomic promotion.
                pass

        return PdfBookExportResult(
            book_title=book_title,
            book_id=book_id,
            book_path=destination_book,
            page_count=page_count,
            asset_bytes=total_asset_bytes,
        )
    except PdfBookExportError:
        raise
    except Exception as exc:
        raise PdfBookExportError(f"PDF book export failed: {exc}") from exc
    finally:
        if document is not None:
            try:
                document.close()
            except Exception:
                # Cleanup must not hide the useful page/export error above.
                pass
        if staging_dir and os.path.exists(staging_dir):
            shutil.rmtree(staging_dir, ignore_errors=True)
        if jpeg_work_dir and os.path.exists(jpeg_work_dir):
            shutil.rmtree(jpeg_work_dir, ignore_errors=True)
        try:
            os.rmdir(staging_parent)
        except OSError:
            # It can be non-empty when another export is running or when a
            # recoverable staging artifact must remain available.
            pass


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Convert one PDF directly into a processed Readmanna SD-card "
            "book with one internal chapter."
        )
    )
    parser.add_argument("pdf", help="Input PDF path.")
    parser.add_argument(
        "--sd-root",
        default=DEFAULT_SD_ROOT,
        help=(
            "SD-card root or package folder. Defaults to build/sd_card next "
            "to this script."
        ),
    )
    parser.add_argument(
        "--name",
        default=None,
        help="Optional book title. The PDF filename is used by default.",
    )
    parser.add_argument("--dpi", type=int, default=DEFAULT_DPI)
    parser.add_argument(
        "--jpeg-quality",
        type=int,
        default=DEFAULT_JPEG_QUALITY,
    )
    return parser


def main(argv=None) -> int:
    args = _build_parser().parse_args(argv)

    def show_progress(done: int, total: int, message: str) -> None:
        if done == 1 or done == total or done % 5 == 0:
            print(f"[{done:4d}/{total:4d}] {message}")

    try:
        result = export_pdf_to_sd_book(
            pdf_path=args.pdf,
            sd_root=args.sd_root,
            book_name=args.name,
            dpi=args.dpi,
            jpeg_quality=args.jpeg_quality,
            progress_cb=show_progress,
        )
    except PdfBookExportError as exc:
        print(f"[ERROR] {exc}", file=sys.stderr)
        return 1

    print("\nProcessed book ready:")
    print(f"  Title: {result.book_title}")
    print(f"  Pages: {result.page_count}")
    print(f"  Path:  {result.book_path}")
    print(f"  Size:  {result.asset_bytes / (1024 * 1024):.2f} MiB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
