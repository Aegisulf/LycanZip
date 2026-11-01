#include "pch.h"
#include "PreviewPane.h"

#include <algorithm>
#include <chrono>
#include <coroutine>
#include <cwctype>

#include <winrt/Windows.Data.Pdf.h>
#include <winrt/Windows.Media.Core.h>

#include "AltBeep.h"
#include "AudioPreview.h"
#include "Foreground.h"
#include "ImagePreview.h"
#include "lycan/audiotags.h"
#include "lycan/diag.h"
#include "lycan/error.h"
#include "lycan/exectype.h"
#include "lycan/i18n.h"
#include "lycan/texttype.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace std::chrono_literals;

namespace lycan_app {
namespace {

struct resume_ui {
    Microsoft::UI::Dispatching::DispatcherQueue q;
    bool await_ready() const { return q.HasThreadAccess(); }
    void await_suspend(std::coroutine_handle<> h) const { q.TryEnqueue([h] { h(); }); }
    void await_resume() const noexcept {}
};

constexpr uint64_t kImageMax = 64ull << 20;      // the same limit as the picture preview window
constexpr uint64_t kPdfMax = 64ull << 20;
constexpr uint64_t kAudioTagBytes = 16ull << 20; // enough for the tags of nearly every file (the length needs the whole file)
constexpr uint64_t kTextPane = 64ull << 10;
constexpr uint64_t kTextLarge = 4ull << 20;
constexpr size_t kCacheItems = 8;
constexpr uint64_t kCacheBytes = 160ull << 20;
constexpr auto kLoadingAfter = 150ms;            // until then the old content stays (a quick result does not flash)

std::wstring LowerExt(const std::wstring& name) {
    auto dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0) return L"";
    std::wstring e = name.substr(dot + 1);
    std::transform(e.begin(), e.end(), e.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return e;
}

std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

// Text that is worth showing as text (scripts too: they are only read into memory and shown, never run)
bool IsTextName(const std::wstring& name) {
    static const wchar_t* const kExt[] = {
        L"txt", L"text", L"md", L"markdown", L"rst", L"log", L"ini", L"cfg", L"conf", L"config", L"properties", L"toml", L"env",
        L"json", L"jsonc", L"json5", L"ndjson", L"xml", L"xaml", L"yaml", L"yml", L"csv", L"tsv", L"htm", L"html", L"xhtml", L"css",
        L"scss", L"less", L"js", L"mjs", L"cjs", L"ts", L"tsx", L"jsx", L"vue", L"c", L"cc", L"cpp", L"cxx", L"h", L"hh", L"hpp", L"hxx",
        L"inl", L"cs", L"vb", L"fs", L"fsx", L"java", L"kt", L"kts", L"gradle", L"scala", L"groovy", L"py", L"pyw", L"rb", L"go", L"rs",
        L"swift", L"m", L"mm", L"php", L"pl", L"pm", L"lua", L"r", L"dart", L"ex", L"exs", L"erl", L"hs", L"ml", L"clj", L"sql", L"sh",
        L"bash", L"zsh", L"fish", L"ps1", L"psm1", L"psd1", L"bat", L"cmd", L"vbs", L"asm", L"s", L"cmake", L"mk", L"tex", L"bib",
        L"nfo", L"diz", L"srt", L"ass", L"ssa", L"vtt", L"sub", L"lrc", L"cue", L"m3u", L"m3u8", L"pls", L"reg", L"inf", L"gitignore",
        L"gitattributes", L"editorconfig", L"sln", L"csproj", L"vcxproj", L"props", L"targets", L"manifest", L"plist", L"svg", L"lst",
        L"me", L"1st", L"patch", L"diff", L"tsv"};
    const std::wstring e = LowerExt(name);
    if (e.empty()) {
        static const wchar_t* const kNames[] = {L"readme", L"license", L"licence", L"copying", L"authors", L"changelog", L"changes", L"news",
                                                L"notice", L"install", L"makefile", L"dockerfile", L"todo", L"contributors", L"credits"};
        const std::wstring n = Lower(name);
        for (auto* k : kNames) if (n == k) return true;
        return false;
    }
    for (auto* k : kExt) if (e == k) return true;
    return false;
}

enum class Content { None, Image, Text, Pdf, Audio, Program, Locked };

Content ContentOf(PaneInfo const& info) {
    if (info.kind != PaneKind::File) return Content::None;
    if (info.locked) return Content::Locked;
    if (CanPreviewImage(info.title)) return Content::Image;
    if (LowerExt(info.title) == L"pdf") return Content::Pdf;
    if (IsTextName(info.title) || lycan::IsTextEditName(info.title)) return Content::Text;
    if (IsPreviewableAudioName(info.title)) return Content::Audio;
    if (lycan::IsExecutableName(info.title)) return Content::Program;
    return Content::None;
}

std::wstring SizeText(uint64_t b) {
    wchar_t buf[64];
    const wchar_t* units[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
    double v = static_cast<double>(b);
    int u = 0;
    while (v >= 1024 && u < 4) { v /= 1024; ++u; }
    if (u == 0) swprintf_s(buf, L"%llu B", static_cast<unsigned long long>(b));
    else swprintf_s(buf, L"%.1f %s", v, units[u]);
    return buf;
}

// Bytes -> text: a BOM (UTF-16 / UTF-8), then valid UTF-8, then the ANSI code page of the system. binary = it does not look like text.
std::wstring DecodeText(std::vector<uint8_t> const& b, bool complete, bool& binary) {
    binary = false;
    size_t n = b.size();
    if (n >= 2 && ((b[0] == 0xFF && b[1] == 0xFE) || (b[0] == 0xFE && b[1] == 0xFF))) {
        const bool be = b[0] == 0xFE;
        std::wstring w;
        w.reserve((n - 2) / 2);
        for (size_t i = 2; i + 1 < n; i += 2) w += static_cast<wchar_t>(be ? (b[i] << 8) | b[i + 1] : b[i] | (b[i + 1] << 8));
        return w;
    }
    size_t start = (n >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) ? 3 : 0;
    for (size_t i = start; i < std::min<size_t>(n, 4096); ++i)
        if (b[i] == 0) { binary = true; return {}; }
    if (!complete) {   // cut in the middle of a UTF-8 sequence: drop the incomplete end
        size_t back = 0;
        while (back < 3 && n - back > start && (b[n - back - 1] & 0xC0) == 0x80) ++back;
        if (n - back > start && (b[n - back - 1] & 0xC0) == 0xC0) n -= back + 1;
    }
    auto conv = [&](UINT cp, DWORD flags) -> std::wstring {
        const char* p = reinterpret_cast<const char*>(b.data() + start);
        const int len = static_cast<int>(n - start);
        if (len <= 0) return {};
        const int w = MultiByteToWideChar(cp, flags, p, len, nullptr, 0);
        if (w <= 0) return {};
        std::wstring s(static_cast<size_t>(w), L'\0');
        MultiByteToWideChar(cp, flags, p, len, s.data(), w);
        return s;
    };
    if (auto s = conv(CP_UTF8, MB_ERR_INVALID_CHARS); !s.empty() || n == start) return s;
    return conv(CP_ACP, 0);
}

Microsoft::UI::Xaml::Media::Brush ThemeBrush(const wchar_t* key) {
    return Application::Current().Resources().Lookup(box_value(key)).as<Microsoft::UI::Xaml::Media::Brush>();
}

}  // namespace

// ---- the reader ------------------------------------------------------------------------------------------------

void PaneSource::Reset(const std::filesystem::path& archive, const lycan::OpenOptions& open) {
    std::lock_guard lk(m_);
    src_.reset();
    archive_ = archive;
    open_ = open;
    cache_.clear();
    ++session_;
}

PaneSource::Data PaneSource::Read(uint32_t entry, uint64_t limit, const std::function<bool()>& keepGoing) {
    Data d;
    std::lock_guard lk(m_);
    if (!keepGoing()) { d.cancelled = true; return d; }
    for (size_t i = 0; i < cache_.size(); ++i) {
        if (cache_[i].entry != entry || !(cache_[i].complete || cache_[i].bytes->size() >= limit)) continue;
        Cached c = cache_[i];
        cache_.erase(cache_.begin() + static_cast<ptrdiff_t>(i));
        cache_.insert(cache_.begin(), c);
        d.bytes = c.bytes;
        d.complete = c.complete;
        return d;
    }
    struct Sink final : lycan::ByteSink {
        std::vector<uint8_t>& v;
        uint64_t limit;
        bool full = false, dropped = false;
        Sink(std::vector<uint8_t>& out, uint64_t l) : v(out), limit(l) {}
        void Write(const void* data, size_t size) override {
            if (full) { dropped = dropped || size > 0; return; }
            const size_t take = static_cast<size_t>(std::min<uint64_t>(size, limit - v.size()));
            auto* p = static_cast<const uint8_t*>(data);
            v.insert(v.end(), p, p + take);
            if (take < size) dropped = true;
            if (v.size() >= limit) full = true;
        }
    };
    auto bytes = std::make_shared<std::vector<uint8_t>>();
    Sink sink(*bytes, limit);
    const HANDLE thread = GetCurrentThread();
    const int priority = GetThreadPriority(thread);
    SetThreadPriority(thread, THREAD_PRIORITY_BELOW_NORMAL);   // the pane must never slow down what the user is doing
    bool stoppedFull = false;
    try {
        if (!src_) src_ = lycan::ArchiveSource::Open(archive_, open_);
        src_->ReadEntry(entry, sink, [&](uint64_t, uint64_t) { return !sink.full && keepGoing(); });
    } catch (lycan::Error const& e) {
        if (e.cancelled() && sink.full) stoppedFull = true;   // stopped on purpose: the first `limit` bytes are there
        else if (e.cancelled()) d.cancelled = true;
        else d.error = e.wrongPassword() ? lycan::Tr(L"preview.wrong_password_file_cannot_be_read") : lycan::Tf(L"preview.failed_read_file", winrt::hstring{e.message().Text()});
    } catch (...) {
        d.error = lycan::Tr(L"preview.failed_read_file_plain");
    }
    SetThreadPriority(thread, priority);
    if (d.cancelled || !d.error.empty()) return d;
    d.bytes = bytes;
    d.complete = !stoppedFull && !sink.dropped;
    if (bytes->size() <= kCacheBytes / 2) {
        cache_.insert(cache_.begin(), Cached{entry, bytes, d.complete});
        uint64_t total = 0;
        size_t keep = 0;
        for (; keep < cache_.size() && keep < kCacheItems; ++keep) {
            total += cache_[keep].bytes->size();
            if (total > kCacheBytes) break;
        }
        cache_.resize(std::max<size_t>(keep, 1));
    }
    return d;
}

// ---- the view (the pane, or the larger window) -----------------------------------------------------------------

class PaneView : public std::enable_shared_from_this<PaneView> {
public:
    PaneView(std::shared_ptr<PaneSource> src, bool large) : src_(std::move(src)), large_(large) {
        dq_ = Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
    }
    // Built after the shared_ptr exists: the event handlers hold weak_from_this(), which is empty inside the constructor
    static std::shared_ptr<PaneView> Make(std::shared_ptr<PaneSource> src, bool large) {
        auto v = std::make_shared<PaneView>(std::move(src), large);
        v->Build();
        return v;
    }
    FrameworkElement Root() const { return root_; }
    std::function<void()> onEnlarge, onPassword;
    std::function<void(uint32_t)> onPlay;

