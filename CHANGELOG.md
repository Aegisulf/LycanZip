# Changelog

All notable changes to LycanZip are listed here, one section per version. Versions follow
[Semantic Versioning](https://semver.org/): from 1.0 on, the settings files (`settings.ini`, `presets.ini`) and the command line
stay compatible within a major version. Release notes are taken from this file.

## 1.1.0 (2026-11-01)

- Five more interface languages: Japanese, Korean, German, Spanish and French. When the interface language differs from the language of Windows, the text is drawn with the fonts of the interface language (for example Japanese kanji forms on a Chinese Windows).
- Convert an archive to another format (More options, or "Convert archive format..." in the File Explorer menu, off by default; several archives at once): the format, template, level and volumes are chosen as for a new archive, the password is kept unless you choose otherwise, and the new archive is tested. The original stays; the finished card offers to delete it.
- Remove the password of a zip or 7z archive: it is packed again without the password and replaces the archive only after the new one passes the integrity test.
- Upgrade an old zip or tar whose file names are in a legacy code page: the names are stored in UTF-8, after a preview of how they will look (the encoding can be changed there); the data is copied as it is.
- New zip archives always store file names outside ASCII in UTF-8 and mark them. Before, a name that fit the code page of the PC (Big5 on a Chinese Windows, for example) was stored in that code page without a mark and looked garbled on a PC with another language.
- Changing an old zip whose names have no UTF-8 mark (adding, deleting, renaming) no longer writes the untouched names again; a tar asks first, because all its names are written again.
- Password manager: the passwords that worked in this session, and saved passwords, are tried before you are asked. Saved passwords are encrypted for your Windows account and managed in a new Passwords tab of the settings (add, edit, import, export), with an optional master password that keeps them locked until it is entered once per run. A new archive can take a saved password.
- Reading mode of the picture preview: a whole book in order across folders, two pages side by side, right to left, the page remembered per book; comic book archives (`.cbz`, `.cbr`, `.cb7`, `.cbt`) open in it.
- Preview pane under the folder tree (pictures, text, PDF, audio), with a larger window that follows the selection; shorter rows in the folder tree.
- Search in the settings (Ctrl+F): finds the setting cards of every tab, also by their English words when the interface is in another language.
- Context menus no longer keep a sizing or busy cursor when they open under a mouse that does not move.

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
- Three ways to install: the Microsoft Store, an installer (per user, no administrator rights, can update itself with a signed update) and zip / portable packages; winget. When the Store version is installed it provides the menus and file types, and the other copies switch their own off.
- Themes (light, dark, high contrast), accent colours, sounds, short animations (they follow the Windows setting), English, Traditional Chinese and Simplified Chinese.
- A command-line tool, `lycanzip-cli.exe` (create, list, extract, test, change, hashes, checksums, scan).
- Nothing is collected. The only network request is an optional check for a new version, off by default.
- Built on the 7-Zip 26.05 engine and the Windows App SDK 2.5.
