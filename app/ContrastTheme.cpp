#include "pch.h"
#include "ContrastTheme.h"

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "HighContrast.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using Microsoft::UI::Xaml::Media::SolidColorBrush;

namespace lycan_app {
namespace {

Windows::UI::Color Rgb(uint32_t v, uint8_t a = 255) {
    return Windows::UI::Color{a, static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v)};
}

constexpr uint32_t kBlack = 0x000000, kWhite = 0xFFFFFF, kHover = 0x2B2B2B, kPressed = 0x454545, kDisabled = 0x3FF23F, kSelect = 0x1AEBFF,
                   kLink = 0xFFFF00;

bool Has(std::wstring const& n, const wchar_t* part) { return n.find(part) != std::wstring::npos; }
bool Starts(std::wstring const& n, const wchar_t* part) { return n.rfind(part, 0) == 0; }

// The colour a brush gets from its name (the names are those of WinUI's theme resources); nullopt = leave it as it is
std::optional<Windows::UI::Color> ColorFor(std::wstring const& n, Windows::UI::Color orig) {
    if (n.size() < 6 || n.compare(n.size() - 5, 5, L"Brush") != 0) return std::nullopt;
    if (Has(n, L"Smoke")) return std::nullopt;   // the dim layer behind a dialog
    const bool disabled = Has(n, L"Disabled");
    const bool secondary = Has(n, L"Secondary") || Has(n, L"Hover"), tertiary = Has(n, L"Tertiary") || Has(n, L"Quarternary") || Has(n, L"Pressed");

    // accent
    if (Starts(n, L"TextOnAccentFillColor")) return Rgb(disabled ? 0x303030 : kBlack);
    if (Starts(n, L"AccentTextFillColor") || Starts(n, L"SystemControlForegroundAccent")) return Rgb(disabled ? kDisabled : kLink);
    if (Starts(n, L"AccentFillColor") || Starts(n, L"SystemControlHighlightAccent") || Starts(n, L"SystemAccent")) return Rgb(disabled ? 0x404040 : kSelect);
    // focus
    if (Starts(n, L"FocusStrokeColorOuter")) return Rgb(kLink);
    if (Starts(n, L"FocusStrokeColorInner")) return Rgb(kBlack);
    // text
    if (Starts(n, L"TextFillColorInverse")) return Rgb(kBlack);
    if (Starts(n, L"TextFillColor")) return Rgb(disabled ? kDisabled : kWhite);
    // lines
    if (Has(n, L"StrokeColor")) return Rgb(disabled ? kDisabled : kWhite);
    // strong fills (the glyph of a check box, the thumb of a slider)
    if (Starts(n, L"ControlStrongFillColor")) return Rgb(disabled ? kDisabled : kWhite);
    // plain fills of the surfaces and the controls
    if (Has(n, L"FillColor")) {
        if (orig.A == 0 && !Has(n, L"Acrylic")) return std::nullopt;   // a transparent one stays transparent
        if (Starts(n, L"SystemFillColor")) return std::nullopt;       // the colours of the info bars
        if (disabled) return Rgb(kBlack);
        if (tertiary) return Rgb(kPressed);
        if (secondary) return Rgb(kHover);
        return Rgb(kBlack);
    }
    // the older names
    if (Starts(n, L"SystemControl")) {
        if (Has(n, L"Foreground")) return Rgb(disabled ? kDisabled : kWhite);
        if (Has(n, L"Highlight") && (Has(n, L"List") || Has(n, L"Low"))) return Rgb(tertiary ? kPressed : kHover);
        if (Has(n, L"Background")) return Rgb(kBlack);
        if (Has(n, L"Border")) return Rgb(kWhite);
    }
    if (n == L"ApplicationPageBackgroundThemeBrush" || n == L"LayerFillColorDefaultBrush") return Rgb(kBlack);
    return std::nullopt;
}

// Two ways to change a brush: set its colour in place (the controls that use it change at once), and where that is not allowed (some of the
// brushes of WinUI are read only) put a new brush in a theme dictionary of the application itself, which is looked at before the ones of the
// controls. Both are taken back when the look is switched off. The window changes its theme (light, then dark) to make the controls ask for
// their brushes again.
struct Target { SolidColorBrush brush{nullptr}; std::wstring key; Windows::UI::Color color{}; };

void Collect(ResourceDictionary const& dict, std::map<std::wstring, Windows::UI::Color>& plain, std::vector<Target>& targets) {
    for (auto const& kv : dict.ThemeDictionaries()) {
        const auto name = unbox_value_or<hstring>(kv.Key(), L"");
        if (name != L"Default" && name != L"Dark") continue;
        auto sub = kv.Value().try_as<ResourceDictionary>();
        if (!sub) continue;
        for (auto const& e : sub) {
            const std::wstring key{unbox_value_or<hstring>(e.Key(), L"")};
            if (auto b = e.Value().try_as<SolidColorBrush>()) {
                if (auto c = ColorFor(key, b.Color())) { plain[key] = *c; targets.push_back({b, key, *c}); }
            } else if (e.Value().try_as<Microsoft::UI::Xaml::Media::AcrylicBrush>()) {
                // the acrylic and mica-like brushes become plain black (the windows are opaque in this look)
                if (key.size() > 5 && key.compare(key.size() - 5, 5, L"Brush") == 0) plain[key] = Rgb(kBlack);
            }
        }
    }
    for (auto const& m : dict.MergedDictionaries()) Collect(m, plain, targets);
}

struct Original { SolidColorBrush brush{nullptr}; Windows::UI::Color color{}; };
bool g_on = false;
std::vector<Original> g_originals;

void RemoveOverrides(ResourceDictionary const& res) {
    auto themes = res.ThemeDictionaries();
    for (auto const* n : {L"Default", L"Dark"}) {
        const auto key = box_value(hstring{n});
        if (themes.HasKey(key)) themes.Remove(key);
    }
}

}  // namespace

bool ContrastThemeOn() { return g_on; }

void SetContrastTheme(bool on) {
    ForceHighContrast(on);
    auto res = Application::Current().Resources();
    if (on) {
        RemoveOverrides(res);   // look at the controls' own brushes, not ours from the last time
        std::map<std::wstring, Windows::UI::Color> plain;
        std::vector<Target> targets;
        Collect(res, plain, targets);
        std::map<std::wstring, Windows::UI::Color> rest = plain;   // what could not be set in place, and the acrylics
        for (auto const& t : targets) {
            bool known = false;
            for (auto const& o : g_originals) if (o.brush == t.brush) { known = true; break; }
            if (!known) g_originals.push_back({t.brush, t.brush.Color()});
            try { t.brush.Color(t.color); rest.erase(t.key); } catch (hresult_error const&) {}
        }
        for (auto const* n : {L"Default", L"Dark"}) {
            ResourceDictionary d;
            for (auto const& c : rest) d.Insert(box_value(hstring{c.first}), SolidColorBrush{c.second});
            res.ThemeDictionaries().Insert(box_value(hstring{n}), d);
        }
        g_on = true;
    } else if (g_on) {
        RemoveOverrides(res);
        for (auto const& o : g_originals) { try { o.brush.Color(o.color); } catch (hresult_error const&) {} }
        g_originals.clear();
        g_on = false;
    }
}

}  // namespace lycan_app
