---
name: Release process — binaries zip + GitHub release
description: Every working build gets a GitHub release with individual binaries and a binaries zip
type: feedback
originSessionId: a3e8548a-cc89-4b3c-a902-a1e183651511
---
On every working build: tag the commit, create a GitHub release, attach the three flash binaries individually AND as a zip named `pdk_reader_vX.Y.Z_binaries.zip`.

**Why:** User wants to grab binaries and flash from any machine using esptool without needing the build environment.

**How to apply:** After every clean build + commit, run `gh release create vX.Y.Z` with all three bins attached, then zip them and `gh release upload` the zip.

Binaries and their flash addresses:
- `bootloader.bin` → `0x0`
- `partition-table.bin` → `0x10000`
- `pdk_reader.bin` → `0x20000`
