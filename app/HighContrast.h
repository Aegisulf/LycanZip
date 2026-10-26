#pragma once
// High contrast (Windows "Contrast themes"): the controls follow the system themselves; this is for what the program colours itself
// (file category colours, red / green status text, dimmed captions). In high contrast those use the system text colour only,
// because colours that look right on the dark and light themes can vanish on a contrast theme (a yellow folder on a white page).
#include <winrt/Windows.UI.ViewManagement.h>

#include <atomic>
#include <cstdint>
#include <functional>

namespace lycan_app {

namespace detail {
inline std::atomic<int>& HcState() { static std::atomic<int> v{-1}; return v; }   // -1 = not read yet
inline std::atomic<uint32_t>& HcForegroundRgb() { static std::atomic<uint32_t> v{0xFFFFFF}; return v; }
inline void ReadHc() {
    try {
        winrt::Windows::UI::ViewManagement::AccessibilitySettings a;
        winrt::Windows::UI::ViewManagement::UISettings ui;
        auto c = ui.GetColorValue(winrt::Windows::UI::ViewManagement::UIColorType::Foreground);
        HcForegroundRgb() = (static_cast<uint32_t>(c.R) << 16) | (static_cast<uint32_t>(c.G) << 8) | c.B;
        HcState() = a.HighContrast() ? 1 : 0;
    } catch (...) { HcState() = 0; }
}
}  // namespace detail

inline std::atomic<bool>& HcForced() { static std::atomic<bool> v{false}; return v; }   // the program's own contrast theme (settings)
inline void ForceHighContrast(bool on) { HcForced() = on; }

inline bool HighContrast() {
    if (HcForced()) return true;
    if (detail::HcState() < 0) detail::ReadHc();
    return detail::HcState() == 1;
}

// The system text colour of the contrast theme (white on a dark theme, black on a light one)
inline winrt::Windows::UI::Color HighContrastForeground() {
    if (HcForced() && detail::HcState() != 1) return winrt::Windows::UI::Color{255, 255, 255, 255};   // our own contrast theme is white on black
    HighContrast();
    const uint32_t v = detail::HcForegroundRgb();
    return winrt::Windows::UI::Color{255, static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v)};
}

// `normal` unless a contrast theme is on, then the system text colour
inline winrt::Windows::UI::Color StatusColor(winrt::Windows::UI::Color normal) { return HighContrast() ? HighContrastForeground() : normal; }

// Tells `onChange` (on some thread) when a contrast theme is switched on or off; the cached state is refreshed first. Keep the result alive.
inline winrt::Windows::UI::ViewManagement::AccessibilitySettings WatchHighContrast(std::function<void()> onChange) {
    winrt::Windows::UI::ViewManagement::AccessibilitySettings a;
    try {
        a.HighContrastChanged([onChange](auto&&, auto&&) {
            detail::ReadHc();
            if (onChange) onChange();
        });
    } catch (...) {}   // no notification (an unpackaged app may not get it): the state is read when it is asked for
    return a;
}

}  // namespace lycan_app
