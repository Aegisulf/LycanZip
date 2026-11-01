#include "pch.h"
#include "Welcome.h"

#include <algorithm>
#include <map>
#include <memory>

#include "AltBeep.h"
#include "EntryRow.h"
#include "FileIconImages.h"
#include "ShellIcons.h"
#include "WolfPath.h"
#include "lycan/languages.h"
#include "lycan/diag.h"
#include "lycan/fileassoc.h"
#include "lycan/i18n.h"
#include "lycan/versioncheck.h"

#include <winrt/Windows.UI.ViewManagement.h>

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Media;
using Microsoft::UI::Xaml::Markup::XamlReader;

namespace lycan_app {
namespace {

constexpr const wchar_t* kNs = L"xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation'";

UIElement Load(std::wstring const& markup) { return XamlReader::Load(hstring{markup}).as<UIElement>(); }

// The card of a group of settings (the brushes follow the theme, so the markup carries them)
Border MakeCard() {
    return Load(std::wstring(L"<Border ") + kNs + L" Background='{ThemeResource CardBackgroundFillColorDefaultBrush}' BorderBrush='{ThemeResource CardStrokeColorDefaultBrush}'"
                L" BorderThickness='1' CornerRadius='8' Padding='16,6'/>").as<Border>();
}

UIElement Divider() { return Load(std::wstring(L"<Rectangle ") + kNs + L" Height='1' Fill='{ThemeResource DividerStrokeColorDefaultBrush}' Margin='0,2'/>"); }

TextBlock Label(std::wstring const& text, double size = 14, double opacity = 1.0) {
    TextBlock t;
    t.Text(hstring{text});
    t.FontSize(size);
    t.Opacity(opacity);
    t.TextWrapping(TextWrapping::Wrap);
    return t;
}

// One line of a card: the title (and a hint under it) on the left, the control on the right
Grid Row(std::wstring const& title, std::wstring const& hint, FrameworkElement const& control) {
    Grid g;
    g.Padding(ThicknessHelper::FromLengths(0, 8, 0, 8));
    g.ColumnSpacing(16);
    ColumnDefinition c0, c1;
    c0.Width(GridLength{1, GridUnitType::Star});
    c1.Width(GridLength{0, GridUnitType::Auto});
    g.ColumnDefinitions().Append(c0);
    g.ColumnDefinitions().Append(c1);
    StackPanel left;
    left.VerticalAlignment(VerticalAlignment::Center);
    left.Children().Append(Label(title));
    if (!hint.empty()) left.Children().Append(Label(hint, 12, 0.65));
    g.Children().Append(left);
    control.VerticalAlignment(VerticalAlignment::Center);
    Grid::SetColumn(control, 1);
    g.Children().Append(control);
    return g;
}

ToggleSwitch Toggle(bool on, std::wstring const& automationName) {
    ToggleSwitch t;
    t.OnContent(box_value(L""));
    t.OffContent(box_value(L""));
    t.MinWidth(0);
    t.IsOn(on);
    Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(t, hstring{automationName});
    return t;
}

RadioButtons Choice(std::initializer_list<std::wstring> items, int selected, int columns, std::wstring const& automationName) {
    RadioButtons r;
    r.MaxColumns(columns);
    for (auto const& s : items) r.Items().Append(box_value(hstring{s}));
    r.SelectedIndex(selected);
    Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(r, hstring{automationName});
    return r;
}

class WelcomeWindow : public std::enable_shared_from_this<WelcomeWindow> {
public:
    void Create(WelcomeHost host) {
        host_ = std::move(host);
        s_ = host_.settings;
        win_ = Window{};
        win_.Title(hstring{lycan::Tr(L"welcome.welcome_lycanzip")});
        win_.SystemBackdrop(Microsoft::UI::Xaml::Media::MicaBackdrop{});
        Build();
        if (auto tag = lycan::i18n::UiLanguageTag(); !tag.empty()) root_.Language(hstring{tag});  // the font fallback picks the fonts by the language
        win_.Content(root_);
        win_.Closed([weak = weak_from_this()](auto&&, auto&&) { if (auto s = weak.lock()) s->OnClosed(); });

        HWND h{};
        check_hresult(win_.as<::IWindowNative>()->get_WindowHandle(&h));
        const double scale = GetDpiForWindow(h) / 96.0;
        auto app = win_.AppWindow();
        app.Resize(Windows::Graphics::SizeInt32{static_cast<int32_t>(720 * scale), static_cast<int32_t>(860 * scale)});
        if (auto p = app.Presenter().try_as<Microsoft::UI::Windowing::OverlappedPresenter>()) { p.IsResizable(false); p.IsMaximizable(false); }
        // in the middle of the screen it is on
        try {
            auto area = Microsoft::UI::Windowing::DisplayArea::GetFromWindowId(app.Id(), Microsoft::UI::Windowing::DisplayAreaFallback::Nearest);
            auto wa = area.WorkArea();
            auto sz = app.Size();
            if (sz.Height > wa.Height - static_cast<int32_t>(40 * scale)) { sz.Height = wa.Height - static_cast<int32_t>(40 * scale); app.Resize(sz); }   // not taller than the screen: the page scrolls
            app.Move(Windows::Graphics::PointInt32{wa.X + (wa.Width - sz.Width) / 2, wa.Y + (wa.Height - sz.Height) / 2});
        } catch (...) {}
        SuppressAltBeep(h);
        ApplyWindowTheme();
        ShowStep(0);
        win_.Activate();
    }

    void Close() {
        closedByCode_ = true;
        if (win_) win_.Close();
    }
    bool IsClosed() const { return closed_; }

private:
    // ---- the look of the window itself ----
    void ApplyWindowTheme() {
        root_.RequestedTheme(s_.theme == lycan::ThemeMode::Light ? ElementTheme::Light
                             : (s_.theme == lycan::ThemeMode::Dark || s_.theme == lycan::ThemeMode::ContrastBlack) ? ElementTheme::Dark
                                                                                                                  : ElementTheme::Default);
    }

    void Preview(bool sample = false) {
        ApplyWindowTheme();
        ApplyScene();
        if (host_.preview) host_.preview(s_);
        if (sample && host_.playSample) host_.playSample();
    }

