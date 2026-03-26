# Changelog

## v2.0.0 (2026-03-26) - Major release

This release turns md-browser into a much more capable disk manager, with full Atari ST image workflows, a redesigned Floppy DB experience, and a stronger runtime for the RP2040.

### Changes
- Full restyle of the Floppy DB page and shared browser/file manager header navigation.
- Added flash usage reporting and web asset minification to the RP build.
- Reduced ROM emulation RAM usage to 64KB to recover memory for new features.

### New features
- Create blank Atari ST disk images in 360KB (DS), 720KB (DD), 1.44MB (HD), and 2.88MB (ED) formats.
- Support both `.st` and `.st.rw` image creation modes.
- Convert floppy images between `.MSA` and `.ST`.
- Copy and move files and folders on the microSD card with progress reporting.
- Browse FAT filesystems inside `.ST` and `.st.rw` images directly from the web file manager.
- Copy files and folders from ST images to the microSD card.
- Import files and folders from the microSD card into writable `.st.rw` images, with automatic Microsoft-style 8.3 name conversion when needed.
- Rename and delete files and folders inside writable `.st.rw` images.
- Return to Booster directly from the browser and file manager menus.
- Replaced the old download page with an in-page download polling flow.

### Fixes
- Fixed folder listing failures caused by JSON payload truncation on large directories.
- Reduced stack and memory pressure across HTTP, copy/move, floppy conversion, and image import/export paths.
- Improved select button handling and core 1 stability.
- Adopted newer Booster network stack improvements, including safer runtime reset, cleaner STA reconnect handling, better DNS parsing, and improved mDNS lifecycle management.
- Replaced remaining native browser dialogs with modal-based UI flows for a more consistent experience.

---

## v1.1.0 (2025-12-17) - Stable release

This is the first stable release of the File and Download manager app. It includes several new features and bug fixes based on feedback from the beta release.

### Changes
- Bump pico SDK and pico extras to version 2.2.0.
- 'What is new' enabled by default.

### New features
- New mDNS support for easier device discovery on local networks. The user can open it with "http://sidecart.local" in their web browser.
- Link to the 'Contribute' floppy disk tutorial.
- Added AGENTS.md for agentic development.

### Fixes
- Fixed buffer overflow when copying the firmware to the ROM in RAM area.
- Optimized memory usage in network stack.
- Fixed renaming failure

---

## v1.0.0beta (2025-07-28) - Beta release

This is a beta release. It's ready for public testing, but may still contain bugs and issues. Please report any problems you encounter.

### Changes
- First public beta release of the File and Download manager app.

### New features
- First public beta release of the File and Download manager app.

### Fixes
- First public beta release of the File and Download manager app.

Note: This release does not include:
- Support for HTTPS connections.

---
