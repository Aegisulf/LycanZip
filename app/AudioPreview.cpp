#include "pch.h"
#include "AudioPreview.h"
#include "AltBeep.h"
#include "Foreground.h"

#include <algorithm>
#include <cwctype>
#include <random>
#include <atomic>
#include <chrono>
#include <coroutine>
#include <map>
#include <memory>

#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Media.MediaProperties.h>
#include <winrt/Windows.Media.Playback.h>

#include "PreviewReader.h"
#include "lycan/audiotags.h"
#include "lycan/i18n.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;

namespace lycan_app {
namespace {

struct resume_ui {
    Microsoft::UI::Dispatching::DispatcherQueue q;
    bool await_ready() const { return q.HasThreadAccess(); }
    void await_suspend(std::coroutine_handle<> h) const { q.TryEnqueue([h] { h(); }); }
    void await_resume() const noexcept {}
};

constexpr uint64_t kMaxBytes = 256ull << 20;  // the file is held in memory (twice, briefly) while it plays

std::wstring LowerExt(const std::wstring& name) {
    auto dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos) return L"";
    std::wstring e = name.substr(dot);
    std::transform(e.begin(), e.end(), e.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return e;
}

std::wstring Stem(const std::wstring& name) {
    auto dot = name.find_last_of(L'.');
    return dot == std::wstring::npos || dot == 0 ? name : name.substr(0, dot);
}

const wchar_t* ContentType(const std::wstring& name) {
    const auto e = LowerExt(name);
    if (e == L".mp3") return L"audio/mpeg";
    if (e == L".m4a" || e == L".m4b") return L"audio/mp4";
    if (e == L".aac") return L"audio/aac";
    if (e == L".wav") return L"audio/wav";
    if (e == L".flac") return L"audio/flac";
    if (e == L".ogg" || e == L".oga" || e == L".opus") return L"audio/ogg";
    if (e == L".wma") return L"audio/x-ms-wma";
    if (e == L".aif" || e == L".aiff" || e == L".aifc") return L"audio/aiff";
    if (e == L".mka") return L"audio/x-matroska";
    return L"audio/mpeg";
}

std::wstring Clock(double seconds) {
    if (!(seconds > 0)) seconds = 0;
    const long long s = static_cast<long long>(seconds);
    wchar_t buf[32];
    if (s >= 3600) swprintf_s(buf, L"%lld:%02lld:%02lld", s / 3600, (s / 60) % 60, s % 60);
    else swprintf_s(buf, L"%lld:%02lld", s / 60, s % 60);
    return buf;
}

double Seconds(Windows::Foundation::TimeSpan t) { return std::chrono::duration<double>(t).count(); }

class AudioWindow : public std::enable_shared_from_this<AudioWindow> {
public:
    const void* owner_ = nullptr;

    void Create(AudioRequest req) {
        dq_ = Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
        win_ = Window{};
        win_.SystemBackdrop(Microsoft::UI::Xaml::Media::DesktopAcrylicBackdrop{});
        win_.ExtendsContentIntoTitleBar(true);
        Build();
        InitPlayer();
        Start(std::move(req));
        win_.Closed([weak = weak_from_this()](auto&&, auto&&) { if (auto s = weak.lock()) s->OnClosed(); });
        BringToFront();
        play_.Focus(FocusState::Programmatic);  // not the first list item (it would show a focus frame next to the real selection)
    }

    // Already open: same list = just play the chosen one; otherwise switch to the new list
    void Apply(AudioRequest req) {
        const bool same = req.archive == req_.archive && req.items.size() == req_.items.size() && !req.items.empty() &&
                          req.items.front().entryIndex == req_.items.front().entryIndex && req.items.back().entryIndex == req_.items.back().entryIndex;
        if (same) {
            req_.start = req.start;
            Play(std::min(req.start, req_.items.size() - 1));
        } else {
            Start(std::move(req));
        }
        BringToFront();
    }

    void Close() {
        if (closed_) return;
        Shutdown();
        if (win_) win_.Close();
    }
    bool IsClosed() const { return closed_; }
    void OnClosed();

private:
    using VK = Windows::System::VirtualKey;
    using VM = Windows::System::VirtualKeyModifiers;