    // ---- the picture on top of the first step: a night sky, the moon and the wolf (the home scene in small) ----
    UIElement Illustration() {
        auto g = Load(std::wstring(L"<Grid ") + kNs + L" Height='160' CornerRadius='8'>"
                      L"<Grid.Background><LinearGradientBrush StartPoint='0.1,0' EndPoint='0.9,1'><GradientStop Color='#FF1B2F5E' Offset='0'/>"
                      L"<GradientStop Color='#FF0A1230' Offset='0.6'/><GradientStop Color='#FF070D1F' Offset='1'/></LinearGradientBrush></Grid.Background>"
                      L"<Ellipse x:Name='Glow' xmlns:x='http://schemas.microsoft.com/winfx/2006/xaml' Width='170' Height='170' HorizontalAlignment='Left' VerticalAlignment='Top' Margin='266,-48,0,0'>"
                      L"<Ellipse.Fill><RadialGradientBrush><GradientStop Color='#55B8CCFF' Offset='0'/><GradientStop Color='#00B8CCFF' Offset='1'/></RadialGradientBrush></Ellipse.Fill></Ellipse>"
                      L"<Ellipse x:Name='Moon' xmlns:x='http://schemas.microsoft.com/winfx/2006/xaml' Width='56' Height='56' HorizontalAlignment='Left' VerticalAlignment='Top' Margin='323,9,0,0' Fill='#FFD6E4FF'/>"
                      L"<Canvas x:Name='Sky' xmlns:x='http://schemas.microsoft.com/winfx/2006/xaml'/>"
                      L"<Path x:Name='Wolf' xmlns:x='http://schemas.microsoft.com/winfx/2006/xaml' HorizontalAlignment='Right' VerticalAlignment='Bottom' Height='142' Stretch='Uniform' Margin='0,0,18,0' Fill='#FF0A1230' Stroke='#996E8CD8' StrokeThickness='2.5' Data='" LYCAN_WOLF_PATH_DATA L"'/>"
                      L"</Grid>").as<Grid>();
        scene_ = g;
        ApplyScene();
        // stars (fixed places, so the picture is the same every time)
        struct S { double x, y, r, o; };
        static const S stars[] = {{30, 20, 1.6, .9}, {200, 14, 1.2, .7}, {240, 58, 1.5, .85}, {160, 26, 1, .6}, {430, 84, 1.3, .7}, {110, 70, 1, .5}, {60, 56, 1.4, .8},
                                  {410, 24, 1, .5}, {470, 30, 1.4, .8}, {500, 70, 1, .6}, {300, 100, 1, .6}};
        for (auto const& st : stars) {
            Microsoft::UI::Xaml::Shapes::Ellipse e;
            e.Width(st.r * 2);
            e.Height(st.r * 2);
            e.Opacity(st.o);
            e.Fill(SolidColorBrush{Windows::UI::Color{255, 245, 249, 255}});
            Canvas::SetLeft(e, st.x);
            Canvas::SetTop(e, st.y);
            g.FindName(L"Sky").as<Canvas>().Children().Append(e);
        }
        StackPanel t;
        t.VerticalAlignment(VerticalAlignment::Bottom);
        t.Margin(ThicknessHelper::FromLengths(22, 0, 0, 16));
        auto title = Label(lycan::Tr(L"welcome.welcome_lycanzip"), 26);
        title.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
        title.Foreground(SolidColorBrush{Windows::UI::Color{255, 230, 238, 255}});
        auto sub = Label(lycan::Tr(L"welcome.free_open_source_ad_free_light"), 13);
        sub.Foreground(SolidColorBrush{Windows::UI::Color{255, 190, 205, 240}});
        sub.MaxWidth(330);
        sub.HorizontalAlignment(HorizontalAlignment::Left);
        title.HorizontalAlignment(HorizontalAlignment::Left);
        t.Children().Append(title);
        t.Children().Append(sub);
        g.Children().Append(t);
        return g;
    }

    // The picture follows the accent colour that is chosen (the same recipe as the start page of the main window: the night sky is the accent
    // colour made dark, the moon and the rim of the wolf are made light), so that a change can be seen in this window itself.
    void ApplyScene() {
        if (!scene_) return;
        Windows::UI::Color base{255, 76, 141, 246};
        const size_t n = host_.accents.size();
        const size_t i = static_cast<size_t>(std::clamp(static_cast<int>(s_.accent), 0, static_cast<int>(n) - 1));
        if (n && host_.accents[i].followSystem) {
            try { base = Windows::UI::ViewManagement::UISettings{}.GetColorValue(Windows::UI::ViewManagement::UIColorType::Accent); } catch (...) {}
        } else if (n) base = host_.accents[i].color;
        auto mix = [&](double toward, double t, uint8_t a = 255) {
            auto m = [&](uint8_t c) { return static_cast<uint8_t>(c + (toward - c) * t + 0.5); };
            return Windows::UI::Color{a, m(base.R), m(base.G), m(base.B)};
        };
        LinearGradientBrush sky;
        sky.StartPoint(Windows::Foundation::Point{0.1f, 0.f});
        sky.EndPoint(Windows::Foundation::Point{0.9f, 1.f});
        for (auto const& [c, off] : {std::pair{mix(0, 0.60), 0.0}, std::pair{mix(0, 0.82), 0.6}, std::pair{mix(0, 0.92), 1.0}}) {
            GradientStop gs;
            gs.Color(c);
            gs.Offset(off);
            sky.GradientStops().Append(gs);
        }
        scene_.Background(sky);
        RadialGradientBrush glow;
        {
            GradientStop a, b;
            a.Color(mix(255, 0.55, 0x66));
            a.Offset(0);
            b.Color(mix(255, 0.55, 0));
            b.Offset(1);
            glow.GradientStops().Append(a);
            glow.GradientStops().Append(b);
        }
        scene_.FindName(L"Glow").as<Microsoft::UI::Xaml::Shapes::Ellipse>().Fill(glow);
        scene_.FindName(L"Moon").as<Microsoft::UI::Xaml::Shapes::Ellipse>().Fill(SolidColorBrush{mix(255, 0.82)});
        auto wolf = scene_.FindName(L"Wolf").as<Microsoft::UI::Xaml::Shapes::Path>();
        wolf.Fill(SolidColorBrush{mix(0, 0.90)});
        wolf.Stroke(SolidColorBrush{mix(255, 0.35, 0x99)});
    }