    void Show(PaneInfo info) {
        const uint64_t g = ++gen_;
        const Content c = ContentOf(info);
        pending_ = std::move(info);
        if (c == Content::None || c == Content::Program || c == Content::Locked) { Apply(pending_, c, {}); return; }
        // the old content stays for a moment; only a slow read shows "loading"
        if (!loadingTimer_) {
            loadingTimer_ = dq_.CreateTimer();
            loadingTimer_.IsRepeating(false);
            loadingTimer_.Interval(kLoadingAfter);
            loadingTimer_.Tick([weak = weak_from_this()](auto&&, auto&&) { if (auto s = weak.lock()) s->ShowLoading(); });
        }
        loadingFor_ = g;
        loadingTimer_.Stop();
        loadingTimer_.Start();
        Load(g, pending_, c);
    }

    void Clear() {
        ++gen_;
        pending_ = {};
        Apply(pending_, Content::None, {});
    }

private:
    // ---- building ----
    void Build() {
        root_ = Grid{};
        root_.RowSpacing(6);
        for (int i = 0; i < 4; ++i) {
            RowDefinition rd;
            rd.Height(i == 0 ? GridLength{1, GridUnitType::Star} : GridLength{0, GridUnitType::Auto});
            root_.RowDefinitions().Append(rd);
        }
        if (large_) root_.Padding(ThicknessHelper::FromLengths(16, 8, 16, 14));

        stage_ = Grid{};
        stage_.MinHeight(48);
        stage_.SizeChanged([weak = weak_from_this()](auto&&, auto&&) { if (auto s = weak.lock()) s->LayoutPicture(); });
        root_.Children().Append(stage_);

        canvas_ = Canvas{};
        img_ = Image{};
        img_.Stretch(Microsoft::UI::Xaml::Media::Stretch::Fill);
        img_.RenderTransformOrigin(Windows::Foundation::Point{0.5f, 0.5f});
        canvas_.Children().Append(img_);
        stage_.Children().Append(canvas_);

        // Text: a scroll area with a text block (in the larger window the text can be selected and copied). Not a read-only TextBox:
        // that one shows the text cursor over its scroll bars too.
        textScroll_ = ScrollViewer{};
        textScroll_.HorizontalScrollBarVisibility(ScrollBarVisibility::Auto);
        textScroll_.HorizontalScrollMode(ScrollMode::Auto);
        textScroll_.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
        Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(textScroll_, hstring{lycan::Tr(L"preview.pane_text")});
        textBlock_ = TextBlock{};
        textBlock_.FontFamily(Microsoft::UI::Xaml::Media::FontFamily{L"Cascadia Mono, Consolas"});
        textBlock_.FontSize(large_ ? 13 : 11);
        textBlock_.TextWrapping(TextWrapping::NoWrap);
        textBlock_.Padding(ThicknessHelper::FromLengths(2, 2, 8, 8));   // (its accessible name is the text itself)
        if (large_) {
            // up to 4 MB: one text block would lay out all of it at once (seconds for 4 MB); pieces of 200 lines in a repeater
            // only lay out what is on the screen. A piece can be selected and copied (not across pieces).
            lines_ = ItemsRepeater{};
            lines_.Layout(StackLayout{});
            lines_.ItemTemplate(Markup::XamlReader::Load(
                L"<DataTemplate xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation'><TextBlock Text='{Binding}'"
                L" FontFamily='Cascadia Mono, Consolas' FontSize='13' TextWrapping='NoWrap' IsTextSelectionEnabled='True' Padding='2,0,8,0'/></DataTemplate>"));
            textScroll_.Content(lines_);
        } else {
            textScroll_.Content(textBlock_);
        }
        stage_.Children().Append(textScroll_);

        iconPanel_ = StackPanel{};
        iconPanel_.Spacing(8);
        iconPanel_.HorizontalAlignment(HorizontalAlignment::Center);
        iconPanel_.VerticalAlignment(VerticalAlignment::Center);
        iconImg_ = Image{};
        iconImg_.Width(large_ ? 96 : 56);
        iconImg_.Height(large_ ? 96 : 56);
        iconGlyph_ = FontIcon{};
        iconGlyph_.FontSize(large_ ? 64 : 40);
        iconGlyph_.Opacity(0.8);
        note_ = TextBlock{};
        note_.TextWrapping(TextWrapping::Wrap);
        note_.TextAlignment(TextAlignment::Center);
        note_.FontSize(12);
        note_.Opacity(0.75);
        note_.MaxWidth(large_ ? 520 : 220);
        action_ = Button{};
        action_.HorizontalAlignment(HorizontalAlignment::Center);
        action_.Click([weak = weak_from_this()](auto&&, auto&&) {
            auto s = weak.lock();
            if (!s) return;
            if (s->actionIsPlay_) { if (s->onPlay) s->onPlay(s->shown_.node); }
            else if (s->onPassword) s->onPassword();
        });
        for (UIElement e : {UIElement{iconImg_}, UIElement{iconGlyph_}, UIElement{note_}, UIElement{action_}}) iconPanel_.Children().Append(e);
        stage_.Children().Append(iconPanel_);

        ring_ = ProgressRing{};
        ring_.Width(28);
        ring_.Height(28);
        ring_.IsActive(false);
        ring_.Visibility(Visibility::Collapsed);
        Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(ring_, hstring{lycan::Tr(L"preview.pane_loading")});
        stage_.Children().Append(ring_);

        if (!large_) {
            // a solid background and a border of its own (theme resources, so it follows the theme of the window): the default button is
            // see-through and its glyph vanishes on a picture of the same brightness (a white picture in the dark theme)
            enlarge_ = Markup::XamlReader::Load(
                L"<Button xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation' HorizontalAlignment='Right' VerticalAlignment='Top'"
                L" Padding='6,4' Margin='0,2,2,0' Background='{ThemeResource SolidBackgroundFillColorSecondaryBrush}'"
                L" BorderBrush='{ThemeResource ControlStrongStrokeColorDefaultBrush}' BorderThickness='1'"
                L" Foreground='{ThemeResource TextFillColorPrimaryBrush}'><FontIcon Glyph='&#xE740;' FontSize='12'/></Button>").as<Button>();
            enlarge_.Visibility(Visibility::Collapsed);
            Controls::ToolTipService::SetToolTip(enlarge_, box_value(hstring{lycan::Tr(L"preview.pane_enlarge")}));
            Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(enlarge_, hstring{lycan::Tr(L"preview.pane_enlarge")});
            enlarge_.Click([weak = weak_from_this()](auto&&, auto&&) { if (auto s = weak.lock()) if (s->onEnlarge) s->onEnlarge(); });
            stage_.Children().Append(enlarge_);
        }

        title_ = TextBlock{};
        title_.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
        title_.TextTrimming(TextTrimming::CharacterEllipsis);
        title_.FontSize(large_ ? 15 : 13);
        Grid::SetRow(title_, 1);
        root_.Children().Append(title_);

        details_ = Grid{};
        details_.ColumnSpacing(8);
        details_.RowSpacing(1);
        ColumnDefinition c0, c1;
        c0.Width(GridLength{0, GridUnitType::Auto});
        c1.Width(GridLength{1, GridUnitType::Star});
        details_.ColumnDefinitions().Append(c0);
        details_.ColumnDefinitions().Append(c1);
        Grid::SetRow(details_, 2);
        root_.Children().Append(details_);

        if (large_) {   // pages of a PDF
            pdfBar_ = StackPanel{};
            pdfBar_.Orientation(Orientation::Horizontal);
            pdfBar_.Spacing(8);
            pdfBar_.HorizontalAlignment(HorizontalAlignment::Center);
            auto nav = [&](const wchar_t* glyph, const wchar_t* tip, int step) {
                Button b;
                FontIcon g;
                g.Glyph(glyph);
                g.FontSize(12);
                b.Content(g);
                Controls::ToolTipService::SetToolTip(b, box_value(hstring{lycan::Tr(tip)}));
                Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(b, hstring{lycan::Tr(tip)});
                b.Click([weak = weak_from_this(), step](auto&&, auto&&) { if (auto s = weak.lock()) s->TurnPage(step); });
                return b;
            };
            pdfPrev_ = nav(L"", L"preview.pane_previous_page", -1);
            pdfNext_ = nav(L"", L"preview.pane_next_page", +1);
            pdfLabel_ = TextBlock{};
            pdfLabel_.VerticalAlignment(VerticalAlignment::Center);
            pdfBar_.Children().Append(pdfPrev_);
            pdfBar_.Children().Append(pdfLabel_);
            pdfBar_.Children().Append(pdfNext_);
            pdfBar_.Visibility(Visibility::Collapsed);
            Grid::SetRow(pdfBar_, 3);
            root_.Children().Append(pdfBar_);
        }
        Apply({}, Content::None, {});
    }

