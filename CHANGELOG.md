# Changelog

All notable changes to LycanZip are listed here, one section per version. Versions follow
[Semantic Versioning](https://semver.org/): from 1.0 on, the settings files (`settings.ini`, `presets.ini`) and the command line
stay compatible within a major version. Release notes are taken from this file.

## 1.0.0 (2026-10-26)

The first public version. The 0.x versions before it went only to a small group of testers; what they changed is summed up here.

- Open and browse: zip, 7z, rar (extract only), tar, gz, xz, bz2, zst, iso, cab and everything else the 7-Zip engine reads, including split archives (`.7z.001`, `.part1.rar`, `.z01`) and `.tar.gz` style archives (opened as one tar). Folder tree, breadcrumb, details / list / icon views with thumbnails, search, band selection, sortable columns (ratio, encryption, method and CRC on request).
- Previews without extracting: pictures with a thumbnail strip, EXIF orientation and animated GIF / WebP (save in another format, set as the desktop picture); an audio player with tags, technical information and playback modes. Hold Shift to go straight to the picture preview.
- Old zip and tar archives: Big5, GBK, Shift-JIS, EUC-KR and UTF-8 file names are detected automatically, or chosen by hand.
- Create zip, 7z, tar, tar.gz, tar.xz, tar.bz2 and ISO images: templates, levels, solid blocks, split volumes, AES-256, encrypted file names in 7z, smart storing of files that are compressed already, and checksum files written next to the archive.
- Change archives in place (zip, 7z, tar): add with update modes (replace, update, freshen, sync), delete, rename, make folders, edit a file and put it back. Unchanged items are copied without being recompressed, and nothing is touched when a change fails.
- Extract: smart extraction (one root folder goes in directly, several items get a folder named like the archive), per-file conflict handling, damaged files kept with what could be read. Drag files and folders out to File Explorer while they are unpacked (also from solid 7z archives, in one pass).
- Checksums: CRC-32, MD5, SHA-1, SHA-256, SHA-384 and SHA-512 for files, folders and the content of archives; verify `.sha256`, `.md5`, `.sfv` and similar files, also against the content of an archive; "Look up on VirusTotal" opens the page of a SHA-256 (only the hash is sent).
- Safety: unsafe paths, reserved device names and NTFS streams are refused or renamed, links are never followed or created, the "downloaded from the internet" mark (Mark of the Web) of an archive is passed to every file that comes out of it (also when dragged out), unusually large extractions are asked about (also when an archive lies about its sizes), files that run code are asked about before they are opened, archives can be scanned by the installed antivirus (AMSI), and only the bundled, checked `7z.dll` is loaded.
- Windows integration: File Explorer context menu (classic and, in the Microsoft Store version, the new Windows 11 menu), right-drag menu, file types (for the current user only, and only when you switch them on). A welcome window at the first start sets the look, the views and the integration.
- Three ways to install: the Microsoft Store, an installer (per user, no administrator rights, can update itself with a signed update) and zip / portable packages; winget and Scoop. When the Store version is installed it provides the menus and file types, and the other copies switch their own off.
- Themes (light, dark, high contrast), accent colours, sounds, short animations (they follow the Windows setting), English, Traditional Chinese and Simplified Chinese.
- A command-line tool, `lycanzip-cli.exe` (create, list, extract, test, change, hashes, checksums, scan).
- Nothing is collected. The only network request is an optional check for a new version, off by default.
- Built on the 7-Zip 26.05 engine and the Windows App SDK 2.5.
