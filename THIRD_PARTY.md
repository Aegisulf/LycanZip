# Third-party components

## 7-Zip (Igor Pavlov)

- Use: the compression / extraction engine. Used by **dynamic linking** of `7z.dll` (loaded at run time); at build time only its C++ interface headers are referenced (`third_party/7zip`, downloaded by `scripts/fetch-7zip.ps1`, not under version control).
- Pinned source version now: 26.05 (SHA256 is verified in the script). During development `7z.dll` is the locally installed version (25.01 in the tests now); a release ships the `7z.dll` of the same version, together with the 7-Zip license texts.
- License: GNU LGPL, plus the unRAR license restriction - the RAR unpacking code **must not** be used to rebuild the RAR compression algorithm. LycanZip provides RAR extraction only.
- Source: https://www.7-zip.org/ / https://github.com/ip7z/7zip

> Note: before a release, make sure the LGPL and unRAR notices that ship with it are complete (see `DOC/License.txt` and `DOC/unRarLicense.txt` of 7-Zip).

## Microsoft Visual C++ runtime

The zip packages and the installer contain the Visual C++ runtime DLLs (vcruntime140.dll, msvcp140.dll and related files) from Visual Studio, which Microsoft allows to be shipped with a program (app-local deployment), so that nothing has to be installed first on a clean Windows.