    // ---- window ------------------------------------------------------------------------------------------------
    void Build() {
        root_ = Grid{};
        root_.RequestedTheme(ElementTheme::Dark);
        auto dim = [](uint8_t a) { return Microsoft::UI::Xaml::Media::SolidColorBrush{Windows::UI::Color{a, 0, 0, 0}}; };
        for (double w : {300.0, 0.0}) {
            ColumnDefinition cd;
            cd.Width(w > 0 ? GridLength{w, GridUnitType::Pixel} : GridLength{1, GridUnitType::Star});
            root_.ColumnDefinitions().Append(cd);
        }
        RowDefinition r0, r1;
        r0.Height(GridLength{0, GridUnitType::Auto});
        r1.Height(GridLength{1, GridUnitType::Star});
        root_.RowDefinitions().Append(r0);
        root_.RowDefinitions().Append(r1);

        // title strip (drag area); the system buttons sit on its right
        titleBar_ = Grid{};
        titleBar_.Height(44);
        titleBar_.Background(dim(0x55));
        titleBar_.Padding(ThicknessHelper::FromLengths(14, 0, 150, 0));
        TextBlock cap;
        cap.Text(lycan::Tr(L"main.audio_preview"));
        cap.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
        cap.VerticalAlignment(VerticalAlignment::Center);
        titleBar_.Children().Append(cap);
        Grid::SetColumnSpan(titleBar_, 2);
        root_.Children().Append(titleBar_);
        win_.SetTitleBar(titleBar_);

        // list of the audio files
        list_ = ListView{};
        list_.SelectionMode(ListViewSelectionMode::Single);
        list_.Background(dim(0x33));
        Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(list_, lycan::Tr(L"audio.audio_files"));
        list_.SelectionChanged([self = weak_from_this()](auto&&, auto&&) {
            auto s = self.lock();
            if (!s || s->syncingList_) return;
            const int i = s->list_.SelectedIndex();
            if (i >= 0 && static_cast<size_t>(i) != s->pos_) s->Play(static_cast<size_t>(i));
        });
        Grid::SetRow(list_, 1);
        root_.Children().Append(list_);

        // the player
        Grid right;
        RowDefinition a, b;
        a.Height(GridLength{1, GridUnitType::Star});
        b.Height(GridLength{0, GridUnitType::Auto});
        right.RowDefinitions().Append(a);
        right.RowDefinitions().Append(b);
        Grid::SetRow(right, 1);
        Grid::SetColumn(right, 1);

        StackPanel info;
        info.VerticalAlignment(VerticalAlignment::Center);
        info.HorizontalAlignment(HorizontalAlignment::Center);
        info.MaxWidth(520);
        info.Spacing(8);
        info.Padding(ThicknessHelper::FromUniformLength(24));
        // where a cover picture would be: what is known about the file (the tags are below, the technical data in this card)
        FontIcon cover;
        cover.Glyph(L"");
        cover.FontSize(48);
        cover.Opacity(0.4);
        cover.Margin(ThicknessHelper::FromLengths(0, 0, 0, 4));
        info.Children().Append(cover);
        techCard_ = Grid{};
        techCard_.ColumnSpacing(14);
        techCard_.RowSpacing(3);
        techCard_.HorizontalAlignment(HorizontalAlignment::Center);
        techCard_.Margin(ThicknessHelper::FromLengths(0, 0, 0, 10));
        {
            ColumnDefinition c0, c1;
            c0.Width(GridLength{0, GridUnitType::Auto});
            c1.Width(GridLength{0, GridUnitType::Auto});
            techCard_.ColumnDefinitions().Append(c0);
            techCard_.ColumnDefinitions().Append(c1);
        }
        info.Children().Append(techCard_);
        auto text = [&](TextBlock& t, double size, double opacity, bool bold) {
            t = TextBlock{};
            t.FontSize(size);
            t.Opacity(opacity);
            t.TextAlignment(TextAlignment::Center);
            t.TextWrapping(TextWrapping::Wrap);
            if (bold) t.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
            info.Children().Append(t);
        };
        text(title_, 26, 1.0, true);
        text(artist_, 17, 0.85, false);
        text(album_, 14, 0.65, false);
        text(file_, 12, 0.5, false);
        file_.TextTrimming(TextTrimming::CharacterEllipsis);
        file_.TextWrapping(TextWrapping::NoWrap);
        text(status_, 13, 0.9, false);
        right.Children().Append(info);

        StackPanel controls;
        controls.Margin(ThicknessHelper::FromLengths(28, 0, 28, 22));
        controls.Spacing(10);
        Grid seekRow;
        seekRow.ColumnSpacing(10);
        for (int i = 0; i < 3; ++i) {
            ColumnDefinition cd;
            cd.Width(i == 1 ? GridLength{1, GridUnitType::Star} : GridLength{0, GridUnitType::Auto});
            seekRow.ColumnDefinitions().Append(cd);
        }
        elapsed_ = TextBlock{};
        elapsed_.Text(L"0:00");
        elapsed_.VerticalAlignment(VerticalAlignment::Center);
        elapsed_.Opacity(0.8);
        total_ = TextBlock{};
        total_.Text(L"0:00");
        total_.VerticalAlignment(VerticalAlignment::Center);
        total_.Opacity(0.8);
        seek_ = Slider{};
        seek_.Minimum(0);
        seek_.Maximum(1);
        seek_.StepFrequency(0.1);
        seek_.IsEnabled(false);
        seek_.IsThumbToolTipEnabled(false);
        Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(seek_, lycan::Tr(L"audio.playback_position"));
        seek_.ValueChanged([self = weak_from_this()](auto&&, auto&&) {
            auto s = self.lock();
            if (!s || s->updating_ || s->closed_) return;
            try { s->player_.PlaybackSession().Position(std::chrono::duration_cast<Windows::Foundation::TimeSpan>(std::chrono::duration<double>(s->seek_.Value()))); } catch (...) {}
        });
        Grid::SetColumn(seek_, 1);
        Grid::SetColumn(total_, 2);
        seekRow.Children().Append(elapsed_);
        seekRow.Children().Append(seek_);
        seekRow.Children().Append(total_);
        controls.Children().Append(seekRow);

        Grid buttons;
        for (int i = 0; i < 3; ++i) {
            ColumnDefinition cd;
            cd.Width(i == 1 ? GridLength{1, GridUnitType::Star} : GridLength{0, GridUnitType::Auto});
            buttons.ColumnDefinitions().Append(cd);
        }
        StackPanel transport;
        transport.Orientation(Orientation::Horizontal);
        transport.Spacing(10);
        transport.HorizontalAlignment(HorizontalAlignment::Center);
        auto mk = [](const wchar_t* glyph, const wchar_t* tip, double size, FontIcon* keep = nullptr) {
            Button btn;
            FontIcon ic;
            ic.Glyph(glyph);
            ic.FontSize(size);
            btn.Content(ic);
            btn.Padding(ThicknessHelper::FromUniformLength(size > 16 ? 12 : 9));
            Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(btn, box_value(hstring{tip}));
            Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(btn, hstring{tip});
            if (keep) *keep = ic;
            return btn;
        };
        prev_ = mk(L"", lycan::Tr(L"audio.previous_track_ctrl_left"), 15);
        play_ = mk(L"", lycan::Tr(L"audio.play"), 22, &playIcon_);
        next_ = mk(L"", lycan::Tr(L"audio.next_track_ctrl_right"), 15);
        prev_.Click([self = weak_from_this()](auto&&, auto&&) { if (auto s = self.lock()) s->Step(-1); });
        play_.Click([self = weak_from_this()](auto&&, auto&&) { if (auto s = self.lock()) s->TogglePlay(); });
        next_.Click([self = weak_from_this()](auto&&, auto&&) { if (auto s = self.lock()) s->Step(+1); });
        transport.Children().Append(prev_);
        transport.Children().Append(play_);
        transport.Children().Append(next_);
        Grid::SetColumn(transport, 1);
        buttons.Children().Append(transport);

        loop_ = mk(L"\uE8EE", L"", 14, &loopIcon_);
        loop_.VerticalAlignment(VerticalAlignment::Center);
        loop_.HorizontalAlignment(HorizontalAlignment::Left);
        loop_.Click([self = weak_from_this()](auto&&, auto&&) { if (auto s = self.lock()) s->CycleLoop(); });
        Grid::SetColumn(loop_, 0);
        buttons.Children().Append(loop_);
        UpdateLoopUi();

        StackPanel vol;
        vol.Orientation(Orientation::Horizontal);
        vol.Spacing(6);
        vol.VerticalAlignment(VerticalAlignment::Center);
        Grid::SetColumn(vol, 2);
        mute_ = mk(L"", lycan::Tr(L"audio.mute_m"), 14, &muteIcon_);
        mute_.Click([self = weak_from_this()](auto&&, auto&&) { if (auto s = self.lock()) s->ToggleMute(); });
        volume_ = Slider{};
        volume_.Minimum(0);
        volume_.Maximum(100);
        volume_.Value(80);
        volume_.Width(110);
        volume_.VerticalAlignment(VerticalAlignment::Center);
        Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(volume_, lycan::Tr(L"audio.volume"));
        volume_.ValueChanged([self = weak_from_this()](auto&&, auto&&) { if (auto s = self.lock()) s->ApplyVolume(); });
        vol.Children().Append(mute_);
        vol.Children().Append(volume_);
        buttons.Children().Append(vol);
        controls.Children().Append(buttons);
        Grid::SetRow(controls, 1);
        right.Children().Append(controls);
        root_.Children().Append(right);

        // keyboard (accelerators work wherever the focus is)
        auto key = [&](VK k, auto&& act, VM m = VM::None) {
            Input::KeyboardAccelerator ka;
            ka.Key(k);
            ka.Modifiers(m);
            ka.Invoked([self = weak_from_this(), act](auto&&, Input::KeyboardAcceleratorInvokedEventArgs const& e) {
                if (auto s = self.lock()) { act(*s); e.Handled(true); }
            });
            root_.KeyboardAccelerators().Append(ka);
        };
        key(VK::Escape, [](AudioWindow& s) { s.Close(); });
        key(VK::Space, [](AudioWindow& s) { s.TogglePlay(); });
        key(VK::Left, [](AudioWindow& s) { s.SeekBy(-5); });
        key(VK::Right, [](AudioWindow& s) { s.SeekBy(+5); });
        key(VK::Left, [](AudioWindow& s) { s.Step(-1); }, VM::Control);
        key(VK::Right, [](AudioWindow& s) { s.Step(+1); }, VM::Control);
        key(VK::PageUp, [](AudioWindow& s) { s.Step(-1); });
        key(VK::PageDown, [](AudioWindow& s) { s.Step(+1); });
        key(VK::Up, [](AudioWindow& s) { s.volume_.Value(std::min(100.0, s.volume_.Value() + 5)); });
        key(VK::Down, [](AudioWindow& s) { s.volume_.Value(std::max(0.0, s.volume_.Value() - 5)); });
        key(VK::M, [](AudioWindow& s) { s.ToggleMute(); });
        key(VK::L, [](AudioWindow& s) { s.CycleLoop(); });
        root_.KeyboardAcceleratorPlacementMode(Input::KeyboardAcceleratorPlacementMode::Hidden);

        if (auto tag = lycan::i18n::UiLanguageTag(); !tag.empty()) root_.Language(hstring{tag});  // the font fallback picks the fonts by the language
        win_.Content(root_);
        auto tb = win_.AppWindow().TitleBar();
        const Windows::UI::Color clear{0, 0, 0, 0};
        tb.ButtonBackgroundColor(clear);
        tb.ButtonInactiveBackgroundColor(clear);
        tb.ButtonForegroundColor(Windows::UI::Color{255, 255, 255, 255});
        tb.ButtonInactiveForegroundColor(Windows::UI::Color{255, 160, 160, 160});

        HWND h{};
        check_hresult(win_.as<::IWindowNative>()->get_WindowHandle(&h));
        const double scale = GetDpiForWindow(h) / 96.0;
        win_.AppWindow().Resize(Windows::Graphics::SizeInt32{static_cast<int32_t>(920 * scale), static_cast<int32_t>(580 * scale)});
    }