    // ---- step 1: look and sound ----
    StackPanel PageLook() {
        StackPanel page;
        page.Spacing(12);
        page.Children().Append(Illustration());
        auto card = MakeCard();
        StackPanel col;

        langCombo_ = ComboBox{};
        langCombo_.Items().Append(box_value(hstring{lycan::Tr(L"main.follow_system")}));   // item 0; the others are lycan::kLanguages
        for (const auto& l : lycan::kLanguages) langCombo_.Items().Append(box_value(hstring{lycan::Tr(l.nameId)}));
        const int found = lycan::LanguageIndex(s_.language);
        langCombo_.SelectedIndex(found < 0 ? 0 : found + 1);
        langCombo_.MinWidth(170);
        Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(langCombo_, hstring{lycan::Tr(L"main.language")});
        langCombo_.SelectionChanged([weak = weak_from_this()](auto&&, auto&&) {
            if (auto s = weak.lock()) {
                const int sel = std::clamp(s->langCombo_.SelectedIndex(), 0, lycan::kLanguageCount);
                s->s_.language = sel == 0 ? L"auto" : lycan::kLanguages[sel - 1].code;
            }
        });
        col.Children().Append(Row(lycan::Tr(L"main.language"), lycan::Tr(L"welcome.language_changes_after_restart"), langCombo_));
        col.Children().Append(Divider());

        themeChoice_ = Choice({lycan::Tr(L"main.follow_system"), lycan::Tr(L"main.light"), lycan::Tr(L"main.dark"), lycan::Tr(L"main.high_contrast_black")}, static_cast<int>(s_.theme), 2, lycan::Tr(L"main.theme"));   // two columns: "High contrast (black)" does not fit in a column of four
        themeChoice_.SelectionChanged([weak = weak_from_this()](auto&&, auto&&) {
            if (auto s = weak.lock()) { s->s_.theme = static_cast<lycan::ThemeMode>(std::clamp(s->themeChoice_.SelectedIndex(), 0, 3)); s->Preview(); }
        });
        col.Children().Append(Row(lycan::Tr(L"main.theme"), L"", themeChoice_));
        col.Children().Append(Divider());

        accentRow_ = StackPanel{};
        accentRow_.Orientation(Orientation::Horizontal);
        accentRow_.Spacing(6);
        for (size_t i = 0; i < host_.accents.size(); ++i) {
            Button b;
            b.Padding(ThicknessHelper::FromUniformLength(3));
            b.Background(SolidColorBrush{Windows::UI::Color{0, 0, 0, 0}});
            b.BorderThickness(ThicknessHelper::FromUniformLength(2));
            b.CornerRadius(CornerRadiusHelper::FromUniformRadius(20));
            Microsoft::UI::Xaml::Shapes::Ellipse dot;
            dot.Width(22);
            dot.Height(22);
            if (host_.accents[i].followSystem) {
                dot.Fill(SolidColorBrush{Windows::UI::Color{255, 120, 120, 128}});
                dot.Stroke(SolidColorBrush{Windows::UI::Color{255, 220, 220, 230}});
                dot.StrokeThickness(2);
            } else dot.Fill(SolidColorBrush{host_.accents[i].color});
            b.Content(dot);
            Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(b, hstring{host_.accents[i].name});
            Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(b, box_value(hstring{host_.accents[i].name}));
            b.Click([weak = weak_from_this(), i](auto&&, auto&&) {
                if (auto s = weak.lock()) { s->s_.accent = static_cast<lycan::AccentTheme>(i); s->MarkAccent(); s->Preview(); }
            });
            accentRow_.Children().Append(b);
        }
        MarkAccent();
        col.Children().Append(Row(lycan::Tr(L"main.accent_color"), L"", accentRow_));
        col.Children().Append(Divider());

        soundToggle_ = Toggle(s_.soundEffects, lycan::Tr(L"main.sound_effects"));
        soundSet_ = ComboBox{};
        soundSet_.Items().Append(box_value(L"Synth"));
        soundSet_.Items().Append(box_value(L"Wave"));
        soundSet_.SelectedIndex(static_cast<int>(s_.soundSet));
        soundSet_.MinWidth(120);
        Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(soundSet_, hstring{lycan::Tr(L"main.sound_set")});
        auto soundBox = StackPanel{};
        soundBox.Orientation(Orientation::Horizontal);
        soundBox.Spacing(12);
        soundBox.Children().Append(soundSet_);
        soundBox.Children().Append(soundToggle_);
        soundSet_.Visibility(s_.soundEffects ? Visibility::Visible : Visibility::Collapsed);
        soundToggle_.Toggled([weak = weak_from_this()](auto&&, auto&&) {
            if (auto s = weak.lock()) {
                s->s_.soundEffects = s->soundToggle_.IsOn();
                s->soundSet_.Visibility(s->s_.soundEffects ? Visibility::Visible : Visibility::Collapsed);
                s->Preview(s->s_.soundEffects);   // a sample, so that it is no surprise later
            }
        });
        soundSet_.SelectionChanged([weak = weak_from_this()](auto&&, auto&&) {
            if (auto s = weak.lock()) { s->s_.soundSet = static_cast<lycan::SoundSet>(std::clamp(s->soundSet_.SelectedIndex(), 0, 1)); s->Preview(true); }
        });
        col.Children().Append(Row(lycan::Tr(L"main.sound_effects"), lycan::Tr(L"welcome.short_sound_when_task_finishes"), soundBox));
        col.Children().Append(Divider());

        animToggle_ = Toggle(s_.animations, lycan::Tr(L"main.animations"));
        animToggle_.Toggled([weak = weak_from_this()](auto&&, auto&&) {
            if (auto s = weak.lock()) { s->s_.animations = s->animToggle_.IsOn(); s->Preview(); }
        });
        col.Children().Append(Row(lycan::Tr(L"main.animations"), lycan::Tr(L"welcome.when_show_animations_off_windows"), animToggle_));
        card.Child(col);
        page.Children().Append(card);
        return page;
    }

    void MarkAccent() {
        const auto brush = SolidColorBrush{Windows::UI::Color{255, 255, 255, 255}};
        const auto none = SolidColorBrush{Windows::UI::Color{0, 0, 0, 0}};
        for (uint32_t i = 0; i < accentRow_.Children().Size(); ++i)
            accentRow_.Children().GetAt(i).as<Button>().BorderBrush(static_cast<int>(i) == static_cast<int>(s_.accent) ? Brush{brush} : Brush{none});
    }


