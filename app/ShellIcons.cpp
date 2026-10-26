#include "pch.h"
#include "ShellIcons.h"

#include <algorithm>
#include <commoncontrols.h>
#include <cstring>
#include <shellapi.h>
#include <shlobj.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.Storage.Streams.h>

namespace lycan_app {

int ShellIconPx(int dip, double scale) {
    const int want = static_cast<int>(dip * scale + 0.5);
    if (want <= 16) return 16;
    if (want <= 32) return 32;
    if (want <= 48) return 48;
    return 256;
}

IconPixels LoadShellIconPixels(const std::wstring& key, int px) {
    IconPixels out;
    struct ComScope {
        bool ok;
        ComScope() : ok(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {}
        ~ComScope() { if (ok) CoUninitialize(); }
    } com;

    const bool folder = key == L"<dir>";
    const std::wstring name = folder ? L"folder" : (key == L"." || key.empty() ? L"file" : L"file" + key);
    SHFILEINFOW fi{};
    if (!SHGetFileInfoW(name.c_str(), folder ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL, &fi, sizeof(fi), SHGFI_USEFILEATTRIBUTES | SHGFI_SYSICONINDEX)) return out;
    const int list = px <= 16 ? SHIL_SMALL : px <= 32 ? SHIL_LARGE : px <= 48 ? SHIL_EXTRALARGE : SHIL_JUMBO;
    IImageList* il = nullptr;
    if (FAILED(SHGetImageList(list, IID_PPV_ARGS(&il))) || !il) return out;
    HICON icon = nullptr;
    const HRESULT hr = il->GetIcon(fi.iIcon, ILD_TRANSPARENT, &icon);
    il->Release();
    if (FAILED(hr) || !icon) return out;

    // the real size of the icon
    int w = px, h = px;
    ICONINFO ii{};
    if (GetIconInfo(icon, &ii)) {
        BITMAP bm{};
        if (ii.hbmColor && GetObjectW(ii.hbmColor, sizeof(bm), &bm)) { w = bm.bmWidth; h = bm.bmHeight; }
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
        if (ii.hbmMask) DeleteObject(ii.hbmMask);
    }
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp && bits) {
        HDC dc = CreateCompatibleDC(nullptr);
        HGDIOBJ old = SelectObject(dc, bmp);
        DrawIconEx(dc, 0, 0, icon, w, h, 0, nullptr, DI_NORMAL);   // onto a cleared surface: an icon with alpha keeps it (premultiplied)
        GdiFlush();
        SelectObject(dc, old);
        DeleteDC(dc);
        out.w = w;
        out.h = h;
        out.bgra.assign(static_cast<uint8_t*>(bits), static_cast<uint8_t*>(bits) + static_cast<size_t>(w) * h * 4);
        // an old icon without an alpha channel leaves the alpha at 0 everywhere: what was drawn is opaque
        bool anyAlpha = false, anyColor = false;
        for (size_t i = 0; i < out.bgra.size(); i += 4) {
            if (out.bgra[i + 3]) { anyAlpha = true; break; }
            if (out.bgra[i] | out.bgra[i + 1] | out.bgra[i + 2]) anyColor = true;
        }
        if (!anyAlpha && anyColor)
            for (size_t i = 0; i < out.bgra.size(); i += 4) if (out.bgra[i] | out.bgra[i + 1] | out.bgra[i + 2]) out.bgra[i + 3] = 255;
    }
    if (bmp) DeleteObject(bmp);
    DestroyIcon(icon);
    // some kinds of file have no 256 px icon: Windows puts the small one in the middle of an empty canvas. Then the 48 px icon (which fills its square) scaled up is better.
    if (px > 48 && !out.empty()) {
        int minX = out.w, maxX = -1;
        for (int y = 0; y < out.h; ++y)
            for (int x = 0; x < out.w; ++x)
                if (out.bgra[(static_cast<size_t>(y) * out.w + x) * 4 + 3] > 8) { minX = std::min(minX, x); maxX = std::max(maxX, x); }
        if (maxX < 0 || (maxX - minX + 1) * 2 < out.w) {
            IconPixels smaller = LoadShellIconPixels(key, 48);
            if (!smaller.empty()) return smaller;
        }
    }
    return out;
}

winrt::Microsoft::UI::Xaml::Media::ImageSource MakeIconBitmap(IconPixels const& p) {
    using winrt::Microsoft::UI::Xaml::Media::Imaging::WriteableBitmap;
    if (p.empty()) return nullptr;
    WriteableBitmap wb(p.w, p.h);
    auto buffer = wb.PixelBuffer();
    std::memcpy(buffer.data(), p.bgra.data(), std::min<size_t>(p.bgra.size(), buffer.Length()));
    wb.Invalidate();
    return wb;
}

}  // namespace lycan_app