    void BringToFront() {
        if (!win_) return;
        { HWND hw{}; if (SUCCEEDED(win_.as<::IWindowNative>()->get_WindowHandle(&hw))) lycan_app::SuppressAltBeep(hw); }
        win_.Activate();
        { HWND hw{}; if (SUCCEEDED(win_.as<::IWindowNative>()->get_WindowHandle(&hw))) lycan_app::ForceForeground(hw); }
        try { win_.AppWindow().MoveInZOrderAtTop(); } catch (...) {}
    }

    // ---- player ------------------------------------------------------------------------------------------------
    void InitPlayer() {
        player_ = Windows::Media::Playback::MediaPlayer{};
        player_.AutoPlay(false);
        player_.CommandManager().IsEnabled(false);  // no system media overlay for a preview
        ApplyVolume();
        endedToken_ = player_.MediaEnded([weak = weak_from_this(), dq = dq_](auto&&, auto&&) {
            dq.TryEnqueue([weak] { if (auto s = weak.lock()) s->OnEnded(); });
        });
        failedToken_ = player_.MediaFailed([weak = weak_from_this(), dq = dq_](auto&&, Windows::Media::Playback::MediaPlayerFailedEventArgs const& e) {
            std::wstring msg{e.ErrorMessage()};
            dq.TryEnqueue([weak, msg] { if (auto s = weak.lock()) s->OnFailed(msg); });
        });
        timer_ = dq_.CreateTimer();
        timer_.Interval(std::chrono::milliseconds(250));
        timer_.Tick([weak = weak_from_this()](auto&&, auto&&) { if (auto s = weak.lock()) s->Tick(); });
        timer_.Start();
    }