    // ---- the picture of step 2: a small archive window that follows the choices (view, thumbnail size, icons, tree) ----
    struct Sample { const wchar_t* name; bool dir; int kind; };   // kind: 0 folder, 1 picture (cover), 2 picture (trip), 3 other file, 4 archive
    static constexpr Sample kSamples[] = {{L"photos", true, 0}, {L"cover.jpg", false, 1}, {L"trip.png", false, 2}, {L"notes.txt", false, 3},
                                          {L"docs", true, 0}, {L"report.docx", false, 3}, {L"song.mp3", false, 3}, {L"backup.7z", false, 4}};

    UIElement SampleIcon(Sample const& s, double dip) {
        const double scale = GetDpiForSystem() / 96.0;
        if (s.kind == 4) {   // an archive inside the archive: the file-type icon of the program
            Image img;
            img.Width(dip);
            img.Height(dip);
            if (auto src = FileIconImage(lycan::IconFamily::SevenZip, dip > 24 ? 64 : 32)) img.Source(src);
            return img;
        }
        std::wstring name = s.name;
        if (s_.shellIcons) {   // the icons of Windows, as in Explorer
            std::wstring key = s.dir ? L"<dir>" : name.substr(name.rfind(L'.'));
            const int px = ShellIconPx(static_cast<int>(dip), scale);
            const std::wstring ck = key + L"#" + std::to_wstring(px);
            static std::map<std::wstring, IconPixels> cache;
            auto it = cache.find(ck);
            if (it == cache.end()) it = cache.emplace(ck, LoadShellIconPixels(key, px)).first;
            if (!it->second.empty()) {
                Image img;
                img.Width(dip);
                img.Height(dip);
                img.Source(MakeIconBitmap(it->second));
                return img;
            }
        }
        // the program's own: a glyph in the colour of its kind
        using namespace winrt::LycanZip::implementation;
        const FileCategory cat = CategoryOfName(name, s.dir, false);
        FontIcon g;
        g.Glyph(GlyphForCategory(cat));
        g.FontSize(dip * 0.85);
        g.Foreground(BrushForCategory(cat));
        g.HorizontalAlignment(HorizontalAlignment::Center);
        g.VerticalAlignment(VerticalAlignment::Center);
        Grid box;
        box.Width(dip);
        box.Height(dip);
        box.Children().Append(g);
        return box;
    }

    UIElement Thumb(int which, double w, double h) {
        const wchar_t* c0 = which == 1 ? L"#FF5BA8E8" : L"#FFE8A05B";
        const wchar_t* c1 = which == 1 ? L"#FF2D5C9E" : L"#FFB04A3A";
        auto b = Load(std::wstring(L"<Border ") + kNs + L" CornerRadius='3' Width='" + std::to_wstring(w) + L"' Height='" + std::to_wstring(h) +
                      L"'><Border.Background><LinearGradientBrush StartPoint='0,0' EndPoint='0.6,1'><GradientStop Color='" + c0 + L"' Offset='0'/><GradientStop Color='" + c1 +
                      L"' Offset='1'/></LinearGradientBrush></Border.Background></Border>").as<Border>();
        return b;
    }