    // ---- showing ----
    enum class Stage { Nothing, Picture, Text, Icon };

    void ShowStage(Stage s) {
        canvas_.Visibility(s == Stage::Picture ? Visibility::Visible : Visibility::Collapsed);
        textScroll_.Visibility(s == Stage::Text ? Visibility::Visible : Visibility::Collapsed);
        iconPanel_.Visibility(s == Stage::Icon ? Visibility::Visible : Visibility::Collapsed);
        ring_.IsActive(false);
        ring_.Visibility(Visibility::Collapsed);
    }

    // A picture, a text or a PDF fills the pane: no name and details under it (the rest keeps them)
    void HideHeader() {
        title_.Visibility(Visibility::Collapsed);
        details_.Visibility(Visibility::Collapsed);
        root_.RowSpacing(0);   // (the empty rows would still be spaced)
    }

    void SetPictureName(std::wstring const& name) {
        Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(img_, hstring{name});
    }

    void ShowLoading() {
        if (loadingFor_ != gen_) return;
        if (const Content c = ContentOf(pending_); c == Content::Image || c == Content::Text || c == Content::Pdf) HideHeader();   // (no jump when it arrives)
        else SetHeader(pending_, {});
        ShowStage(Stage::Nothing);
        if (enlarge_) enlarge_.Visibility(Visibility::Collapsed);
        if (pdfBar_) pdfBar_.Visibility(Visibility::Collapsed);
        ring_.Visibility(Visibility::Visible);
        ring_.IsActive(true);
    }

