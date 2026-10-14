#!/usr/bin/env python3
"""Refresh ``book.cfg`` files for already exported Readmanna books.

The page images are not converted again.  The script scans the paired V3
detail/preview assets (plus optional preprocessed zoom assets) already on the SD card, rebuilds the chapter
counts, preserves the newest valid reader position, and writes one canonical
extended resume record containing page, rotation, and zoom.
"""

from __future__ import annotations

import argparse
import os
import re
import stat
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Optional


BASE_DIR = Path(__file__).resolve().parent
DEFAULT_SD_ROOT = BASE_DIR / "build" / "sd_card"

MANIFEST_MAGIC = "DS_MANGAMAN_BOOK_V3_ASSET"
DIMENSIONS_LINE = "960 1440 256 384"
LAST_POSITION_PREFIX = "last_position="
VALID_CROP_WIDTHS = (960, 768, 640, 512, 480, 384, 320)
ASSET_MAGICS = (b"DSMP8L10", b"DSM16L10")
PALETTE_BYTES = 256 * 2

CHAPTER_DIRECTORY_PATTERN = re.compile(r"^[0-9]{4}$")
BOOK_FOLDER_PATTERN = re.compile(r"^[A-Za-z0-9_-]{1,48}$")
PAGE_ASSET_PATTERN = re.compile(
    r"^page([0-9]{3,})_(full|preview|zoom480|zoom512)\.dsm$"
)
LEGACY_POSITION_PATTERN = re.compile(
    r"^last_position=([0-9]{4}):([0-9]{10})$"
)
EXTENDED_POSITION_PATTERN = re.compile(
    r"^last_position=([0-9]{4}):([0-9]{10}):([0-3]):([0-9]{4})$"
)


class BookConfigError(RuntimeError):
    """A book cannot be refreshed safely without user intervention."""


@dataclass(frozen=True)
class ReaderPosition:
    chapter: int
    page: int
    rotation: int
    crop_width: int


@dataclass(frozen=True)
class BookUpdate:
    folder: str
    title: str
    chapter_count: int
    page_count: int
    position: ReaderPosition
    changed: bool


def _valid_book_title(title: str) -> bool:
    return (
        0 < len(title) < 49
        and title == title.strip(" ")
        and any(character != " " for character in title)
        and all(0x20 <= ord(character) <= 0x7E for character in title)
    )


def _read_manifest(book_directory: Path) -> tuple[bytes, str, list[str]]:
    manifest_path = book_directory / "book.cfg"
    try:
        original = manifest_path.read_bytes()
        text = original.decode("ascii")
    except FileNotFoundError as exc:
        raise BookConfigError("book.cfg is missing") from exc
    except UnicodeDecodeError as exc:
        raise BookConfigError("book.cfg is not plain ASCII") from exc
    except OSError as exc:
        raise BookConfigError(f"could not read book.cfg: {exc}") from exc

    lines = text.splitlines()
    if len(lines) < 3:
        raise BookConfigError("book.cfg is missing its V3 header")
    if lines[0] != MANIFEST_MAGIC:
        raise BookConfigError(
            "unsupported asset format; export this book again with the current tools"
        )
    try:
        parsed_dimensions = tuple(int(value) for value in lines[1].split())
    except ValueError:
        parsed_dimensions = ()
    if parsed_dimensions != tuple(int(value) for value in DIMENSIONS_LINE.split()):
        raise BookConfigError(
            f"unsupported dimensions {lines[1]!r}; expected {DIMENSIONS_LINE}"
        )
    if not _valid_book_title(lines[2]):
        raise BookConfigError("book.cfg contains an invalid title")
    return original, lines[2], lines[3:]


def _parse_position(line: str) -> Optional[ReaderPosition]:
    extended = EXTENDED_POSITION_PATTERN.fullmatch(line.lstrip())
    if extended is not None:
        chapter, page, rotation, crop_width = map(int, extended.groups())
        if page < 1 or crop_width not in VALID_CROP_WIDTHS:
            return None
        return ReaderPosition(chapter, page, rotation, crop_width)

    legacy = LEGACY_POSITION_PATTERN.fullmatch(line.lstrip())
    if legacy is not None:
        chapter, page = map(int, legacy.groups())
        if page < 1:
            return None
        return ReaderPosition(chapter, page, 0, VALID_CROP_WIDTHS[0])
    return None


def _latest_valid_position(lines: Iterable[str]) -> Optional[ReaderPosition]:
    latest = None
    for line in lines:
        parsed = _parse_position(line)
        if parsed is not None:
            latest = parsed
    return latest