    void ApplyVolume() {
        if (!player_) return;
        double v = muted_ ? 0.0 : volume_.Value() / 100.0;
        if (_wgetenv(L"LYCAN_TEST_MUTE")) v = 0.0;  // tests must not make noise
        try { player_.Volume(v); } catch (...) {}
        muteIcon_.Glyph(muted_ || volume_.Value() <= 0 ? L"" : L"");
    }
    void ToggleMute() {
        muted_ = !muted_;
        ApplyVolume();
    }

    void Start(AudioRequest req) {
        ++gen_;
        loopMode_ = std::clamp(req.loopMode, 0, 3);
        onLoopMode_ = req.onLoopMode;
        history_.clear();
        req_ = std::move(req);
        UpdateLoopUi();
        reader_.Reset(req_.archive, req_.open);
        auto names = single_threaded_observable_vector<Windows::Foundation::IInspectable>();
        // One object per file (not the name as a string): files with the same name in different folders must stay
        // different items of the list
        for (auto const& it : req_.items) {
            Microsoft::UI::Xaml::Controls::TextBlock t;
            t.Text(hstring{it.name});
            names.Append(t);
        }
        syncingList_ = true;
        list_.ItemsSource(names);
        syncingList_ = false;
        Play(std::min(req_.start, req_.items.empty() ? size_t{0} : req_.items.size() - 1));
    }