    void SetHeader(PaneInfo const& info, std::vector<std::pair<std::wstring, std::wstring>> const& extra) {
        title_.Text(hstring{info.title});
        title_.Visibility(info.title.empty() ? Visibility::Collapsed : Visibility::Visible);
        details_.Visibility(Visibility::Visible);
        root_.RowSpacing(6);
        details_.Children().Clear();
        details_.RowDefinitions().Clear();
        int r = 0;
        auto add = [&](std::pair<std::wstring, std::wstring> const& row) {
            RowDefinition rd;
            rd.Height(GridLength{0, GridUnitType::Auto});
            details_.RowDefinitions().Append(rd);
            TextBlock l, v;
            l.Text(hstring{row.first});
            v.Text(hstring{row.second});
            for (TextBlock t : {l, v}) {
                t.FontSize(large_ ? 13 : 12);
                t.TextTrimming(TextTrimming::CharacterEllipsis);
                Grid::SetRow(t, r);
            }
            l.Opacity(0.65);
            Grid::SetColumn(v, 1);
            details_.Children().Append(l);
            details_.Children().Append(v);
            ++r;
        };
        for (auto const& row : extra) add(row);
        for (auto const& row : info.rows) add(row);
    }

    // The icon (or glyph) of the item, with a note and maybe a button
    void ShowIcon(PaneInfo const& info, std::wstring const& note, int action) {   // action: 0 none, 1 play, 2 password
        iconImg_.Source(info.icon);
        iconImg_.Visibility(info.icon ? Visibility::Visible : Visibility::Collapsed);
        iconGlyph_.Glyph(hstring{info.glyph.empty() ? std::wstring(L"") : info.glyph});
        iconGlyph_.Visibility(info.icon || info.kind == PaneKind::Empty ? Visibility::Collapsed : Visibility::Visible);
        note_.Text(hstring{note});
        note_.Visibility(note.empty() ? Visibility::Collapsed : Visibility::Visible);
        actionIsPlay_ = action == 1;
        if (action) {
            action_.Content(box_value(hstring{lycan::Tr(action == 1 ? L"preview.pane_play" : L"preview.pane_enter_password")}));
            Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(action_, hstring{lycan::Tr(action == 1 ? L"preview.pane_play" : L"preview.pane_enter_password")});
        }
        action_.Visibility(action ? Visibility::Visible : Visibility::Collapsed);
        ShowStage(Stage::Icon);
    }