def _validate_asset_header(asset_path: Path, role: str) -> None:
    expected_dimensions = {
        "full": (960, 1440),
        "preview": (256, 384),
        "zoom480": (480, 720),
        "zoom512": (512, 768),
    }
    expected_width, expected_height = expected_dimensions[role]
    try:
        with asset_path.open("rb") as asset:
            magic = asset.read(8)
            if magic not in ASSET_MAGICS:
                raise BookConfigError(
                    f"{asset_path.name} has an unsupported asset header"
                )
            if magic == ASSET_MAGICS[0]:
                palette = asset.read(PALETTE_BYTES)
                if len(palette) != PALETTE_BYTES:
                    raise BookConfigError(
                        f"{asset_path.name} has a truncated palette"
                    )
            lz_header = asset.read(4)
    except BookConfigError:
        raise
    except OSError as exc:
        raise BookConfigError(f"could not read {asset_path.name}: {exc}") from exc

    if len(lz_header) != 4 or lz_header[0] != 0x10:
        raise BookConfigError(f"{asset_path.name} has no valid LZ10 stream")
    expanded_bytes = (
        lz_header[1] | (lz_header[2] << 8) | (lz_header[3] << 16)
    )
    bytes_per_pixel = 1 if magic == ASSET_MAGICS[0] else 2
    expected_bytes = expected_width * expected_height * bytes_per_pixel
    if expanded_bytes != expected_bytes:
        raise BookConfigError(
            f"{asset_path.name} expands to {expanded_bytes} bytes; "
            f"expected {expected_bytes}"
        )


def _scan_chapter(chapter_directory: Path) -> int:
    page_roles: dict[int, set[str]] = {}
    try:
        entries = list(chapter_directory.iterdir())
    except OSError as exc:
        raise BookConfigError(
            f"could not scan chapter {chapter_directory.name}: {exc}"
        ) from exc

    for entry in entries:
        if not entry.is_file():
            continue
        match = PAGE_ASSET_PATTERN.fullmatch(entry.name)
        if match is None:
            if entry.name.startswith("page") and entry.suffix == ".dsm":
                raise BookConfigError(
                    f"chapter {chapter_directory.name} contains malformed asset "
                    f"name {entry.name!r}"
                )
            continue

        page_text, role = match.groups()
        page_number = int(page_text)
        if (
            page_number < 1
            or page_number > 2_147_483_647
            or page_text != f"{page_number:03d}"
        ):
            raise BookConfigError(
                f"chapter {chapter_directory.name} contains non-canonical asset "
                f"name {entry.name!r}"
            )
        _validate_asset_header(entry, role)
        page_roles.setdefault(page_number, set()).add(role)

    if not page_roles:
        raise BookConfigError(
            f"chapter {chapter_directory.name} contains no page assets"
        )

    ordered_pages = sorted(page_roles)
    for expected_page, page_number in enumerate(ordered_pages, start=1):
        if page_number != expected_page:
            raise BookConfigError(
                f"chapter {chapter_directory.name} is missing page {expected_page}"
            )
        roles = page_roles[page_number]
        required_roles = {"full", "preview"}
        if not required_roles.issubset(roles):
            missing_role = "preview" if "preview" not in roles else "full"
            raise BookConfigError(
                f"chapter {chapter_directory.name}, page {page_number} is missing "
                f"its {missing_role} asset"
            )
    return len(ordered_pages)


def _scan_chapters(book_directory: Path) -> list[tuple[int, int]]:
    try:
        chapter_directories = sorted(
            (
                entry
                for entry in book_directory.iterdir()
                if entry.is_dir()
                and CHAPTER_DIRECTORY_PATTERN.fullmatch(entry.name)
            ),
            key=lambda path: int(path.name),
        )
    except OSError as exc:
        raise BookConfigError(f"could not scan book directory: {exc}") from exc

    if not chapter_directories:
        raise BookConfigError("no four-digit chapter directories were found")
    if len(chapter_directories) > 256:
        raise BookConfigError("the reader supports at most 256 chapters per book")

    chapters = []
    for chapter_directory in chapter_directories:
        chapters.append((int(chapter_directory.name), _scan_chapter(chapter_directory)))
    return chapters


def _canonical_position(
    saved_position: Optional[ReaderPosition],
    chapters: list[tuple[int, int]],
) -> ReaderPosition:
    if saved_position is not None:
        page_counts = dict(chapters)
        if saved_position.chapter in page_counts:
            return ReaderPosition(
                saved_position.chapter,
                min(max(1, saved_position.page), page_counts[saved_position.chapter]),
                saved_position.rotation,
                saved_position.crop_width,
            )
        return ReaderPosition(
            chapters[0][0],
            1,
            saved_position.rotation,
            saved_position.crop_width,
        )
    return ReaderPosition(chapters[0][0], 1, 0, VALID_CROP_WIDTHS[0])


def _render_manifest(
    title: str,
    chapters: list[tuple[int, int]],
    position: ReaderPosition,
) -> bytes:
    lines = [
        MANIFEST_MAGIC,
        DIMENSIONS_LINE,
        title,
        (
            f"{LAST_POSITION_PREFIX}{position.chapter:04d}:"
            f"{position.page:010d}:{position.rotation}:{position.crop_width:04d}"
        ),
    ]
    lines.extend(f"{chapter:04d} {page_count}" for chapter, page_count in chapters)
    return ("\n".join(lines) + "\n").encode("ascii")


