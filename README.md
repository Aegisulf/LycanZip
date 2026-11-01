<div align="center">

<img src="assets/design/logo-icon.svg" alt="" width="96">

# LycanZip

**A light, free and open source file archiver for Windows 10 and 11.**
No ads, no bundled software, no telemetry.

[![Latest release](https://img.shields.io/github/v/release/Aegisulf/LycanZip)](https://github.com/Aegisulf/LycanZip/releases/latest)
[![Build](https://github.com/Aegisulf/LycanZip/actions/workflows/ci.yml/badge.svg)](https://github.com/Aegisulf/LycanZip/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue)](LICENSE)

[Web site](https://aegisulf.com/lycanzip/) · [Download](https://aegisulf.com/lycanzip/download/) · [Microsoft Store](https://apps.microsoft.com/detail/9PB97QFZM56W) · [Changelog](CHANGELOG.md)

</div>

| Home | Browsing an archive |
|---|---|
| ![Home](assets/screenshots/1.0-home.webp) | ![Details view](assets/screenshots/1.0-details.webp) |
| **Picture preview** | **Creating an archive** |
| ![Picture preview](assets/screenshots/1.0-preview.webp) | ![Compress](assets/screenshots/1.0-compress.webp) |

## Why LycanZip

- **Everything 7-Zip opens, in a modern window.** The packing and unpacking is done by the [7-Zip](https://www.7-zip.org/) engine; the interface is WinUI 3, in eight languages.
- **Old archives with readable names.** File names in Big5, GBK, Shift-JIS and EUC-KR are detected automatically, and an old zip or tar can be upgraded to UTF-8 names.
- **Look inside without unpacking.** Pictures, text, PDF and audio in a preview pane; a picture viewer and an audio player; a reading mode for comic books.
- **Careful with what comes out.** Unsafe paths are refused, links are not followed, decompression bombs are caught, and the "downloaded from the internet" mark is passed on to every file.
- **Yours.** Nothing is collected. The only network request is an optional check for a new version, off by default.

## Get LycanZip

| | |
|---|---|
| **[Microsoft Store](https://apps.microsoft.com/detail/9PB97QFZM56W)** | Updated by Windows, and the only version with entries in the new top-level Windows 11 menu (Windows lets only a signed package add them). |
| **[Installer](https://aegisulf.com/lycanzip/download/)** | One click, per user, no administrator rights; can update itself (off until you switch it on). |
| **[Zip / portable](https://aegisulf.com/lycanzip/download/)** | Unpack and run; the portable zip keeps its settings next to the program (a USB stick). |

Windows 10 version 1809 or newer, or Windows 11; 64-bit. Nothing else to install.

Only the Store version is code-signed (by the Store); for the others, check the SHA-256 shown on the download page against your download. One copy is enough: when the Store version is installed it provides the right-click menu and the file types, and the other versions switch their own off.

## Features

**Open and browse**
- zip, 7z, rar (extract only), tar, gz, xz, bz2, zst, iso, cab and everything else the 7-Zip engine reads, including split archives (`.7z.001`, `.part1.rar`, `.z01`) and double-layer formats such as `.tar.gz` (opened as one tar).
- Folder tree, breadcrumb, details / list / icon views, search, check boxes, band selection.
- Preview pane under the folder tree (pictures, text, PDF, audio), with a larger window that follows the selection.
- Picture preview with a thumbnail strip, EXIF orientation and animated GIF / WebP; pictures can be saved in another format and set as the desktop picture. Audio player with tags and technical information.
- Reading mode: a whole book in order, two pages side by side, right to left, the page remembered per book; `.cbz`, `.cbr`, `.cb7` and `.cbt` open in it.
- File names of old zip / tar archives are detected automatically, and you can choose the encoding yourself.

**Create and change**
- zip, 7z, tar, tar.gz, tar.xz, tar.bz2 and iso; templates, levels, solid blocks, split volumes, AES-256, encrypted file names in 7z, checksum files written next to the archive.
- Add, delete, rename and replace items inside zip, 7z and tar archives, with a safe temporary copy and an optional backup.
- Convert an archive to another format, one or several at once; the new archive is tested and the original stays until you delete it.
- Upgrade an old zip or tar to UTF-8 file names, after a preview of how they will look; the data is copied as it is.

**Extract**
- Smart extraction (one root folder goes in directly, several items get a folder named like the archive), conflict handling per file, extract only the selection.
- Drag out to File Explorer, also from solid 7z archives, in one pass.

**Passwords**
- AES-256 zip and 7z with encrypted file names; a wrong password is asked again before anything is extracted.
- Password manager: passwords that worked in this session and saved passwords are tried before you are asked. Saved passwords are encrypted for your Windows account, with an optional master password.
- Remove the password of a zip or 7z archive; the archive is replaced only after the new one passes the integrity test.

**Checksums**
- CRC-32, MD5, SHA-1, SHA-256, SHA-384, SHA-512 for files, folders and the content of archives; verify `.sha256` / `.md5` / `.sfv` files.
- "Look up on VirusTotal" opens the page of the SHA-256 of an archive, or of a file in it, in your browser (only the hash is in the address; nothing is uploaded).

**Safety**
- Unsafe paths (`..`, absolute paths, drive letters, NTFS streams, reserved names) are refused, symbolic links are not followed, decompression bombs are caught, the Mark of the Web of the archive is passed on, and archives can be scanned by the installed antivirus (AMSI).

**Windows integration**
- File Explorer context menu (classic and the new Windows 11 menu), right-drag menu, file types (registered for the current user only, and only when you switch them on).
- Portable mode: a file `LycanZip.portable` next to the exe keeps all settings in the program folder.
- Interface languages: English, Traditional Chinese, Simplified Chinese, Japanese, Korean, German, Spanish, French.
- Themes, accent colours and a search in the settings (Ctrl+F).
- A command line tool, `lycanzip-cli.exe`.

## Command line

```
lycanzip-cli a <archive> <inputs...> [-t zip|7z|tar|tgz|txz|tbz2|iso] [-p pw] [-l 0-9]
           [--profile compatible|balanced|smallest|fastest] [-v 50m] [--encrypt-names] [-y]
lycanzip-cli l <archive>                      lists the content
lycanzip-cli x <archive> [-o dir] [-p pw] [-y|-s|-r] [--smart|--no-smart]
lycanzip-cli t <archive> [-p pw]              tests the integrity
lycanzip-cli u <archive> <operations...>      changes an existing zip / 7z / tar
lycanzip-cli convert <archive> <new archive> [-t zip|7z|tar|tgz|txz|tbz2] [-p pw] [--new-password pw | --no-password]
lycanzip-cli hash <files...> [-a sha256,...]  checksums;  verify <checksum files...> checks them
lycanzip-cli cat <archive> <index> [-p pw]    writes one item to stdout
common option: --encoding auto|utf8|big5|gbk|sjis|kr|oem|<code page>
```

Run `lycanzip-cli` without arguments for the complete list. Without `-o`, `x` extracts smartly next to the archive; with `-o` it puts the content exactly into that folder. `lycanzip-cli info` prints the version and the environment (please attach it to a bug report).

## Build

Requirements: Visual Studio 2022 Build Tools (Desktop development with C++ and the Windows 11 SDK), CMake, Ninja, PowerShell, Python 3 (tests), and a copy of `7z.dll`.

```powershell
.\scripts\check-env.ps1         # checks the tools and tells what is missing
.\scripts\fetch-7zip.ps1        # downloads the 7-Zip interface headers (once); add -Binaries for 7z.dll
.\scripts\build.ps1 -Test       # builds the Core and the CLI and runs the unit tests (Debug by default)
.\build\debug\bin\lycanzip-cli.exe l archive.zip
```

The graphical program (WinUI 3):

```powershell
.\scripts\build-app.ps1 -Config Release   # the first time it restores the NuGet packages (app\restore.ps1), then builds the Core and the app
.\app\bin\Release\LycanZip.exe [archive]
```

The VS Build Tools do not ship the targets that connect the XAML compiler to a native project, so `app\LycanZip.vcxproj` hooks `MarkupCompilePass1/2` into the build itself (the comments there explain why and in which order).

`7z.dll` is looked for in this order: the environment variable `LYCAN_7Z_DLL`, next to the exe, `%ProgramFiles%\7-Zip`, the Scoop install place. The build copies it to the output folder when it finds one.

UI smoke tests (they operate real windows and need an interactive desktop; do not touch the mouse and keyboard while they run): `.\scripts\ui-smoke.ps1`

The installer, the zip packages and the Microsoft Store package are made by the release flow, which is not part of this repository; the builds are on the [releases page](https://github.com/Aegisulf/LycanZip/releases) and in the Store.

### Project layout

```
src/core/     the Core static library: 7z wrapper, archive model, extraction, compression, safety layer, settings
src/cli/      the command line front end
app/          the WinUI 3 graphical program (C++/WinRT + XAML), with its language files in app/Assets/Lang
shellext/     the context-menu DLL of File Explorer (classic menu, right-drag menu, Windows 11 menu)
tests/        unit tests and the end-to-end test of the CLI
scripts/      build and test scripts
tools/        helper scripts (language files, sounds)
assets/       logo and icon sources
```

## Versions

Version numbers follow [semantic versioning](https://semver.org/): the settings files (`settings.ini`, `presets.ini`) and the command line stay compatible within a major version. What changed is in [CHANGELOG.md](CHANGELOG.md).

## License

The code of LycanZip is under the [MIT license](LICENSE). The licenses of 7-Zip and the other parts that are used are in [THIRD_PARTY.md](THIRD_PARTY.md).

## Support

LycanZip is made in spare time and stays free. Reports of problems are welcome in the [issues](https://github.com/Aegisulf/LycanZip/issues); if it saves you time, you can also support the project on [Ko-fi](https://ko-fi.com/aegisulf_lycan).