    void RefreshPreview() {
        if (!previewFrame_) return;
        Grid root;
        root.ColumnSpacing(0);
        ColumnDefinition c0, c1;
        const bool left = s_.showFolderTree || s_.previewPane;   // the left column: the tree and / or the preview pane
        c0.Width(left ? GridLength{128, GridUnitType::Pixel} : GridLength{0, GridUnitType::Pixel});
        c1.Width(GridLength{1, GridUnitType::Star});
        root.ColumnDefinitions().Append(c0);
        root.ColumnDefinitions().Append(c1);

        if (s_.previewPane) {   // under the tree (or alone): the selected picture
            StackPanel pane;
            pane.Spacing(3);
            pane.Padding(ThicknessHelper::FromLengths(6, 6, 6, 4));
            pane.Margin(ThicknessHelper::FromLengths(6, 0, 2, 6));
            pane.VerticalAlignment(s_.showFolderTree ? VerticalAlignment::Bottom : VerticalAlignment::Center);
            pane.CornerRadius(CornerRadiusHelper::FromUniformRadius(4));
            pane.BorderThickness(ThicknessHelper::FromUniformLength(1));
            pane.BorderBrush(Application::Current().Resources().Lookup(box_value(L"ControlStrokeColorDefaultBrush")).as<Microsoft::UI::Xaml::Media::Brush>());
            auto pic = Thumb(1, 104, 64);
            pic.as<FrameworkElement>().HorizontalAlignment(HorizontalAlignment::Center);
            pane.Children().Append(pic);
            auto t = Label(kSamples[1].name, 10, 0.75);
            t.HorizontalAlignment(HorizontalAlignment::Center);
            pane.Children().Append(t);
            Grid::SetColumn(pane, 0);
            root.Children().Append(pane);
        }

        if (s_.showFolderTree) {
            StackPanel tree;
            tree.Padding(ThicknessHelper::FromLengths(8, 8, 4, 4));
            tree.Spacing(4);
            auto line = [&](UIElement const& icon, std::wstring const& text, double indent, bool bold) {
                StackPanel row;
                row.Orientation(Orientation::Horizontal);
                row.Spacing(5);
                row.Margin(ThicknessHelper::FromLengths(indent, 0, 0, 0));
                row.Children().Append(icon);
                auto t = Label(text, 11);
                t.VerticalAlignment(VerticalAlignment::Center);
                if (bold) t.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
                row.Children().Append(t);
                return row;
            };
            Image zip;
            zip.Width(16);
            zip.Height(16);
            if (auto src = FileIconImage(lycan::IconFamily::Zip, 32)) zip.Source(src);
            tree.Children().Append(line(zip, L"sample.zip", 0, true));
            tree.Children().Append(line(SampleIcon(kSamples[0], 16), kSamples[0].name, 14, false));
            tree.Children().Append(line(SampleIcon(kSamples[4], 16), kSamples[4].name, 14, false));
            Grid::SetColumn(tree, 0);
            root.Children().Append(tree);
        }

        StackPanel area;
        area.Padding(ThicknessHelper::FromLengths(10, 8, 8, 4));
        area.Spacing(2);
        Grid::SetColumn(area, 1);
        auto crumb = Label(L"sample.zip  ›", 11, 0.7);
        crumb.Margin(ThicknessHelper::FromLengths(0, 0, 0, 4));
        area.Children().Append(crumb);

        auto fileRow = [&](Sample const& s, double iconDip) {
            Grid g;
            g.ColumnSpacing(6);
            ColumnDefinition a0, a1, a2;
            a0.Width(GridLength{iconDip + 2, GridUnitType::Pixel});
            a1.Width(GridLength{1, GridUnitType::Star});
            a2.Width(GridLength{60, GridUnitType::Pixel});
            g.ColumnDefinitions().Append(a0);
            g.ColumnDefinitions().Append(a1);
            g.ColumnDefinitions().Append(a2);
            auto ic = SampleIcon(s, iconDip);
            g.Children().Append(ic);
            auto n = Label(s.name, 11.5);
            n.VerticalAlignment(VerticalAlignment::Center);
            Grid::SetColumn(n, 1);
            g.Children().Append(n);
            return std::pair<Grid, int>(g, 2);
        };

        if (s_.viewMode == lycan::ViewMode::Details) {
            Grid head;
            ColumnDefinition h0, h1;
            h0.Width(GridLength{1, GridUnitType::Star});
            h1.Width(GridLength{60, GridUnitType::Pixel});
            head.ColumnDefinitions().Append(h0);
            head.ColumnDefinitions().Append(h1);
            auto a = Label(lycan::Tr(L"main.name"), 11, 0.7), b = Label(lycan::Tr(L"main.size"), 11, 0.7);
            Grid::SetColumn(b, 1);
            head.Children().Append(a);
            head.Children().Append(b);
            area.Children().Append(head);
            static const wchar_t* const sizes[] = {L"", L"284 KB", L"1.2 MB", L"3 KB", L"", L"56 KB", L"4.8 MB", L"2.1 MB"};
            for (int i = 0; i < 6; ++i) {
                auto [g, sz] = fileRow(kSamples[i], 16);
                (void)sz;
                auto t = Label(sizes[i], 11, 0.7);
                t.HorizontalAlignment(HorizontalAlignment::Right);
                Grid::SetColumn(t, 2);
                g.Children().Append(t);
                g.Padding(ThicknessHelper::FromLengths(0, 1, 0, 1));
                area.Children().Append(g);
            }
        } else if (s_.viewMode == lycan::ViewMode::List) {
            Grid cols;
            cols.ColumnSpacing(14);
            for (int c = 0; c < 2; ++c) {
                ColumnDefinition cd;
                cd.Width(GridLength{1, GridUnitType::Star});
                cols.ColumnDefinitions().Append(cd);
                StackPanel col;
                col.Spacing(2);
                for (int i = c * 4; i < c * 4 + 4; ++i) {
                    StackPanel row;
                    row.Orientation(Orientation::Horizontal);
                    row.Spacing(6);
                    row.Children().Append(SampleIcon(kSamples[i], 16));
                    auto t = Label(kSamples[i].name, 11.5);
                    t.VerticalAlignment(VerticalAlignment::Center);
                    row.Children().Append(t);
                    col.Children().Append(row);
                }
                Grid::SetColumn(col, c);
                cols.Children().Append(col);
            }
            area.Children().Append(cols);
        } else {   // icons: with thumbnails of the pictures, in the size that was chosen
            const double th[] = {40, 64, 96, 140}, cell[] = {70, 92, 124, 176};
            const int sz = static_cast<int>(std::clamp(s_.thumbSize, 0u, 3u));
            const double tw = th[sz], tHeight = th[sz] * 0.75, cw = cell[sz], ch = tHeight + 24;
            const double areaW = (left ? 640.0 - 128 : 640.0) - 24;
            const int perRow = (std::max)(1, static_cast<int>(areaW / cw));
            const int rows = (std::max)(1, static_cast<int>(150 / ch));
            const int count = (std::min)(8, perRow * rows);
            auto wrap = Load(std::wstring(L"<ItemsControl ") + kNs + L"><ItemsControl.ItemsPanel><ItemsPanelTemplate><VariableSizedWrapGrid Orientation='Horizontal' ItemWidth='" +
                             std::to_wstring(cw) + L"' ItemHeight='" + std::to_wstring(ch) + L"' MaximumRowsOrColumns='" + std::to_wstring(perRow) +
                             L"'/></ItemsPanelTemplate></ItemsControl.ItemsPanel></ItemsControl>").as<ItemsControl>();
            for (int i = 0; i < count; ++i) {
                const Sample& s = kSamples[i];
                StackPanel cellBox;
                cellBox.HorizontalAlignment(HorizontalAlignment::Center);
                cellBox.Spacing(3);
                UIElement pic = (s.kind == 1 || s.kind == 2) ? Thumb(s.kind, tw, tHeight) : SampleIcon(s, std::round(tHeight * 0.9));
                Grid slot;
                slot.Width(tw);
                slot.Height(tHeight);
                if (auto fe = pic.try_as<FrameworkElement>()) { fe.HorizontalAlignment(HorizontalAlignment::Center); fe.VerticalAlignment(VerticalAlignment::Bottom); }
                slot.Children().Append(pic);
                cellBox.Children().Append(slot);
                auto t = Label(s.name, 10.5);
                t.TextTrimming(TextTrimming::CharacterEllipsis);
                t.MaxWidth(cw - 8);
                t.TextAlignment(TextAlignment::Center);
                t.HorizontalAlignment(HorizontalAlignment::Center);
                cellBox.Children().Append(t);
                wrap.Items().Append(cellBox);
            }
            area.Children().Append(wrap);
        }
        root.Children().Append(area);
        previewFrame_.Child(root);

        // what a double-click does
        std::wstring note = s_.doubleClickPreview ? lycan::Tr(L"welcome.double_click_picture_opens_preview") : lycan::Tr(L"welcome.double_click_picture_opens");
        note += std::wstring{lycan::Tr(L"fmt.gap")} + lycan::Tr(L"fmt.gap");
        note += s_.doubleClickAudioPreview ? lycan::Tr(L"welcome.double_click_audio_file_plays") : lycan::Tr(L"welcome.double_click_audio_file_opens");
        previewNote_.Text(hstring{note});
    }