    // Everything that does not need bytes, or a failure
    void Apply(PaneInfo const& info, Content c, std::wstring const& error) {
        if (loadingTimer_ && loadingFor_ == gen_) loadingTimer_.Stop();
        shown_ = info;
        img_.Source(nullptr);
        textBlock_.Text(L"");
        if (lines_) lines_.ItemsSource(nullptr);
        if (enlarge_) enlarge_.Visibility(Visibility::Collapsed);
        if (pdfBar_) pdfBar_.Visibility(Visibility::Collapsed);
        pdf_ = nullptr;
        SetHeader(info, {});
        if (info.kind == PaneKind::Empty) { ShowStage(Stage::Nothing); return; }
        std::wstring note = error;
        int action = 0;
        if (c == Content::Locked) { note = lycan::Tr(L"preview.pane_locked"); action = onPassword ? 2 : 0; }
        else if (c == Content::Program && note.empty()) note = lycan::Tr(L"preview.pane_program_not_previewed");
        else if (c == Content::Audio && onPlay) action = 1;
        ShowIcon(info, note, action);
    }

    void Done(uint64_t g) {
        if (g != gen_) return;
        if (loadingTimer_) loadingTimer_.Stop();
    }

    fire_and_forget Load(uint64_t g, PaneInfo info, Content c) {
        auto self = shared_from_this();
        const auto started = std::chrono::steady_clock::now();
        uint64_t limit = 0;
        switch (c) {
            case Content::Image: limit = kImageMax; break;
            case Content::Pdf: limit = kPdfMax; break;
            case Content::Text: limit = large_ ? kTextLarge : kTextPane; break;
            case Content::Audio: limit = kAudioTagBytes; break;
            default: break;
        }
        if ((c == Content::Image || c == Content::Pdf) && info.size > limit) {
            Done(g);
            Apply(info, c, lycan::Tf(L"preview.file_too_large_preview_limit_64", SizeText(info.size)));
            co_return;
        }
        co_await winrt::resume_background();   // (the main window waits until the selection stops moving before it calls Show)
        auto data = src_->Read(info.entryIndex, limit, [self, g] { return self->gen_.load() == g; });
        co_await resume_ui{dq_};
        if (g != gen_ || data.cancelled) co_return;
        if (!data.error.empty() || !data.bytes) { Done(g); Apply(info, c, data.error); co_return; }

        std::vector<std::pair<std::wstring, std::wstring>> extra;
        try {
            if (c == Content::Image) {
                auto out = std::make_shared<PaneImage>();
                co_await DecodePaneImage(data.bytes, DecodeWidth(), out);
                if (g != gen_) co_return;
                Done(g);
                shown_ = info;
                pdf_ = nullptr;
                SetPicture(out->image, out->w, out->h, out->rotate, out->mirror);
                SetPictureName(info.title + L", " + std::to_wstring(out->w) + L" × " + std::to_wstring(out->h) + L" px");
                HideHeader();   // something to look at: the whole pane is for it
                ShowEnlarge(true);
            } else if (c == Content::Pdf) {
                Windows::Storage::Streams::InMemoryRandomAccessStream ras;
                {
                    Windows::Storage::Streams::DataWriter dw(ras);
                    dw.WriteBytes(array_view<const uint8_t>(data.bytes->data(), data.bytes->data() + data.bytes->size()));
                    co_await dw.StoreAsync();
                    dw.DetachStream();
                }
                ras.Seek(0);
                auto doc = co_await Windows::Data::Pdf::PdfDocument::LoadFromStreamAsync(ras);
                if (g != gen_) co_return;
                pdf_ = doc;
                page_ = 0;
                shown_ = info;
                co_await RenderPage(g);
                if (g != gen_) co_return;
                Done(g);
                HideHeader();
                ShowEnlarge(true);
            } else if (c == Content::Text) {
                bool binary = false;
                std::wstring text = DecodeText(*data.bytes, data.complete, binary);
                Done(g);
                if (binary) { Apply(info, c, lycan::Tr(L"preview.pane_not_text")); co_return; }
                shown_ = info;
                pdf_ = nullptr;
                if (!data.complete) text += L"\n\n" + lycan::Tf(L"preview.pane_text_truncated", SizeText(limit));
                if (lines_) {
                    auto pieces = single_threaded_vector<Windows::Foundation::IInspectable>();
                    size_t start = 0, count = 0;
                    for (size_t i = 0; i <= text.size(); ++i) {
                        if (i < text.size() && !(text[i] == L'\n' && ++count == 200)) continue;
                        size_t end = i;   // without the line break that ends the piece
                        if (end > start && text[end - 1] == L'\r') --end;
                        pieces.Append(box_value(hstring{std::wstring_view(text).substr(start, end - start)}));
                        start = i + 1;
                        count = 0;
                    }
                    lines_.ItemsSource(pieces);
                } else {
                    textBlock_.Text(hstring{text});
                }
                textScroll_.ChangeView(0.0, 0.0, nullptr, true);
                ShowStage(Stage::Text);
                HideHeader();
                ShowEnlarge(true);
            } else if (c == Content::Audio) {
                const lycan::AudioTags tags = lycan::ReadAudioTags(data.bytes->data(), data.bytes->size());
                if (!tags.title.empty()) extra.emplace_back(lycan::Tr(L"preview.pane_audio_title"), tags.title);
                if (!tags.artist.empty()) extra.emplace_back(lycan::Tr(L"preview.pane_audio_artist"), tags.artist);
                if (!tags.album.empty()) extra.emplace_back(lycan::Tr(L"preview.pane_audio_album"), tags.album);
                if (data.complete) {   // the length needs the whole file
                    try {
                        Windows::Storage::Streams::InMemoryRandomAccessStream ras;
                        {
                            Windows::Storage::Streams::DataWriter dw(ras);
                            dw.WriteBytes(array_view<const uint8_t>(data.bytes->data(), data.bytes->data() + data.bytes->size()));
                            co_await dw.StoreAsync();
                            dw.DetachStream();
                        }
                        ras.Seek(0);
                        auto ms = Windows::Media::Core::MediaSource::CreateFromStream(ras, hstring{AudioContentType(info.title)});
                        co_await ms.OpenAsync();
                        if (auto dur = ms.Duration()) {
                            const long long s = std::chrono::duration_cast<std::chrono::seconds>(dur.Value()).count();
                            wchar_t buf[32];
                            if (s >= 3600) swprintf_s(buf, L"%lld:%02lld:%02lld", s / 3600, (s / 60) % 60, s % 60);
                            else swprintf_s(buf, L"%lld:%02lld", s / 60, s % 60);
                            extra.emplace_back(lycan::Tr(L"preview.pane_length"), buf);
                        }
                        ms.Close();
                    } catch (...) {}   // a format this PC does not know: only the tags
                    if (g != gen_) co_return;
                }
                Done(g);
                Apply(info, c, {});
                SetHeader(info, extra);
            }
        } catch (...) {
            if (g != gen_) co_return;
            Done(g);
            Apply(info, c, c == Content::Pdf ? lycan::Tr(L"preview.pane_pdf_failed") : lycan::Tr(L"preview.cannot_display_image_file_may_be"));
        }
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
        lycan::diag::Debug(L"preview pane{}: {} bytes{} shown in {} ms", large_ ? L" (large)" : L"", data.bytes ? data.bytes->size() : 0, data.complete ? L"" : L" (first part)", ms);
    }