    void StopPlayer() {
        try {
            player_.Pause();
            player_.Source(nullptr);
        } catch (...) {}
        haveSource_ = false;
    }

    void SetStatus(const std::wstring& s, bool error = false) {
        status_.Text(hstring{s});
        status_.Foreground(error ? Microsoft::UI::Xaml::Media::SolidColorBrush{Windows::UI::Color{255, 255, 140, 120}}
                                 : Microsoft::UI::Xaml::Media::SolidColorBrush{Windows::UI::Color{255, 255, 255, 255}});
    }

    void ShowItem(size_t pos) {
        auto const& it = req_.items[pos];
        title_.Text(hstring{Stem(it.name)});
        artist_.Text(L"");
        album_.Text(L"");
        file_.Text(hstring{it.path});
        ShowTech(it, nullptr);
        win_.Title(hstring{std::wstring{lycan::Tr(L"main.audio_preview")} + L" - " + it.name});
        syncingList_ = true;
        list_.SelectedIndex(static_cast<int>(pos));
        list_.ScrollIntoView(list_.SelectedItem());
        syncingList_ = false;
        elapsed_.Text(L"0:00");
        total_.Text(L"0:00");
        updating_ = true;
        seek_.Value(0);
        seek_.IsEnabled(false);
        updating_ = false;
    }

    void Play(size_t pos) {
        if (req_.items.empty() || pos >= req_.items.size() || closed_) return;
        pos_ = pos;
        const uint64_t gen = ++gen_;
        StopPlayer();
        ShowItem(pos);
        auto const& it = req_.items[pos];
        if (it.size > kMaxBytes) {
            SetStatus(lycan::Tr(L"audio.file_too_large_preview_over_256"), true);
            return;
        }
        SetStatus(lycan::Tr(L"audio.reading"));
        LoadAsync(gen, pos);
    }