    // ---- step 2: viewing and previews ----
    StackPanel PageView() {
        StackPanel page;
        page.Spacing(12);
        page.Children().Append(Heading(lycan::Tr(L"welcome.view_preview"), lycan::Tr(L"welcome.how_archive_shown_when_opened")));
        previewFrame_ = Load(std::wstring(L"<Border ") + kNs + L" Background='{ThemeResource LayerFillColorAltBrush}' BorderBrush='{ThemeResource CardStrokeColorDefaultBrush}'"
                             L" BorderThickness='1' CornerRadius='8' Height='190'/>").as<Border>();
        page.Children().Append(previewFrame_);
        previewNote_ = Label(L"", 12, 0.7);
        previewNote_.Margin(ThicknessHelper::FromLengths(4, 0, 0, 0));
        page.Children().Append(previewNote_);
        auto card = MakeCard();
        StackPanel col;

        // one choice, like the View menu: details, list, then the icon view in its four sizes
        const int viewIndex = s_.viewMode == lycan::ViewMode::Icons ? 2 + static_cast<int>(std::min(s_.thumbSize, 3u)) : static_cast<int>(s_.viewMode);
        viewChoice_ = Choice({lycan::Tr(L"main.details"), lycan::Tr(L"main.list"), lycan::Tr(L"main.view_small_icons"), lycan::Tr(L"main.view_medium_icons"),
                              lycan::Tr(L"main.view_large_icons"), lycan::Tr(L"main.view_huge_icons")}, viewIndex, 2, lycan::Tr(L"main.default_view"));
        col.Children().Append(Row(lycan::Tr(L"main.default_view"), lycan::Tr(L"welcome.pictures_show_thumbnails_other"), viewChoice_));
        col.Children().Append(Divider());
        viewChoice_.SelectionChanged([weak = weak_from_this()](auto&&, auto&&) {
            if (auto s = weak.lock()) {
                const int i = std::clamp(s->viewChoice_.SelectedIndex(), 0, 5);
                s->s_.viewMode = i < 2 ? static_cast<lycan::ViewMode>(i) : lycan::ViewMode::Icons;
                if (i >= 2) { s->s_.thumbSize = static_cast<unsigned>(i - 2); s->s_.iconThumbnails = true; }
                s->RefreshPreview();
            }
        });

        auto add = [&](std::wstring const& title, std::wstring const& hint, bool on, std::function<void(lycan::Settings&, bool)> apply, bool divider = true) {
            auto t = Toggle(on, title);
            t.Toggled([weak = weak_from_this(), t, apply](auto&&, auto&&) { if (auto s = weak.lock()) { apply(s->s_, t.IsOn()); s->RefreshPreview(); } });
            col.Children().Append(Row(title, hint, t));
            if (divider) col.Children().Append(Divider());
        };
        add(lycan::Tr(L"main.use_file_icons_windows"), L"", s_.shellIcons, [](lycan::Settings& s, bool v) { s.shellIcons = v; });
        add(lycan::Tr(L"main.show_folder_tree"), L"", s_.showFolderTree, [](lycan::Settings& s, bool v) { s.showFolderTree = v; });
        add(lycan::Tr(L"main.show_preview_pane"), lycan::Tr(L"welcome.preview_pane_hint"), s_.previewPane, [](lycan::Settings& s, bool v) { s.previewPane = v; });

        formatCombo_ = ComboBox{};
        for (auto const* n : {L"ZIP", L"7z", L"TAR.GZ", L"TAR.XZ", L"TAR.BZ2", L"TAR"}) formatCombo_.Items().Append(box_value(hstring{n}));
        formatCombo_.SelectedIndex(FormatIndex(s_.defaultFormat));
        formatCombo_.MinWidth(130);
        Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(formatCombo_, hstring{lycan::Tr(L"welcome.default_format")});
        formatCombo_.SelectionChanged([weak = weak_from_this()](auto&&, auto&&) {
            if (auto s = weak.lock()) s->s_.defaultFormat = FormatAt(s->formatCombo_.SelectedIndex());
        });
        col.Children().Append(Row(lycan::Tr(L"welcome.default_format"), lycan::Tr(L"welcome.zip_works_everywhere_7z_compresses"), formatCombo_));
        col.Children().Append(Divider());

        add(lycan::Tr(L"main.image_preview_heading"), lycan::Tr(L"welcome.double_click_picture_archive_open"), s_.doubleClickPreview, [](lycan::Settings& s, bool v) { s.doubleClickPreview = v; });
        add(lycan::Tr(L"main.audio_preview"), lycan::Tr(L"welcome.double_click_audio_file_archive"), s_.doubleClickAudioPreview, [](lycan::Settings& s, bool v) { s.doubleClickAudioPreview = v; }, false);
        card.Child(col);
        page.Children().Append(card);
        RefreshPreview();
        return page;
    }

    static int FormatIndex(lycan::ArchiveType t) {
        switch (t) {
            case lycan::ArchiveType::SevenZip: return 1;
            case lycan::ArchiveType::TarGz: return 2;
            case lycan::ArchiveType::TarXz: return 3;
            case lycan::ArchiveType::TarBz2: return 4;
            case lycan::ArchiveType::Tar: return 5;
            default: return 0;
        }
    }
    static lycan::ArchiveType FormatAt(int i) {
        static const lycan::ArchiveType all[] = {lycan::ArchiveType::Zip, lycan::ArchiveType::SevenZip, lycan::ArchiveType::TarGz, lycan::ArchiveType::TarXz,
                                                 lycan::ArchiveType::TarBz2, lycan::ArchiveType::Tar};
        return all[std::clamp(i, 0, 5)];
    }