    void ShowEnlarge(bool on) {
        if (!enlarge_) return;
        enlarge_.Visibility(on && onEnlarge ? Visibility::Visible : Visibility::Collapsed);
        // over a text it stays left of the vertical scroll bar (which widens to about 16 px under the mouse)
        const bool text = textScroll_.Visibility() == Visibility::Visible;
        enlarge_.Margin(ThicknessHelper::FromLengths(0, 2, text ? 20 : 2, 0));
    }

    uint32_t DecodeWidth() const {
        double scale = 1.0;
        if (auto xr = root_.XamlRoot()) scale = xr.RasterizationScale();
        const double w = std::max(stage_.ActualWidth(), stage_.ActualHeight()) * scale;
        const uint32_t cap = large_ ? 4096 : 1600;
        return std::clamp<uint32_t>(static_cast<uint32_t>(w > 0 ? w : 600), 256, cap);
    }

    void SetPicture(Microsoft::UI::Xaml::Media::ImageSource const& src, uint32_t w, uint32_t h, int rotate, bool mirror) {
        img_.Source(src);
        picW_ = w;
        picH_ = h;
        rotate_ = rotate;
        mirror_ = mirror;
        ShowStage(Stage::Picture);
        LayoutPicture();
    }

    // Fits the picture into the stage (never larger than its own size); a quarter turn is drawn by a transform, so the element
    // keeps the unturned size and sits in a canvas (no layout clip)
    void LayoutPicture() {
        if (!picW_ || !picH_) return;
        const double sw = stage_.ActualWidth(), sh = stage_.ActualHeight();
        if (sw <= 0 || sh <= 0) return;
        const double scale = std::min({sw / picW_, sh / picH_, 1.0});
        const double dw = picW_ * scale, dh = picH_ * scale;          // upright on the screen
        const bool quarter = (rotate_ & 1) != 0;
        const double ew = quarter ? dh : dw, eh = quarter ? dw : dh;    // the element before the turn
        img_.Width(ew);
        img_.Height(eh);
        Canvas::SetLeft(img_, (sw - ew) / 2);
        Canvas::SetTop(img_, (sh - eh) / 2);
        Microsoft::UI::Xaml::Media::CompositeTransform t;
        t.Rotation(90.0 * rotate_);
        t.ScaleX(mirror_ ? -1.0 : 1.0);
        img_.RenderTransform(t);
        Microsoft::UI::Xaml::Media::RectangleGeometry clip;
        clip.Rect(Windows::Foundation::Rect{0, 0, static_cast<float>(sw), static_cast<float>(sh)});
        canvas_.Clip(clip);
    }