    fire_and_forget LoadAsync(uint64_t gen, size_t pos) {
        auto self = shared_from_this();
        const AudioItem item = req_.items[pos];
        co_await resume_background();
        std::vector<uint8_t> bytes;
        std::wstring err;
        const bool ok = reader_.Read(item.entryIndex, kMaxBytes, bytes, [&] { return !closed_ && gen_.load() == gen; }, err);
        lycan::AudioTags tags;
        Windows::Storage::Streams::InMemoryRandomAccessStream stream{nullptr};
        if (ok && !bytes.empty()) {
            tags = lycan::ReadAudioTags(bytes.data(), bytes.size());
            try {
                stream = Windows::Storage::Streams::InMemoryRandomAccessStream{};
                Windows::Storage::Streams::DataWriter w{stream};
                w.WriteBytes(array_view<const uint8_t>(bytes.data(), bytes.data() + bytes.size()));
                co_await w.StoreAsync();
                w.DetachStream();
                stream.Seek(0);
            } catch (...) {
                stream = nullptr;
            }
        }
        std::vector<uint8_t>().swap(bytes);  // the stream holds the data now
        co_await resume_ui{dq_};
        if (closed_ || gen_.load() != gen) co_return;
        if (!ok) {
            if (!err.empty()) SetStatus(err, true);
            co_return;
        }
        if (!stream) {
            SetStatus(lycan::Tr(L"audio.file_could_not_be_read"), true);
            co_return;
        }
        if (!tags.title.empty()) title_.Text(hstring{tags.title});
        artist_.Text(hstring{tags.artist});
        album_.Text(hstring{tags.album});
        try {
            auto source = Windows::Media::Core::MediaSource::CreateFromStream(stream, ContentType(item.name));
            Windows::Media::Playback::MediaPlaybackItem playItem{source};
            player_.Source(playItem);
            haveSource_ = true;
            SetStatus(L"");
            player_.Play();
            // the track list is there once the source is open (the decoder of Windows tells what the file is)
            try { co_await source.OpenAsync(); } catch (...) {}
            if (closed_ || gen_.load() != gen) co_return;
            ShowTech(item, &playItem, source.Duration() ? source.Duration().Value().count() / 10000000.0 : 0.0);
        } catch (winrt::hresult_error const& e) {
            OnFailed(std::wstring{e.message()});
        }
    }

    // The card where a cover picture would be. `playItem` null = nothing is known yet (only the size and the kind of file).
    void ShowTech(const AudioItem& item, Windows::Media::Playback::MediaPlaybackItem const* playItem, double seconds = 0.0) {
        techCard_.Children().Clear();
        techCard_.RowDefinitions().Clear();
        int row = 0;
        auto add = [&](const wchar_t* labelId, const std::wstring& value) {
            if (value.empty()) return;
            RowDefinition rd;
            rd.Height(GridLength{0, GridUnitType::Auto});
            techCard_.RowDefinitions().Append(rd);
            TextBlock k, v;
            k.Text(hstring{lycan::Tr(labelId)});
            k.FontSize(12);
            k.Opacity(0.5);
            k.HorizontalAlignment(HorizontalAlignment::Right);
            v.Text(hstring{value});
            v.FontSize(12);
            v.Opacity(0.85);
            Grid::SetRow(k, row);
            Grid::SetRow(v, row);
            Grid::SetColumn(v, 1);
            techCard_.Children().Append(k);
            techCard_.Children().Append(v);
            ++row;
        };
        std::wstring ext = std::filesystem::path(item.name).extension().wstring();
        if (!ext.empty()) ext.erase(0, 1);
        for (auto& c : ext) c = static_cast<wchar_t>(towupper(c));
        std::wstring codec;
        uint32_t bitrate = 0, rate = 0, channels = 0;
        if (playItem && playItem->AudioTracks().Size() > 0) {
            try {
                auto ep = playItem->AudioTracks().GetAt(0).GetEncodingProperties();
                codec = std::wstring{ep.Subtype()};
                bitrate = ep.Bitrate();
                rate = ep.SampleRate();
                channels = ep.ChannelCount();
                if (!codec.empty() && codec[0] == L'{') codec.clear();  // a GUID, not a name
            } catch (...) {}
        }
        add(L"audio.info_format", codec.empty() || _wcsicmp(codec.c_str(), ext.c_str()) == 0 ? ext : ext + L" \u00b7 " + codec);
        if (bitrate) add(L"audio.info_bitrate", lycan::Tf(L"audio.kbps", (bitrate + 500) / 1000));
        if (rate) {
            wchar_t b[24];
            swprintf_s(b, L"%.1f", rate / 1000.0);
            std::wstring khz = b;
            if (khz.size() > 2 && khz.compare(khz.size() - 2, 2, L".0") == 0) khz.resize(khz.size() - 2);
            add(L"audio.info_sample_rate", lycan::Tf(L"audio.khz", khz));
        }
        if (channels) add(L"audio.info_channels", channels == 1 ? std::wstring{lycan::Tr(L"audio.mono")} : channels == 2 ? std::wstring{lycan::Tr(L"audio.stereo")} : lycan::Tf(L"audio.channels_n", channels));
        if (seconds > 0) add(L"audio.info_length", Clock(seconds));
        {
            wchar_t b[32];
            if (item.size >= (1ull << 20)) swprintf_s(b, L"%.1f MB", item.size / 1048576.0);
            else swprintf_s(b, L"%.0f KB", item.size / 1024.0);
            add(L"audio.info_size", b);
        }
    }