    // ---- step 3: Windows ----
    StackPanel PageWindows() {
        StackPanel page;
        page.Spacing(12);
        page.Children().Append(Heading(lycan::Tr(L"welcome.integrate_with_windows"), lycan::Tr(L"welcome.only_step_changes_system_nothing")));
        auto card = MakeCard();
        Grid two;
        two.ColumnSpacing(16);
        ColumnDefinition c0, c1;
        c0.Width(GridLength{1, GridUnitType::Star});
        c1.Width(GridLength{0, GridUnitType::Auto});
        two.ColumnDefinitions().Append(c0);
        two.ColumnDefinitions().Append(c1);
        StackPanel col;

        const bool fixed = !host_.integrationNote.empty();   // the Store version has them built in, or another copy is the Store version
        auto note = [&](std::wstring const& normal) { return fixed ? host_.integrationNote : normal; };
        // The Store version: ONE switch for the menus of File Explorer (it is the shell_menu setting and covers the new Windows 11 menu, Show more
        // options and the menu of Windows 10), no row of its own for the Windows 11 menu. Elsewhere the classic menu and the Windows 11 menu are two
        // registrations; the Windows 11 row only shows where it can be switched on (Windows 11 and a signed menu package).
        const bool menuSwitch = !host_.menuSwitchNote.empty();
        const std::wstring classicTitle = menuSwitch ? lycan::Tr(L"welcome.file_explorer_menu") : lycan::Tr(L"welcome.classic_right_click_menu");
        classicToggle_ = Toggle(menuSwitch ? host_.settings.shellMenu : fixed ? host_.integrationOn : true, classicTitle);
        classicToggle_.IsEnabled(!fixed || menuSwitch);
        col.Children().Append(Row(classicTitle, menuSwitch ? host_.menuSwitchNote : note(lycan::Tr(L"welcome.right_click_files_compress_extract")), classicToggle_));
        col.Children().Append(Divider());

        win11Toggle_ = Toggle(!menuSwitch && !fixed && host_.win11Possible, lycan::Tr(L"welcome.windows_11_right_click_menu"));
        win11Toggle_.IsEnabled(!menuSwitch && !fixed && host_.win11Possible);
        if (!menuSwitch && host_.win11Possible) {
            col.Children().Append(Row(lycan::Tr(L"welcome.windows_11_right_click_menu"), note(lycan::Tr(L"welcome.visible_without_choosing_show_more")), win11Toggle_));
            col.Children().Append(Divider());
        }

        assocToggle_ = Toggle(fixed ? host_.integrationOn : true, lycan::Tr(L"main.file_associations"));
        assocToggle_.IsEnabled(!fixed);
        col.Children().Append(Row(lycan::Tr(L"welcome.file_associations_open_with"), note(lycan::Tr(L"welcome.only_lists_lycanzip_under_open")), assocToggle_));
        // the extensions
        size_t n = 0;
        auto exts = lycan::AssociatedExtensions(&n);
        std::wstring all, some;
        for (size_t i = 0; i < n; ++i) {
            all += (i ? L"  " : L"") + std::wstring(exts[i]);
            if (i < 9) some += (i ? L"  " : L"") + std::wstring(exts[i]);
        }
        auto extText = Label(some + L"  " + lycan::Tf(L"welcome.all", n), 11, 0.65);
        Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(extText, box_value(hstring{all}));
        extText.Margin(ThicknessHelper::FromLengths(0, 0, 0, 8));
        col.Children().Append(extText);
        col.Children().Append(Label(lycan::Tr(L"welcome.which_items_right_click_menu_shows"), 12, 0.65));
        two.Children().Append(col);

        // what the menu looks like (a sketch, not the real menu)
        auto menu = MakeCard();
        menu.Padding(ThicknessHelper::FromUniformLength(8));
        menu.VerticalAlignment(VerticalAlignment::Top);
        menu.MinWidth(190);
        StackPanel items;
        for (auto const* t : {L"main.extract_here", L"welcome.extract_name", L"main.extract", L"main.test_integrity", L"main.image_preview"}) {
            auto l = Label(lycan::Tr(t), 13);
            l.Margin(ThicknessHelper::FromLengths(8, 3, 8, 3));
            items.Children().Append(l);
        }
        items.Children().Append(Divider());
        for (auto const* t : {L"welcome.compress_zip", L"welcome.compress_7z"}) {
            auto l = Label(lycan::Tr(t), 13);
            l.Margin(ThicknessHelper::FromLengths(8, 3, 8, 3));
            items.Children().Append(l);
        }
        menu.Child(items);
        Grid::SetColumn(menu, 1);
        two.Children().Append(menu);
        card.Child(two);
        page.Children().Append(card);

        // updates: not for the Store version (the Store updates it); off unless it is ticked here
        if (lycan::DetectChannel() != lycan::InstallChannel::Store) {
            auto upd = MakeCard();
            updateToggle_ = Toggle(s_.checkUpdates, lycan::Tr(L"main.update_check"));
            upd.Child(Row(lycan::Tr(L"main.update_check"), lycan::Tr(L"main.update_check_hint"), updateToggle_));
            page.Children().Append(upd);
        }
        return page;
    }

    StackPanel Heading(std::wstring const& title, std::wstring const& sub) {
        StackPanel h;
        h.Margin(ThicknessHelper::FromLengths(4, 8, 0, 4));
        auto t = Label(title, 24);
        t.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
        h.Children().Append(t);
        h.Children().Append(Label(sub, 13, 0.7));
        return h;
    }

    // ---- the frame: steps, buttons ----
    void Build() {
        root_ = Grid{};
        RowDefinition r0, r1;
        r0.Height(GridLength{1, GridUnitType::Star});
        r1.Height(GridLength{0, GridUnitType::Auto});
        root_.RowDefinitions().Append(r0);
        root_.RowDefinitions().Append(r1);

        pages_.push_back(PageLook());
        pages_.push_back(PageView());
        pages_.push_back(PageWindows());
        ScrollViewer scroll;
        scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
        StackPanel host;
        host.Margin(ThicknessHelper::FromLengths(24, 20, 24, 12));
        for (auto const& p : pages_) host.Children().Append(p);
        scroll.Content(host);
        root_.Children().Append(scroll);

        Grid nav;
        nav.Padding(ThicknessHelper::FromLengths(24, 8, 24, 18));
        Grid::SetRow(nav, 1);
        ColumnDefinition n0, n1;
        n0.Width(GridLength{1, GridUnitType::Star});
        n1.Width(GridLength{0, GridUnitType::Auto});
        nav.ColumnDefinitions().Append(n0);
        nav.ColumnDefinitions().Append(n1);
        dots_ = StackPanel{};
        dots_.Orientation(Orientation::Horizontal);
        dots_.Spacing(8);
        dots_.VerticalAlignment(VerticalAlignment::Center);
        for (int i = 0; i < 3; ++i) {
            Microsoft::UI::Xaml::Shapes::Ellipse e;
            e.Width(8);
            e.Height(8);
            dots_.Children().Append(e);
        }
        nav.Children().Append(dots_);
        StackPanel buttons;
        buttons.Orientation(Orientation::Horizontal);
        buttons.Spacing(8);
        Grid::SetColumn(buttons, 1);
        back_ = Button{};
        back_.Content(box_value(hstring{lycan::Tr(L"welcome.back")}));
        skip_ = Button{};
        skip_.Content(box_value(hstring{lycan::Tr(L"main.skip")}));
        next_ = Button{};
        next_.Style(Application::Current().Resources().Lookup(box_value(L"AccentButtonStyle")).as<Style>());
        buttons.Children().Append(back_);
        buttons.Children().Append(skip_);
        buttons.Children().Append(next_);
        nav.Children().Append(buttons);
        root_.Children().Append(nav);

        back_.Click([weak = weak_from_this()](auto&&, auto&&) { if (auto s = weak.lock()) s->ShowStep(s->step_ - 1); });
        skip_.Click([weak = weak_from_this()](auto&&, auto&&) { if (auto s = weak.lock()) s->Close(); });
        next_.Click([weak = weak_from_this()](auto&&, auto&&) { if (auto s = weak.lock()) s->OnNext(); });
    }

