#pragma once
// The file-type icons of Assets\FileIcons as XAML images (lycan/fileicons.h): one shared BitmapImage per family and size
// (32 px for lists and the breadcrumb, 64 px for the small cells of the icon view, 256 px for the bigger ones), null when the file is missing. UI thread only.
#include <filesystem>
#include <string>
#include <vector>

#include <windows.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>

#include "lycan/fileicons.h"

namespace lycan_app {

inline winrt::Microsoft::UI::Xaml::Media::Imaging::BitmapImage FileIconImage(lycan::IconFamily family, int px = 32)
{
    using winrt::Microsoft::UI::Xaml::Media::Imaging::BitmapImage;
    // NOTE: a C++/WinRT array element that is not given nullptr is a real (empty) object, so `if (cache[..])` would be true and return a picture with no source
    static std::vector<BitmapImage> cache(7 * 3, nullptr);
    static bool missing[7][3] = {};
    const size_t f = static_cast<size_t>(family), s = px > 64 ? 2 : px >= 64 ? 1 : 0;
    if (f >= 7) return nullptr;
    if (cache[f * 3 + s]) return cache[f * 3 + s];
    if (missing[f][s]) return nullptr;
    wchar_t exe[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    const std::filesystem::path p = std::filesystem::path(std::wstring(exe, n)).parent_path() / L"Assets" / L"FileIcons" /
                                    (std::wstring(lycan::IconFamilyName(family)) + (s == 2 ? L"-256.png" : s == 1 ? L"-64.png" : L"-32.png"));
    std::error_code ec;
    if (!std::filesystem::exists(p, ec)) { missing[f][s] = true; return nullptr; }
    cache[f * 3 + s] = BitmapImage{winrt::Windows::Foundation::Uri{winrt::hstring{p.wstring()}}};
    return cache[f * 3 + s];
}

}  // namespace lycan_app
