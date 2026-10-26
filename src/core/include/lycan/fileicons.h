#pragma once
// The file-type icons: archive files are shown with one of seven icon families (ZIP, 7Z, RAR, TAR, ISO, other, comic book),
// in Explorer (the program ids of the file associations and the MSIX file types) and inside the program.
// The icons live in the program's resources (app/LycanZip.rc, IDs 101..107) and as PNG files in Assets\FileIcons.
#include <string>
#include <string_view>

namespace lycan {

enum class IconFamily { Zip, SevenZip, Rar, Tar, Iso, Other, Comic };   // Comic last: the ids of the others stay

// Family of a file extension (with or without the dot, any case); unknown extensions are Other.
IconFamily IconFamilyForExtension(std::wstring_view ext);
// Family of a file name or path (its last extension: "a.tar.gz" is gz, so Tar).
IconFamily IconFamilyForPath(std::wstring_view path);

const wchar_t* IconFamilyName(IconFamily f);   // "zip" "7z" "rar" "tar" "iso" "arc" "comic": also the base of the .ico / .png file names
int IconResourceId(IconFamily f);              // 101..107: the ICON resource of LycanZip.exe

}  // namespace lycan