def _atomic_replace(manifest_path: Path, content: bytes) -> None:
    temporary_path: Optional[Path] = None
    original_mode = None
    try:
        original_mode = stat.S_IMODE(manifest_path.stat().st_mode)
        descriptor, temporary_name = tempfile.mkstemp(
            prefix=".book.cfg.",
            suffix=".tmp",
            dir=manifest_path.parent,
        )
        temporary_path = Path(temporary_name)
        with os.fdopen(descriptor, "wb") as temporary:
            temporary.write(content)
            temporary.flush()
            os.fsync(temporary.fileno())
        if original_mode is not None:
            os.chmod(temporary_path, original_mode)
        os.replace(temporary_path, manifest_path)
        temporary_path = None

        # Linux filesystems can also flush the directory entry. Some FAT
        # mounts and Windows do not support directory fsync, so the already
        # completed atomic replacement remains valid when this optional step
        # is unavailable.
        try:
            directory_flags = os.O_RDONLY | getattr(os, "O_DIRECTORY", 0)
            directory_descriptor = os.open(manifest_path.parent, directory_flags)
            try:
                os.fsync(directory_descriptor)
            finally:
                os.close(directory_descriptor)
        except OSError:
            pass
    except OSError as exc:
        raise BookConfigError(f"could not replace book.cfg atomically: {exc}") from exc
    finally:
        if temporary_path is not None:
            try:
                temporary_path.unlink()
            except FileNotFoundError:
                pass


def update_book(book_directory: Path, dry_run: bool = False) -> BookUpdate:
    if BOOK_FOLDER_PATTERN.fullmatch(book_directory.name) is None:
        raise BookConfigError(
            "the folder name is not visible to the reader; use 1-48 ASCII "
            "letters, digits, hyphens, or underscores"
        )
    original, title, manifest_lines = _read_manifest(book_directory)
    chapters = _scan_chapters(book_directory)
    position = _canonical_position(
        _latest_valid_position(manifest_lines),
        chapters,
    )
    replacement = _render_manifest(title, chapters, position)
    changed = replacement != original
    if changed and not dry_run:
        _atomic_replace(book_directory / "book.cfg", replacement)

    return BookUpdate(
        folder=book_directory.name,
        title=title,
        chapter_count=len(chapters),
        page_count=sum(page_count for _, page_count in chapters),
        position=position,
        changed=changed,
    )


def _resolve_book_directories(root: Path) -> list[Path]:
    root = root.expanduser().resolve()
    if (root / "book.cfg").is_file():
        return [root]

    candidates = (
        root / "readmanna" / "books",
        root / "books",
        root,
    )
    books_root = next(
        (
            candidate
            for candidate in candidates
            if candidate.is_dir()
            and (
                candidate.name == "books"
                or any(
                    entry.is_dir() and (entry / "book.cfg").is_file()
                    for entry in candidate.iterdir()
                )
            )
        ),
        None,
    )
    if books_root is None:
        raise BookConfigError(
            f"no exported books folder was found below {root}"
        )

    book_directories = sorted(
        (
            entry
            for entry in books_root.iterdir()
            if entry.is_dir()
            and not entry.name.startswith(".")
            and (entry / "book.cfg").is_file()
        ),
        key=lambda path: (path.name.casefold(), path.name),
    )
    if not book_directories:
        raise BookConfigError(f"no exported books were found in {books_root}")
    return book_directories


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Refresh current Readmanna V3 book.cfg files from their existing "
            "page assets without re-exporting the books."
        )
    )
    parser.add_argument(
        "path",
        nargs="?",
        default=str(DEFAULT_SD_ROOT),
        help=(
            "SD-card root, readmanna folder, books folder, or one book folder "
            "(default: build/sd_card next to this script)."
        ),
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Validate and report changes without writing book.cfg files.",
    )
    return parser


def main(argv: Optional[list[str]] = None) -> int:
    args = _build_parser().parse_args(argv)
    try:
        book_directories = _resolve_book_directories(Path(args.path))
    except BookConfigError as exc:
        print(f"[ERROR] {exc}", file=sys.stderr)
        return 1

    updated = 0
    unchanged = 0
    failures = 0
    for book_directory in book_directories:
        try:
            result = update_book(book_directory, dry_run=args.dry_run)
        except BookConfigError as exc:
            failures += 1
            print(f"[ERROR] {book_directory.name}: {exc}", file=sys.stderr)
            continue

        if result.changed:
            updated += 1
            status = "WOULD UPDATE" if args.dry_run else "UPDATED"
        else:
            unchanged += 1
            status = "OK"
        position = result.position
        print(
            f"[{status}] {result.folder}: {result.chapter_count} chapters, "
            f"{result.page_count} pages; resume {position.chapter:04d}:"
            f"{position.page}, rotation={position.rotation * 90} degrees, "
            f"crop={position.crop_width}"
        )

    print(
        f"\nChecked {len(book_directories)} book(s): "
        f"{updated} update(s), {unchanged} unchanged, {failures} failed."
    )
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
