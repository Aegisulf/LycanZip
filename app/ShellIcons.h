#pragma once
// The icons Windows shows for a kind of file (the ones of Explorer): by extension, without touching the disk (SHGFI_USEFILEATTRIBUTES).
// Used by the lists and the folder tree when "Windows icons" is on (Settings > View).
#include <cstdint>
#include <string>
#include <vector>

#include <winrt/Microsoft.UI.Xaml.Media.h>

namespace lycan_app {

struct IconPixels {
    int w = 0, h = 0;
    std::vector<uint8_t> bgra;   // premultiplied, top-down
    bool empty() const { return w <= 0 || h <= 0 || bgra.empty(); }
};

// The pixel size to ask Explorer's image list for when something is drawn `dip` big at `scale` (1.0 = 96 dpi): 16, 32, 48 or 256
int ShellIconPx(int dip, double scale);

// "<dir>" for a folder, ".txt" for an extension (lower case), "." for a file without one. Safe on any thread (it starts COM on it).
IconPixels LoadShellIconPixels(const std::wstring& key, int px);

// The picture for XAML (UI thread)
winrt::Microsoft::UI::Xaml::Media::ImageSource MakeIconBitmap(IconPixels const& pixels);

}  // namespace lycan_app