    Windows::Foundation::IAsyncAction RenderPage(uint64_t g) {
        auto self = shared_from_this();
        if (!pdf_ || pdf_.PageCount() == 0) co_return;
        page_ = std::min(page_, pdf_.PageCount() - 1);
        auto page = pdf_.GetPage(page_);
        const auto size = page.Size();
        Windows::Data::Pdf::PdfPageRenderOptions opt;
        opt.DestinationWidth(DecodeWidth());
        Windows::Storage::Streams::InMemoryRandomAccessStream out;
        co_await page.RenderToStreamAsync(out, opt);
        if (g != gen_) co_return;
        Microsoft::UI::Xaml::Media::Imaging::BitmapImage bi;
        out.Seek(0);
        co_await bi.SetSourceAsync(out);
        if (g != gen_) co_return;
        const uint32_t w = opt.DestinationWidth(), h = size.Width > 0 ? static_cast<uint32_t>(w * size.Height / size.Width) : w;
        SetPicture(bi, w, h, 0, false);
        SetPictureName(shown_.title + L", " + lycan::Tf(L"preview.pane_page_of", page_ + 1, pdf_.PageCount()));
        if (pdfBar_) {
            pdfBar_.Visibility(pdf_.PageCount() > 1 ? Visibility::Visible : Visibility::Collapsed);
            pdfLabel_.Text(hstring{lycan::Tf(L"preview.pane_page_of", page_ + 1, pdf_.PageCount())});
            pdfPrev_.IsEnabled(page_ > 0);
            pdfNext_.IsEnabled(page_ + 1 < pdf_.PageCount());
        }
    }

    fire_and_forget TurnPage(int step) {
        auto self = shared_from_this();
        if (!pdf_) co_return;
        const int64_t next = static_cast<int64_t>(page_) + step;
        if (next < 0 || next >= static_cast<int64_t>(pdf_.PageCount())) co_return;
        page_ = static_cast<uint32_t>(next);
        try { co_await RenderPage(gen_); } catch (...) {}
    }

    std::shared_ptr<PaneSource> src_;
    bool large_ = false;
    Microsoft::UI::Dispatching::DispatcherQueue dq_{nullptr};
    std::atomic<uint64_t> gen_{0};
    uint64_t loadingFor_ = 0;
    Microsoft::UI::Dispatching::DispatcherQueueTimer loadingTimer_{nullptr};
    PaneInfo pending_, shown_;
    bool actionIsPlay_ = false;
    uint32_t picW_ = 0, picH_ = 0;
    int rotate_ = 0;
    bool mirror_ = false;
    Windows::Data::Pdf::PdfDocument pdf_{nullptr};
    uint32_t page_ = 0;