    void ShowStep(int step) {
        const int old = step_;
        step_ = std::clamp(step, 0, 2);
        for (int i = 0; i < 3; ++i) pages_[static_cast<size_t>(i)].Visibility(i == step_ ? Visibility::Visible : Visibility::Collapsed);
        if (old != step_) SlideStep(pages_[static_cast<size_t>(step_)], step_ > old ? 34.0 : -34.0);
        back_.Visibility(step_ > 0 ? Visibility::Visible : Visibility::Collapsed);
        skip_.Visibility(step_ < 2 ? Visibility::Visible : Visibility::Collapsed);
        next_.Content(box_value(hstring{step_ < 2 ? lycan::Tr(L"welcome.next") : lycan::Tr(L"main.done")}));
        const auto on = ResourceBrush(L"AccentFillColorDefaultBrush"), off = ResourceBrush(L"ControlStrongStrokeColorDefaultBrush");
        for (int i = 0; i < 3; ++i) dots_.Children().GetAt(static_cast<uint32_t>(i)).as<Microsoft::UI::Xaml::Shapes::Ellipse>().Fill(i == step_ ? on : off);
    }

    // the next step comes in from the right, the one before from the left (the animation choice of this window, and the Windows setting)
    void SlideStep(UIElement const& page, double dx) {
        bool system = true;
        try { system = Windows::UI::ViewManagement::UISettings{}.AnimationsEnabled(); } catch (...) {}
        if (!s_.animations || !system || !page) return;
        using namespace Microsoft::UI::Xaml::Media::Animation;
        auto ct = page.RenderTransform().try_as<CompositeTransform>();
        if (!ct) { ct = CompositeTransform{}; page.RenderTransform(ct); }
        Storyboard sb;
        auto make = [&](DependencyObject const& target, const wchar_t* prop, double from, double to) {
            DoubleAnimation a;
            a.From(from);
            a.To(to);
            a.Duration(DurationHelper::FromTimeSpan(std::chrono::milliseconds(220)));
            CubicEase ease;
            ease.EasingMode(EasingMode::EaseOut);
            a.EasingFunction(ease);
            Storyboard::SetTarget(a, target);
            Storyboard::SetTargetProperty(a, prop);
            sb.Children().Append(a);
        };
        make(ct, L"TranslateX", dx, 0.0);
        make(page, L"Opacity", 0.0, 1.0);
        sb.Completed([page, ct, sb](auto&&, auto&&) { sb.Stop(); page.Opacity(1); ct.TranslateX(0); });
        sb.Begin();
    }

    static Brush ResourceBrush(const wchar_t* key) {
        try { return Application::Current().Resources().Lookup(box_value(hstring{key})).as<Brush>(); } catch (...) { return SolidColorBrush{Windows::UI::Color{255, 120, 120, 128}}; }
    }

    void OnNext() {
        if (step_ < 2) { ShowStep(step_ + 1); return; }
        finished_ = true;
        auto finish = host_.finish;
        auto s = s_;
        s.welcomeDone = true;
        if (updateToggle_) s.checkUpdates = updateToggle_.IsOn();
        const bool menuSwitch = !host_.menuSwitchNote.empty();
        if (menuSwitch) s.shellMenu = classicToggle_.IsOn();   // the Store version: a setting; nothing is registered
        const bool classic = !menuSwitch && classicToggle_.IsOn() && classicToggle_.IsEnabled(), win11 = win11Toggle_.IsOn() && win11Toggle_.IsEnabled(), assoc = assocToggle_.IsOn() && assocToggle_.IsEnabled();
        Close();
        if (finish) finish(s, classic, win11, assoc);
    }

    void OnClosed() {
        closed_ = true;
        if (!finished_ && host_.skipped) host_.skipped();
    }

    WelcomeHost host_;
    lycan::Settings s_;
    Window win_{nullptr};
    Grid root_{nullptr};
    std::vector<StackPanel> pages_;
    StackPanel dots_{nullptr}, accentRow_{nullptr};
    Button back_{nullptr}, skip_{nullptr}, next_{nullptr};
    ComboBox langCombo_{nullptr}, soundSet_{nullptr}, formatCombo_{nullptr};
    RadioButtons themeChoice_{nullptr}, viewChoice_{nullptr};
    ToggleSwitch soundToggle_{nullptr}, animToggle_{nullptr}, classicToggle_{nullptr}, win11Toggle_{nullptr}, assocToggle_{nullptr}, updateToggle_{nullptr};
    Grid scene_{nullptr};   // the picture on top of the first step
    Border previewFrame_{nullptr};
    TextBlock previewNote_{nullptr};
    int step_ = 0;
    bool closed_ = false, finished_ = false, closedByCode_ = false;
};

std::shared_ptr<WelcomeWindow> g_window;

}  // namespace

void ShowWelcome(WelcomeHost host) {
    if (g_window && !g_window->IsClosed()) return;
    g_window = std::make_shared<WelcomeWindow>();
    try {
        g_window->Create(std::move(host));
    } catch (hresult_error const& e) {   // a window that cannot be built must not take the program down: it is only a welcome
        lycan::diag::Warn(L"welcome window: {} (0x{:08x})", std::wstring{e.message()}, static_cast<unsigned>(e.code()));
        g_window.reset();
    }
}

bool WelcomeIsOpen() { return g_window && !g_window->IsClosed(); }

void CloseWelcome() {
    if (g_window && !g_window->IsClosed()) g_window->Close();
}

}  // namespace lycan_app