    void OnFailed(const std::wstring& message) {
        if (closed_) return;
        std::wstring text = lycan::Tr(L"audio.pc_cannot_play_file_decoder_format");
        if (!message.empty()) text += L"\n" + message;
        SetStatus(text, true);
        haveSource_ = false;
    }

    // ---- the playback mode: 0 play on and stop after the last track, 1 repeat all, 2 repeat the track, 3 shuffle
    void UpdateLoopUi() {
        if (!loop_) return;
        static const wchar_t* const kGlyphs[] = {L"\uE8EE", L"\uE8EE", L"\uE8ED", L"\uE8B1"};
        const wchar_t* ids[] = {L"audio.loop_none", L"audio.loop_all", L"audio.loop_one", L"audio.shuffle"};
        const int m = std::clamp(loopMode_, 0, 3);
        loopIcon_.Glyph(kGlyphs[m]);
        loop_.Opacity(m == 0 ? 0.55 : 1.0);   // a mode that is "on" looks on
        const hstring tip{lycan::Tf(L"audio.playback_mode_tip", lycan::Tr(ids[m]))};
        Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(loop_, box_value(tip));
        Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(loop_, tip);
    }

    void CycleLoop() {
        loopMode_ = (loopMode_ + 1) % 4;
        history_.clear();
        UpdateLoopUi();
        if (onLoopMode_) onLoopMode_(loopMode_);
    }

    void PlayRandom() {
        const size_t n = req_.items.size();
        if (n < 2) { Play(pos_); return; }
        size_t r = static_cast<size_t>(rng_() % (n - 1));
        if (r >= pos_) ++r;   // another track than the one that played
        history_.push_back(pos_);
        if (history_.size() > 200) history_.erase(history_.begin());
        Play(r);
    }

    void OnEnded() {
        if (closed_) return;
        if (loopMode_ == 2) { Play(pos_); return; }          // repeat the track
        if (loopMode_ == 3) { PlayRandom(); return; }        // shuffle
        if (pos_ + 1 < req_.items.size()) Play(pos_ + 1);
        else if (loopMode_ == 1 && !req_.items.empty()) Play(0);   // repeat all: round to the first
        else {
            SetStatus(lycan::Tr(L"audio.reached_last_track"));
            playIcon_.Glyph(L"");
        }
    }

    void Step(int d) {
        const int64_t n = static_cast<int64_t>(req_.items.size());
        if (n < 2) return;
        if (loopMode_ == 3) {   // shuffle: next = another one by chance, previous = the one that played before
            if (d > 0) PlayRandom();
            else if (!history_.empty()) { const size_t p = history_.back(); history_.pop_back(); Play(p); }
            else PlayRandom();
            return;
        }
        Play(static_cast<size_t>(((static_cast<int64_t>(pos_) + d) % n + n) % n));
    }

    void TogglePlay() {
        if (!haveSource_) return;
        try {
            using S = Windows::Media::Playback::MediaPlaybackState;
            auto session = player_.PlaybackSession();
            if (session.PlaybackState() == S::Playing || session.PlaybackState() == S::Buffering) player_.Pause();
            else {
                // after the end of the last track, play it again from the start
                if (session.NaturalDuration().count() > 0 && session.Position() >= session.NaturalDuration()) session.Position(Windows::Foundation::TimeSpan{0});
                player_.Play();
            }
        } catch (...) {}
        Tick();
    }

    void SeekBy(double seconds) {
        if (!haveSource_) return;
        try {
            auto session = player_.PlaybackSession();
            const double dur = Seconds(session.NaturalDuration());
            const double now = Seconds(session.Position());
            const double to = std::clamp(now + seconds, 0.0, dur > 0 ? dur : now + seconds);
            session.Position(std::chrono::duration_cast<Windows::Foundation::TimeSpan>(std::chrono::duration<double>(to)));
        } catch (...) {}
        Tick();
    }