    Grid root_{nullptr}, stage_{nullptr}, details_{nullptr};
    Canvas canvas_{nullptr};
    Image img_{nullptr}, iconImg_{nullptr};
    ScrollViewer textScroll_{nullptr};
    ItemsRepeater lines_{nullptr};   // the text of the larger window, in pieces
    TextBlock textBlock_{nullptr}, title_{nullptr}, note_{nullptr}, pdfLabel_{nullptr};
    StackPanel iconPanel_{nullptr}, pdfBar_{nullptr};
    FontIcon iconGlyph_{nullptr};
    Button action_{nullptr}, enlarge_{nullptr}, pdfPrev_{nullptr}, pdfNext_{nullptr};
    ProgressRing ring_{nullptr};
};

// ---- the pane and its larger window ----------------------------------------------------------------------------

PreviewPane::PreviewPane() : source_(std::make_shared<PaneSource>()) {
    view_ = PaneView::Make(source_, false);
    view_->onEnlarge = [this] { if (onEnlarge) onEnlarge(); };
    view_->onPassword = [this] { if (onPassword) onPassword(); };
    view_->onPlay = [this](uint32_t node) { if (onPlayAudio) onPlayAudio(node); };
}

PreviewPane::~PreviewPane() {
    life_.reset();          // the Closed handler of the larger window must not touch this object any more
    largeClosed_ = nullptr;
    if (largeWin_) { try { largeWin_.Close(); } catch (...) {} }
}

FrameworkElement PreviewPane::Root() const { return view_->Root(); }

void PreviewPane::Reset(const std::filesystem::path& archive, const lycan::OpenOptions& open) {
    source_->Reset(archive, open);
    last_ = {};
    view_->Clear();
    if (large_) large_->Clear();
}

void PreviewPane::Show(PaneInfo info) {
    last_ = info;
    if (largeWin_) largeWin_.Title(hstring{info.title.empty() ? std::wstring(lycan::Tr(L"preview.pane_window_title")) : lycan::Tf(L"preview.pane_window_title_name", info.title)});
    if (active_) view_->Show(info);
    if (large_) large_->Show(std::move(info));
}

void PreviewPane::SetActive(bool on) {
    if (active_ == on) return;
    active_ = on;
    if (on) view_->Show(last_);
    else view_->Clear();
}

std::string PreviewPane::LargeRect() const {
    if (!largeWin_) return {};
    const auto pos = largeWin_.AppWindow().Position();
    const auto size = largeWin_.AppWindow().Size();
    return std::to_string(pos.X) + "," + std::to_string(pos.Y) + "," + std::to_string(size.Width) + "," + std::to_string(size.Height);
}

void PreviewPane::OpenLarge(LargeOptions o) {
    if (largeWin_) {
        largeWin_.Activate();
        return;
    }
    large_ = PaneView::Make(source_, true);
    large_->onPassword = [this] { if (onPassword) onPassword(); };
    largeClosed_ = std::move(o.onClosed);
    largeWin_ = Window{};
    largeWin_.SystemBackdrop(Microsoft::UI::Xaml::Media::MicaBackdrop{});
    Grid root;
    root.RequestedTheme(o.theme);
    root.Children().Append(large_->Root());
    Input::KeyboardAccelerator esc;
    esc.Key(Windows::System::VirtualKey::Escape);
    esc.Invoked([this, life = std::weak_ptr<int>(life_)](auto&&, Input::KeyboardAcceleratorInvokedEventArgs const& a) {
        a.Handled(true);
        // not inside the key handler of the window that goes away
        Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread().TryEnqueue([this, life] { if (life.lock()) CloseLarge(); });
    });
    root.KeyboardAccelerators().Append(esc);
    root.KeyboardAcceleratorPlacementMode(Input::KeyboardAcceleratorPlacementMode::Hidden);
    if (auto tag = lycan::i18n::UiLanguageTag(); !tag.empty()) root.Language(hstring{tag});  // the font fallback picks the fonts by the language
    largeWin_.Content(root);
    largeWin_.Title(hstring{lycan::Tr(L"preview.pane_window_title")});

    HWND h{};
    check_hresult(largeWin_.as<::IWindowNative>()->get_WindowHandle(&h));
    SuppressAltBeep(h);
    int x = 0, y = 0, w = 0, ht = 0;
    if (sscanf_s(o.rect.c_str(), "%d,%d,%d,%d", &x, &y, &w, &ht) == 4 && w >= 200 && ht >= 150) {
        RECT r{x, y, x + w, y + ht};   // only when it is still on a monitor (the monitors may have changed)
        if (MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) largeWin_.AppWindow().MoveAndResize(Windows::Graphics::RectInt32{x, y, w, ht});
        else w = 0;
    } else {
        w = 0;
    }
    if (w == 0) {
        const double scale = GetDpiForWindow(o.owner ? o.owner : h) / 96.0;
        largeWin_.AppWindow().Resize(Windows::Graphics::SizeInt32{static_cast<int32_t>(900 * scale), static_cast<int32_t>(680 * scale)});
    }
    largeWin_.Closed([this, life = std::weak_ptr<int>(life_)](auto&&, auto&&) {
        if (!life.lock()) return;
        std::string rect;
        try { rect = LargeRect(); } catch (...) {}
        // the window and its view are let go on the next turn, not while the window is still handling its own Closed event
        Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread().TryEnqueue([win = largeWin_, view = large_] {});
        largeWin_ = nullptr;
        large_.reset();
        if (auto done = std::move(largeClosed_)) done(rect);
    });
    largeWin_.Activate();
    ForceForeground(h);
    if (!last_.title.empty()) largeWin_.Title(hstring{lycan::Tf(L"preview.pane_window_title_name", last_.title)});
    large_->Show(last_);
}

void PreviewPane::CloseLarge() {
    if (largeWin_) largeWin_.Close();   // Closed does the rest
}

bool PreviewPane::LargeOpen() const { return static_cast<bool>(largeWin_); }

}  // namespace lycan_app
