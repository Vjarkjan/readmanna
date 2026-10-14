# Source snapshot

The starting point is `readmanna-preprocessed-480x720-512x768`, the newest
Readmanna source snapshot found in the local Downloads and project folders
during this setup. Its archive was saved on 30 August 2026. The source files
in the extracted folder match that archive.

The other local snapshots progress through ARM/ITCM changes, filter and cache
optimizations, a 512 × 768 preprocessed image, and finally both 480 × 720 and
512 × 768 images. The existing related
[ds-mangaman repository](https://github.com/Vjarkjan/ds-mangaman) was also
checked; its latest commit at the time was from 29 August 2026. This identifies
the newest version found in those sources, not an upstream numbered release.

Archive SHA-256:
`ab3475db8acbfb6b3e496889e2d8ecdff3402855f50a3898fe0b5aef8391dd52`.

[source-inventory.json](source-inventory.json) records the original program and
tool hashes. The reader, converters, build files, icon, and bundled Windows
devkitPro installer were copied from that snapshot. Generated books, compiler
output, caches, and the older DS-Mangaman demo pictures were excluded.
The previous README is retained as [technical notes](previous-readme.md).

The repository setup adds an English README, current desktop screenshots,
validation notes, and a regression test. Two small GUI changes are recorded:

- Accept DSi-exclusive ROM headers as well as DSi-enhanced headers. The
  Makefile already builds a DSi-exclusive ROM, so the previous check rejected
  a successful build and prevented GUI exports from completing.
- Show both preprocessed reading sizes in the output description.

Text line endings are normalized for Git, and an extra blank line at the end
of the Makefile is removed. The C++ reader and book format are unchanged.
The source snapshot did not
include a license file; this setup does not assign a new license to it.