    // every 250 ms: position, duration, play / pause button
    void Tick() {
        if (closed_ || !player_) return;
        try {
            using S = Windows::Media::Playback::MediaPlaybackState;
            auto session = player_.PlaybackSession();
            const bool playing = haveSource_ && (session.PlaybackState() == S::Playing || session.PlaybackState() == S::Buffering);
            if (playing != playingShown_ || firstTick_) {
                playingShown_ = playing;
                firstTick_ = false;
                playIcon_.Glyph(playing ? L"" : L"");
                const hstring tip{playing ? lycan::Tr(L"main.pause") : lycan::Tr(L"audio.play")};
                Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(play_, box_value(tip));
                Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(play_, tip);
            }
            if (!haveSource_) return;
            const double dur = Seconds(session.NaturalDuration());
            const double pos = Seconds(session.Position());
            updating_ = true;
            if (dur > 0) {
                seek_.IsEnabled(true);
                seek_.Maximum(dur);
                seek_.Value(std::min(pos, dur));
                total_.Text(hstring{Clock(dur)});
            }
            elapsed_.Text(hstring{Clock(pos)});
            updating_ = false;
        } catch (...) {
            updating_ = false;
        }
    }

    // stops the sound and everything that runs by itself
    void Shutdown() {
        closed_ = true;
        ++gen_;
        try {
            if (timer_) timer_.Stop();
            if (player_) {
                player_.MediaEnded(endedToken_);
                player_.MediaFailed(failedToken_);
                player_.Pause();
                player_.Source(nullptr);
                player_.Close();
            }
        } catch (...) {}
        player_ = nullptr;
    }

    Window win_{nullptr};
    Grid root_{nullptr}, titleBar_{nullptr};
    ListView list_{nullptr};
    TextBlock title_{nullptr}, artist_{nullptr}, album_{nullptr}, file_{nullptr}, status_{nullptr}, elapsed_{nullptr}, total_{nullptr};
    Slider seek_{nullptr}, volume_{nullptr};
    Button prev_{nullptr}, play_{nullptr}, next_{nullptr}, mute_{nullptr}, loop_{nullptr};
    FontIcon playIcon_{nullptr}, muteIcon_{nullptr}, loopIcon_{nullptr};
    Grid techCard_{nullptr};   // the technical data of the file that plays (format, bit rate, sample rate, channels, length, size)
    int loopMode_ = 0;
    std::function<void(int)> onLoopMode_;
    std::vector<size_t> history_;   // shuffle: what played before (for "previous")
    std::mt19937 rng_{std::random_device{}()};
    Windows::Media::Playback::MediaPlayer player_{nullptr};
    winrt::event_token endedToken_{}, failedToken_{};
    Microsoft::UI::Dispatching::DispatcherQueue dq_{nullptr};
    Microsoft::UI::Dispatching::DispatcherQueueTimer timer_{nullptr};
    AudioRequest req_;
    Reader reader_;
    size_t pos_ = 0;
    std::atomic<uint64_t> gen_{0};
    bool closed_ = false, haveSource_ = false, muted_ = false, syncingList_ = false, updating_ = false, playingShown_ = false, firstTick_ = true;
};

std::map<const void*, std::shared_ptr<AudioWindow>> g_windows;

void AudioWindow::OnClosed() {
    if (auto done = std::move(req_.onClosed)) done();
    Shutdown();
    // not while handling the event: drop our own entry on the next turn (the owner may already have a newer player)
    auto dq = Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
    dq.TryEnqueue([key = owner_, self = this] {
        auto it = g_windows.find(key);
        if (it != g_windows.end() && it->second.get() == self) g_windows.erase(it);
    });
}

}  // namespace

bool IsPreviewableAudioName(const std::wstring& name) {
    static const wchar_t* const kExt[] = {L".mp3", L".m4a", L".m4b", L".aac", L".wav", L".flac", L".ogg", L".oga",
                                          L".opus", L".wma", L".aif", L".aiff", L".aifc", L".mka"};
    const auto e = LowerExt(name);
    for (auto* x : kExt)
        if (e == x) return true;
    return false;
}

const wchar_t* AudioContentType(const std::wstring& name) { return ContentType(name); }

void ShowAudioPreview(AudioRequest req) {
    if (req.items.empty()) return;
    const void* owner = req.owner;
    if (auto it = g_windows.find(owner); it != g_windows.end() && !it->second->IsClosed()) {
        it->second->Apply(std::move(req));
        return;
    }
    auto w = std::make_shared<AudioWindow>();
    w->owner_ = owner;
    g_windows[owner] = w;
    w->Create(std::move(req));
}

void CloseAudioPreview(const void* owner) {
    if (!owner) {
        auto all = std::move(g_windows);
        g_windows.clear();
        for (auto& [k, w] : all) w->Close();
        return;
    }
    auto it = g_windows.find(owner);
    if (it == g_windows.end()) return;
    auto w = it->second;
    g_windows.erase(it);
    w->Close();
}

}  // namespace lycan_app
