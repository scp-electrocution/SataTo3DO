# Changelog

Fork of [FCare/SataTo3DO](https://github.com/FCare/SataTo3DO) with bug fixes,
Raspberry Pi Pico board support and protocol documentation.

All notable changes are listed here. Dates are ISO (YYYY-MM-DD).

---

## [fork-1.1] - 2026-10-21 — Raspberry Pi Pico support & documentation

### Added
- `PICO_BOARD` can now be overridden from the command line while keeping the
  project default (`waveshare_rp2040_plus_4mb`). Build for an official
  Raspberry Pi Pico (2 MB flash) with `-DPICO_BOARD=pico`.
- `docs/ODE_PROTOCOL.md`: full specification of the LOCODE ODE protocol
  (extended CD commands `0x93`, `0xC0`–`0xF0`, TOC buffer format, playlist and
  file commands) exposed by the firmware to the on-console menu (`boot.iso`).
  Reverse-engineered from the firmware so a custom menu can stay compatible.
- This `CHANGELOG.md`.

### Build matrix (prebuilt .uf2)
| File | Board | Flash | Options | Status |
|------|-------|-------|---------|--------|
| `SataTo3DO-PiPico-2MB.uf2` | Raspberry Pi Pico | 2 MB | default (DIR low) | ✅ tested on hardware: USB enumeration, NVRAM dump, game launch |
| `SataTo3DO-PiPico-2MB-DIR_IN_HIGH.uf2` | Raspberry Pi Pico | 2 MB | `USE_DIR_IN_HIGH` (DIR high) | ✅ tested on hardware (different PCB): game launch |
| `SataTo3DO-fixed-untested.uf2` | Waveshare RP2040-Plus | 4 MB | default | untested |
| `SataTo3DO-fixed-HS_FC-debug-untested.uf2` | Waveshare RP2040-Plus | 4 MB | `USE_HS_FC` + `USE_DEBUG` | untested |

> The two Raspberry Pi Pico builds target boards with opposite data-direction
> polarity and are mutually exclusive: use the one matching your PCB.

---

## [fork-1.0] - 2026-10-19 — Bug fixes: memory, deadlocks, protocol, images

A single commit fixing memory corruption, deadlocks and protocol bugs found by
a static review, verified by a host-side test suite (AddressSanitizer + UBSan)
covering the cue parsing, sector reads, TOC building and data-file logic.

### Fixed — Concurrency
- TinyUSB and FatFs were used from both cores at once (menu commands ran FatFs
  and `tuh_task()` on core 1 while core 0 also ran `tuh_task()`). All USB/FatFs
  work requested by the 3DO now runs on core 0 through `runOnCore0()`.
- `status` is modified under a critical section; shared flags are `volatile`.
- The media-check timer no longer sends USB commands from an IRQ, the USB reset
  is no longer done from inside a TinyUSB callback, and the FAT mount is no
  longer done from the mount callback.

### Fixed — Memory
- Double free in `loadBootIso` when the boot image could not be opened.
- `curBinPath` pointed to a playlist string freed by `clearPlaylist`
  (double free); image file paths are now owned copies.
- Leaks in `ValidateInfofromIso`/`ValidateInfofromCue` (one per listed file),
  `setTocLevel`, and when a playlist CD replaced the current image.
- Buffer overflows driven by the 3DO: `WRITE_BUFFER_OFFSET`,
  `READ`/`WRITE_FILE_BYTE` lengths, `GET_TOC` offset, `READ_TOC` track number,
  `SET_MODE` block size.
- Core stacks were 2 KB (the CMake `PICO_STACK_SIZE` variable had no effect)
  while a 2500-byte sector buffer lived on the stack: stacks are now 4 KB and
  the sector buffer is static.
- `getDevice()` no longer silently allocates slots for unknown addresses.
- FatFs `create_name()` read one byte past the end of the path.

### Fixed — Deadlocks
- `usb_cmd_on_going` stayed set forever when no command was queued (eject
  without capability, failed submissions): the USB loop stopped.
- `waitForLoad()` never returned without `boot.iso` or on a non-FAT key.
- Reads, sub-Q reads and answers now stop on 3DO reset, error or timeout.

### Fixed — 3DO protocol
- The 6 parameter bytes were left in the FIFO when no disc was present and were
  then executed as commands.
- `GET_DESC`, `READ_DATA` in LBA mode and `READ_DATA` with 0 blocks never
  answered.
- `GET_TOC` returned an empty device list (`getTocFull` with `nb = -1`).
- File name truncated by one character on `OPEN_FILE`; opening for read created
  the file; short reads were reported as full reads.
- Garbage was no longer sent to the 3DO after a failed read.
- `SPIN_UP` or-ed the error code instead of assigning it.

### Fixed — Images
- Multi-`FILE` `.cue` sheets (Redump, one file per track) now work: per-file
  INDEX positions, pregaps (`INDEX 00`) mapped to the right file, `INDEX 01` of
  track 1 as LBA 0.
- MSF seconds overflowed (`sec + 2 >= 60`).
- The header of 2352-byte images could be skipped twice (stale offset).
- Multi-sector reads of raw images read across sector boundaries.
- `isAudioBlock`/`readBlock` ignored the last track of images.
- `ValidateInfofromCue` checked the currently loaded image instead of the
  listed one.

### Fixed — Misc
- Undefined behaviour: unsequenced `i++` in TOC parsing, `id = (id++)%2`,
  missing return values, signed shifts, `isCmd` operator precedence.
- DMA helpers used the channel index instead of the claimed channel number.
- Drive strength was also written to the non-existent pads 30 and 31.
- The device counter was incremented on each inquiry and decremented for empty
  slots on USB reset.
- `disk_read`/`disk_write` ignored the SCSI status; prototypes now match FatFs.
- `LOG_SATA` without trace produced statements with no effect; log formats are
  now checked by the compiler.
