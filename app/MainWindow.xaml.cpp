#include "pch.h"
#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif
#include "MainWindowShared.h"

namespace winrt::LycanZip::implementation
{
    static LRESULT CALLBACK MinSizeProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);


    MainWindow::MainWindow()
    {
        lycan::diag::Info(L"window: created");
        InitializeComponent();
        lycan_app::TranslateTree(Content());
        if (auto tag = lycan::i18n::UiLanguageTag(); !tag.empty()) Content().as<FrameworkElement>().Language(hstring{tag});  // the font fallback picks the fonts by the language (Japanese kanji forms)
        {
            // The settings search box is at least as wide as its placeholder ("Rechercher dans les paramètres (Ctrl+F)" is longer
            // than half of the free space); the empty column left of it gives way.
            TextBlock t;
            t.Text(SettingsSearch().PlaceholderText());
            t.Measure(Windows::Foundation::Size{10000, 10000});
            const double need = std::clamp(std::ceil(static_cast<double>(t.DesiredSize().Width)) + 56, 160.0, 400.0);  // + the padding and the search button
            SettingsSearchColumn().MinWidth(need);
            SettingsSearchColumn().MaxWidth(std::max(need, 320.0));
        }
        dispatcher_ = DispatcherQueue();
        taskRows_ = single_threaded_observable_vector<Windows::Foundation::IInspectable>();  // must be an "observable" collection, so that added / removed rows are shown
        TaskList().ItemsSource(taskRows_);

        LoadBrandImages();
        ExtendsContentIntoTitleBar(true);
        // No SetTitleBar: the whole strip would be the caption, and the settings button inside it sometimes ignored a click
        // (a second quick click counted as a double-click on the caption and maximized the window). Only the empty parts
        // of the strip are drag rectangles; the button is plain client area.
        AppTitleBar().SizeChanged([weak = get_weak()](auto&&, auto&&) { if (auto self = weak.get()) self->UpdateTitleDragRects(); });
        ResizeToDefault();
        UpdateTitleDragRects();

        // File name encoding menu
        struct Enc { const wchar_t* label; unsigned cp; };
        for (auto const& e : {Enc{lycan::Tr(L"main.auto_detect"), 0}, Enc{L"UTF-8", 65001}, Enc{lycan::Tr(L"main.big5_traditional_chinese"), 950}, Enc{lycan::Tr(L"main.gbk_simplified_chinese"), 936},
                              Enc{lycan::Tr(L"main.shift_jis_japanese"), 932}, Enc{lycan::Tr(L"main.euc_kr_korean"), 949}})
        {
            ComboBoxItem item;
            item.Content(box_value(e.label));
            item.Tag(box_value(static_cast<int32_t>(e.cp)));
            EncodingCombo().Items().Append(item);
        }
        EncodingCombo().SelectedIndex(0);

        settingsPath_ = lycan::DefaultSettingsPath();
        settings_ = lycan::LoadSettings(settingsPath_);
        lycan_app::PasswordVault::Get().SetFile(settingsPath_.parent_path() / L"passwords.dat");   // the saved passwords live next to the settings
        // the names of the file types follow the language of the program (only the names are rewritten, not where the registration points to)
        if (GetEnvironmentVariableW(L"LYCAN_NO_STALE_CHECK", nullptr, 0) == 0 && !StepAsideForStore()) lycan::UpdateAssociationNames(AssocOptions());
        StampSettings();
        RootGrid().ActualThemeChanged([weak = get_weak()](auto&&, auto&&) { if (auto self = weak.get()) self->UpdateBackdrop(); });
        // the twinkling stars of the home page rest while the window is not the active one (minimized included)
        Activated([weak = get_weak()](auto&&, WindowActivatedEventArgs const& e) {
            if (auto self = weak.get()) { self->windowActive_ = e.WindowActivationState() != WindowActivationState::Deactivated; self->UpdateTwinkle(); }
        });
        // a contrast theme switched on or off while the program runs: the background goes away / comes back, and the rows get their colours again
        hcWatch_ = lycan_app::WatchHighContrast([weak = get_weak()] {
            if (auto self = weak.get())
                self->dispatcher_.TryEnqueue([weak] {
                    if (auto s = weak.get()) { s->UpdateBackdrop(); if (s->view_ == View::Archive) s->RefreshList(); }
                });
        });
        BuildAccentPicker();
        SettingsTabs().SelectedItem(TabItemGeneral());
        InitCompressPage();
        InitShortcuts();
        BuildShellItemsUi();
        this->Closed([weak = get_weak(), id = static_cast<const void*>(this)](auto&&, auto&&) {
            lycan_app::CloseImagePreview(id);  // close only the previews that belong to this window
            lycan_app::CloseAudioPreview(id);
            lycan_app::SettingsHub::Get().Unsubscribe(id);
            if (auto self = weak.get()) { self->notifier_.Detach(); if (self->previewPane_) self->previewPane_->CloseLarge(); }
        });
        EntryList().SizeChanged([weak = get_weak()](auto&&, auto&&) { if (auto s = weak.get()) s->UpdateListColumns(); });
        InitTreeSplitter();
        InitPreviewPane();
        InitListHeader();
        SetWindowSubclass(Hwnd(), MinSizeProc, 1, reinterpret_cast<DWORD_PTR>(&compactFlag_));
        lycan_app::SuppressAltBeep(Hwnd());
        notifier_.Attach(Hwnd());
        // Several windows: when another window saved the settings, apply them at once (see SettingsHub.h)
        lycan_app::SettingsHub::Get().Subscribe(this, [weak = get_weak()](lycan::Settings const& s) {
            if (auto self = weak.get()) self->ApplyExternalSettings(s);
        });
        // Closing the window while tasks are running needs a confirmation first; with no tasks it closes at once
        AppWindow().Closing([weak = get_weak()](Microsoft::UI::Windowing::AppWindow const&, Microsoft::UI::Windowing::AppWindowClosingEventArgs const& e) {
            auto self = weak.get();
            if (!self || self->closeConfirmed_) return;
            const int n = self->UnfinishedTaskCount();
            if (n == 0)
            {
                if (!self->HasUnsavedEdits()) return;
                e.Cancel(true);
                self->AskCloseWithEditsAsync();
                return;
            }
            e.Cancel(true);
            self->AskCloseWithTasksAsync(n);
        });
        LoadSettingsToUi();
        ApplySettings();
        ApplyAccent();

        queue_.SetListener([weak = get_weak()](lycan::TaskInfo const& info) {
            if (auto self = weak.get()) self->Post([self, info] { self->OnTaskUpdate(info); });
        });
        // The list items handle the click first (toggle the check) and mark the event as handled, so a double click is received only with handledEventsToo
        for (UIElement list : {EntryList().as<UIElement>(), EntryGrid().as<UIElement>()})
        {
            list.AddHandler(UIElement::DoubleTappedEvent(), box_value(Input::DoubleTappedEventHandler{this, &MainWindow::OnEntryDoubleTapped}), true);
            list.AddHandler(UIElement::PointerPressedEvent(), box_value(Input::PointerEventHandler{this, &MainWindow::OnBandPressed}), true);
            list.AddHandler(UIElement::PointerMovedEvent(), box_value(Input::PointerEventHandler{this, &MainWindow::OnBandMoved}), true);
            list.AddHandler(UIElement::PointerReleasedEvent(), box_value(Input::PointerEventHandler{this, &MainWindow::OnBandReleased}), true);
            list.AddHandler(UIElement::PointerCanceledEvent(), box_value(Input::PointerEventHandler{this, &MainWindow::OnBandReleased}), true);
            list.AddHandler(UIElement::PointerCaptureLostEvent(), box_value(Input::PointerEventHandler{this, &MainWindow::OnBandCaptureLost}), true);
        }
        CleanupStaleTemp();
    }

    // While a window shows an archive, the file is held open for reading (others may read it, nobody may write to it or delete it): a
    // download or a sync program cannot swap the content under a list that was read earlier. Not in the compact window of the context
    // menu (it is gone in a moment). Released before this program changes or deletes the file itself, and when the archive is closed.
    void MainWindow::LockArchiveFile(fs::path const& path)
    {
        UnlockArchiveFile();
        if (!settings_.lockOpenArchive || compact_ || path.empty()) return;
        archiveLock_ = CreateFileW(lycan::ToLongPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (archiveLock_ == INVALID_HANDLE_VALUE) lycan::diag::Debug(L"archive lock: not taken ({})", GetLastError());
    }

    void MainWindow::UnlockArchiveFile()
    {
        if (archiveLock_ != INVALID_HANDLE_VALUE) { CloseHandle(archiveLock_); archiveLock_ = INVALID_HANDLE_VALUE; }
    }

    MainWindow::~MainWindow()
    {
        UnlockArchiveFile();
        lycan::diag::Info(L"window: destroyed");
        lycan_app::SettingsHub::Get().Unsubscribe(this);
        closing_ = true;  // lets a worker thread that waits for the user to answer a conflict / dialog give up the wait, so that queue_ can end
        if (aggTimer_) aggTimer_.Stop();
        if (closeTimer_) closeTimer_.Stop();
    }

    void MainWindow::Post(std::function<void()> fn)
    {
        dispatcher_.TryEnqueue([fn = std::move(fn)] { fn(); });
    }

    HWND MainWindow::Hwnd()
    {
        HWND h{};
        check_hresult(this->try_as<::IWindowNative>()->get_WindowHandle(&h));
        return h;
    }

    // The window size is in DIP, converted to pixels by the screen scale; not more than 95% of the work area
    void MainWindow::ResizeDip(int widthDip, int heightDip)
    {
        HWND h = Hwnd();
        double scale = GetDpiForWindow(h) / 96.0;
        MONITORINFO mi{sizeof(mi)};
        int w = static_cast<int>(widthDip * scale), ht = static_cast<int>(heightDip * scale);
        if (GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi))
        {
            w = std::min(w, static_cast<int>((mi.rcWork.right - mi.rcWork.left) * 0.95));
            ht = std::min(ht, static_cast<int>((mi.rcWork.bottom - mi.rcWork.top) * 0.95));
        }
        AppWindow().Resize({w, ht});
    }

    void MainWindow::ResizeToDefault() { ResizeDip(1000, 700); }

    // The width of the compact window: 620 DIP fit the buttons under the task in English ("Open folder", "Test integrity", "Delete the
    // originals"); a language with longer texts (German, French, Spanish) gets as much more as its buttons are wider than the English ones.
    int MainWindow::CompactWidthDip()
    {
        static const int width = [] {
            auto measure = [](std::wstring const& s) {
                TextBlock t;
                t.Text(hstring{s});
                t.Measure(Windows::Foundation::Size{10000, 10000});
                return static_cast<double>(t.DesiredSize().Width);
            };
            double shown = 0, english = 0;
            for (const wchar_t* id : {L"main.open_folder", L"main.test_integrity", L"main.delete_originals"})
            {
                const std::wstring text = lycan::Tr(id);
                const std::wstring en = lycan::i18n::EnglishOf(text);
                shown += measure(text);
                english += measure(en.empty() ? text : en);
            }
            const double extra = shown - english;
            return extra > 0 ? 620 + static_cast<int>(std::ceil(extra)) + 12 : 620;  // a little room to spare at the right edge
        }();
        return width;
    }

    // The caption (drag) area is the title strip without the settings button and without the system buttons; the button
    // keeps clear of the system buttons by the inset the window reports instead of a fixed margin.
    void MainWindow::UpdateTitleDragRects()
    {
        try {
            const double scale = GetDpiForWindow(Hwnd()) / 96.0;
            auto tb = AppWindow().TitleBar();
            const double right = tb.RightInset() / scale, left = tb.LeftInset() / scale;
            const double width = AppTitleBar().ActualWidth(), height = AppTitleBar().ActualHeight();
            if (width <= 0 || height <= 0) return;
            const bool button = SettingsButton().Visibility() == Visibility::Visible;
            const double bw = button ? 40.0 : 0.0, margin = right + 8;
            if (button) SettingsButton().Margin(ThicknessHelper::FromLengths(0, 0, margin, 0));
            const double bl = width - margin - bw;  // left edge of the button
            std::vector<Windows::Graphics::RectInt32> rects;
            auto add = [&](double x0, double x1) {
                if (x1 - x0 < 1) return;
                rects.push_back({static_cast<int32_t>(x0 * scale), 0, static_cast<int32_t>((x1 - x0) * scale), static_cast<int32_t>(height * scale)});
            };
            if (button) { add(left, bl); add(bl + bw, width - right); }
            else add(left, width - right);
            tb.SetDragRectangles(array_view<const Windows::Graphics::RectInt32>(rects.data(), rects.data() + rects.size()));
        } catch (...) {}
    }

    // Brand pictures are in Assets\ next to the exe (see CopyBrandAssets in the vcxproj); loaded by file path; if not found they stay blank (no effect on functions)
    void MainWindow::LoadBrandImages()
    {
        wchar_t exe[MAX_PATH * 2];
        DWORD n = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
        fs::path dir = fs::path(std::wstring(exe, n)).parent_path() / L"Assets";
        auto load = [&](Image const& img, const wchar_t* file) {
            std::error_code ec;
            fs::path p = dir / file;
            if (!fs::exists(p, ec)) return;
            Microsoft::UI::Xaml::Media::Imaging::BitmapImage bmp{Windows::Foundation::Uri{hstring{p.wstring()}}};
            img.Source(bmp);
        };
        load(TitleLogo(), L"logo-small-32.png");
        load(PreviewWolf(), L"logo-small-32.png");  // the wolf marks the features that are LycanZip's own
        load(HomeLogo(), L"logo-icon-256.png");   // the same icon as the program itself
        load(AboutLogo(), L"logo-icon-128.png");
    }

    // The file-type icon of an archive (the family of its extension, lycan/fileicons.h): a 32 px PNG of Assets\FileIcons, null when it is missing
    static Microsoft::UI::Xaml::Media::Imaging::BitmapImage FileIconImage(fs::path const& archive)
    {
        return lycan_app::FileIconImage(lycan::IconFamilyForPath(archive.wstring()), 32);
    }

    // The icon in front of the breadcrumb: the archive's file-type icon (the generic glyph while there is none)
    void MainWindow::UpdateArchiveIcon()
    {
        auto bmp = FileIconImage(currentPath_);
        PathIcon().Source(bmp);
        PathIcon().Visibility(bmp ? Visibility::Visible : Visibility::Collapsed);
        PathGlyph().Visibility(bmp ? Visibility::Collapsed : Visibility::Visible);
    }

    void MainWindow::SetBusy(bool busy)
    {
        BusyRing().IsActive(busy);
        BusyRing().Visibility(busy ? Visibility::Visible : Visibility::Collapsed);
    }


    // ---------------------------------------------------------------- Minimum window size

    // A normal window must not get so small that the layout breaks (only the title bar left); the compact window (context menu action) is small anyway, so its lower limit is relaxed.
    // ref is the compactFlag_ of this window (several windows: one for each window)
    static LRESULT CALLBACK MinSizeProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref)
    {
        if (msg == WM_GETMINMAXINFO)
        {
            const double scale = GetDpiForWindow(hwnd) / 96.0;
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            const bool compact = reinterpret_cast<std::atomic<bool>*>(ref)->load();
            mmi->ptMinTrackSize.x = static_cast<LONG>((compact ? 420 : 640) * scale);
            mmi->ptMinTrackSize.y = static_cast<LONG>((compact ? 220 : 480) * scale);
            return 0;
        }
        return DefSubclassProc(hwnd, msg, wp, lp);
    }

    // Upper limit of the height of the task panel: about 22% of the window height (at least 90, at most 240 DIP); above that a scroll bar is used
    void MainWindow::UpdateTaskPanelMax()
    {
        if (compact_) return;
        TaskScroll().MaxHeight(std::clamp(RootGrid().ActualHeight() * 0.22, 90.0, 240.0));
    }

    // ---------------------------------------------------------------- Motion and sound

    // Played only when the setting is on and the Windows "show animations" is on too
    bool MainWindow::AnimOn() const
    {
        if (!settings_.animations) return false;
        try { return Windows::UI::ViewManagement::UISettings{}.AnimationsEnabled(); } catch (...) { return true; }
    }

    // Fade in and slide in from below (dy is the starting offset, DIP)
    void MainWindow::FadeIn(UIElement const& el, double dy, int ms, int delayMs)
    {
        if (!AnimOn() || !el) return;
        using namespace Microsoft::UI::Xaml::Media;
        using namespace Microsoft::UI::Xaml::Media::Animation;
        auto ct = el.RenderTransform().try_as<CompositeTransform>();
        if (!ct) { ct = CompositeTransform{}; el.RenderTransform(ct); }
        el.Opacity(0);
        ct.TranslateY(dy);
        Storyboard sb;
        auto make = [&](DependencyObject const& target, const wchar_t* prop, double from, double to) {
            DoubleAnimation a;
            a.From(from);
            a.To(to);
            a.Duration(DurationHelper::FromTimeSpan(std::chrono::milliseconds(ms)));
            a.BeginTime(Windows::Foundation::TimeSpan{std::chrono::milliseconds(delayMs)});
            CubicEase ease;
            ease.EasingMode(EasingMode::EaseOut);
            a.EasingFunction(ease);
            Storyboard::SetTarget(a, target);
            Storyboard::SetTargetProperty(a, prop);
            sb.Children().Append(a);
        };
        make(el, L"Opacity", 0.0, 1.0);
        if (dy != 0) make(ct, L"TranslateY", dy, 0.0);
        sb.Completed([el, ct, sb](auto&&, auto&&) {
            sb.Stop();  // the held value of an animation would override later settings: release it at the end and write back the final value
            el.Opacity(1);
            ct.TranslateY(0);
        });
        sb.Begin();
    }

    // Sound: Assets\Sounds\<name>.wav (placeholders for now, replace them with files of the same name). Off by default, only one plays at a time
    void MainWindow::PlaySfx(const wchar_t* name)
    {
        if (!settings_.soundEffects) { lycan::diag::Debug(L"sound {}: skipped, sound effects are off", name); return; }
        wchar_t exe[MAX_PATH];
        DWORD n = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
        if (n == 0 || n >= std::size(exe)) return;
        auto path = fs::path(exe).parent_path() / L"Assets" / L"Sounds" / (settings_.soundSet == lycan::SoundSet::Wave ? L"Wave" : L"Synth") / (std::wstring(name) + L".wav");
        const bool muted = GetEnvironmentVariableW(L"LYCAN_TEST_MUTE", nullptr, 0) > 0;  // tests must not make noise
        const BOOL queued = muted ? TRUE : PlaySoundW(path.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
        lycan::diag::Info(L"sound {} ({}): {}{}", name, settings_.soundSet == lycan::SoundSet::Wave ? L"Wave" : L"Synth", queued ? L"queued" : L"NOT played", muted ? L" (muted by the test)" : L"");
    }

    // When files are dragged into the window, the halo of the start page grows and glows
    void MainWindow::SetDragGlow(bool on)
    {
        if (dragGlow_ == on) return;
        dragGlow_ = on;
        if (!AnimOn() || view_ != View::Home) return;
        using namespace Microsoft::UI::Xaml::Media;
        using namespace Microsoft::UI::Xaml::Media::Animation;
        auto halo = MoonHalo();
        auto ct = halo.RenderTransform().try_as<CompositeTransform>();
        if (!ct) { ct = CompositeTransform{}; halo.RenderTransform(ct); halo.RenderTransformOrigin({0.5, 0.5}); }
        Storyboard sb;
        auto make = [&](DependencyObject const& target, const wchar_t* prop, double to) {
            DoubleAnimation a;
            a.To(to);
            a.Duration(DurationHelper::FromTimeSpan(std::chrono::milliseconds(260)));
            CubicEase ease;
            ease.EasingMode(EasingMode::EaseOut);
            a.EasingFunction(ease);
            Storyboard::SetTarget(a, target);
            Storyboard::SetTargetProperty(a, prop);
            sb.Children().Append(a);
        };
        make(ct, L"ScaleX", on ? 1.35 : 1.0);
        make(ct, L"ScaleY", on ? 1.35 : 1.0);
        sb.Begin();
    }

    // When a task is done, the frame of the task panel flashes in the accent colour
    void MainWindow::PulseTaskPanel()
    {
        if (!AnimOn() || compact_) return;
        using namespace Microsoft::UI::Xaml::Media;
        using namespace Microsoft::UI::Xaml::Media::Animation;
        SolidColorBrush brush{AccentColorNow()};
        TaskPanel().BorderBrush(brush);
        ColorAnimation a;
        a.From(AccentColorNow());
        a.To(Windows::UI::Color{0x30, 0xFF, 0xFF, 0xFF});
        a.Duration(DurationHelper::FromTimeSpan(std::chrono::milliseconds(900)));
        Storyboard::SetTarget(a, brush);
        Storyboard::SetTargetProperty(a, L"Color");
        Storyboard sb;
        sb.Children().Append(a);
        sb.Completed([weak = get_weak()](auto&&, auto&&) {
            if (auto self = weak.get()) self->TaskPanel().ClearValue(Border::BorderBrushProperty());
        });
        sb.Begin();
    }

    // The moon of a task card glows once when the task ends: moonlight when it is done, red (twice) when it failed. Also in the compact window (the same list,
    // another template). The card is the data of a template: its element is found through the container of the list, after the layout (a task that ends at
    // once may have no element yet). A card that is not on screen does not glow.
    void MainWindow::GlowMoon(LycanZip::TaskRow const& row, bool failed)
    {
        if (!AnimOn()) return;
        dispatcher_.TryEnqueue(Microsoft::UI::Dispatching::DispatcherQueuePriority::Low, [weak = get_weak(), row, failed] {
            auto self = weak.get();
            if (!self) return;
            using namespace Microsoft::UI::Xaml::Media;
            using namespace Microsoft::UI::Xaml::Media::Animation;
            auto container = self->TaskList().ContainerFromItem(row).try_as<DependencyObject>();
            if (!container || VisualTreeHelper::GetChildrenCount(container) == 0) return;
            auto root = VisualTreeHelper::GetChild(container, 0).try_as<FrameworkElement>();
            auto glow = root ? root.FindName(L"MoonGlow").try_as<Microsoft::UI::Xaml::Shapes::Ellipse>() : nullptr;
            lycan::diag::Debug(L"moon glow ({}): {}", failed ? L"failed" : L"done", glow ? L"played" : L"no element");
            if (!glow) return;
            const Windows::UI::Color c = failed ? Windows::UI::Color{255, 0xFF, 0x6B, 0x6B} : Windows::UI::Color{255, 0xF4, 0xF8, 0xFF};
            RadialGradientBrush brush;
            GradientStop inner, outer;
            inner.Color(c);
            inner.Offset(0.4);
            outer.Color(Windows::UI::Color{0, c.R, c.G, c.B});
            outer.Offset(1);
            brush.GradientStops().Append(inner);
            brush.GradientStops().Append(outer);
            glow.Fill(brush);
            auto ct = glow.RenderTransform().try_as<CompositeTransform>();
            if (!ct) { ct = CompositeTransform{}; glow.RenderTransform(ct); }
            Storyboard sb;
            DoubleAnimationUsingKeyFrames op;
            auto key = [&](DoubleAnimationUsingKeyFrames& k, int ms, double v) {
                EasingDoubleKeyFrame f;
                f.KeyTime(KeyTimeHelper::FromTimeSpan(std::chrono::milliseconds(ms)));
                f.Value(v);
                k.KeyFrames().Append(f);
            };
            key(op, 0, 0.0);
            if (failed) { key(op, 140, 0.9); key(op, 300, 0.15); key(op, 440, 0.9); key(op, 760, 0.0); }   // red, twice
            else { key(op, 180, 0.95); key(op, 760, 0.0); }                                             // moonlight, once
            Storyboard::SetTarget(op, glow);
            Storyboard::SetTargetProperty(op, L"Opacity");
            sb.Children().Append(op);
            for (const wchar_t* prop : {L"ScaleX", L"ScaleY"})
            {
                DoubleAnimation sc;
                sc.From(1.0);
                sc.To(failed ? 1.3 : 1.4);   // stays inside the room the templates leave around the moon (Padding): the list clips what spills over
                sc.Duration(DurationHelper::FromTimeSpan(std::chrono::milliseconds(760)));
                CubicEase ease;
                ease.EasingMode(EasingMode::EaseOut);
                sc.EasingFunction(ease);
                Storyboard::SetTarget(sc, ct);
                Storyboard::SetTargetProperty(sc, prop);
                sb.Children().Append(sc);
            }
            sb.Completed([glow, ct, sb](auto&&, auto&&) { sb.Stop(); glow.Opacity(0); ct.ScaleX(1); ct.ScaleY(1); });
            sb.Begin();
        });
    }

    void MainWindow::ShowView(View v)
    {
        const View old = view_;
        view_ = v;
        HomePanel().Visibility(v == View::Home && !loading_ ? Visibility::Visible : Visibility::Collapsed);
        ArchivePanel().Visibility(v == View::Archive ? Visibility::Visible : Visibility::Collapsed);
        SettingsPanel().Visibility(v == View::Settings ? Visibility::Visible : Visibility::Collapsed);
        CompressPanel().Visibility(v == View::Compress ? Visibility::Visible : Visibility::Collapsed);
        ApplyLoadingVisibility();
        UpdateBackdrop();
        if (v != old && !(v == View::Home && loading_))  // page switch: the new page fades in and slides up a little
            FadeIn(v == View::Home ? HomePanel().as<UIElement>() : v == View::Archive ? ArchivePanel().as<UIElement>() : v == View::Compress ? CompressPanel().as<UIElement>() : SettingsPanel().as<UIElement>(), 12, 220);
        UpdateTwinkle();
    }

    // The only animation that keeps running: five of the big stars of the home page slowly dim and come back, each at its own pace (opacity and scale, on the
    // compositor). It stops when the home page is left, the window loses the focus or is minimized, or animations are off.
    void MainWindow::UpdateTwinkle()
    {
        const bool want = AnimOn() && !compact_ && view_ == View::Home && windowActive_ && Stars().Visibility() == Visibility::Visible && HomeScene().Visibility() == Visibility::Visible;
        if (!want)
        {
            for (auto& sb : twinkle_) sb.Stop();   // Stop puts the stars back to their own opacity
            twinkle_.clear();
            return;
        }
        if (!twinkle_.empty()) return;
        using namespace Microsoft::UI::Xaml::Media;
        using namespace Microsoft::UI::Xaml::Media::Animation;
        // the big four-pointed stars of Canis Major (they follow the small ones in Stars()); Sirius, the biggest, also swells a little
        const uint32_t first = static_cast<uint32_t>(starFrac_.size());
        static constexpr struct { uint32_t index; int ms; int delay; bool swell; } kTwinkle[] = {
            {2, 1700, 0, true},       // Sirius
            {0, 2300, 900, false},    // head
            {7, 2000, 400, false},    // hip
            {10, 2600, 1500, false},  // hind leg
            {5, 1900, 2100, false},   // paw
        };
        auto anim = [](DependencyObject const& target, const wchar_t* prop, double to, int ms, int delay) {
            DoubleAnimation a;
            a.To(to);
            a.Duration(DurationHelper::FromTimeSpan(std::chrono::milliseconds(ms)));
            a.AutoReverse(true);
            a.RepeatBehavior(RepeatBehaviorHelper::Forever());
            a.BeginTime(Windows::Foundation::TimeSpan{std::chrono::milliseconds(delay)});
            SineEase ease;
            ease.EasingMode(EasingMode::EaseInOut);
            a.EasingFunction(ease);
            Storyboard::SetTarget(a, target);
            Storyboard::SetTargetProperty(a, prop);
            return a;
        };
        for (auto const& t : kTwinkle)
        {
            if (t.index >= conStars_.size() || first + t.index >= Stars().Children().Size()) continue;
            auto star = Stars().Children().GetAt(first + t.index).as<UIElement>();
            Storyboard sb;
            sb.Children().Append(anim(star, L"Opacity", 0.4, t.ms, t.delay));
            if (t.swell)
            {
                auto ct = star.RenderTransform().try_as<CompositeTransform>();
                if (!ct) { ct = CompositeTransform{}; star.RenderTransform(ct); }
                star.RenderTransformOrigin({0.5, 0.5});
                sb.Children().Append(anim(ct, L"ScaleX", 1.22, t.ms, t.delay));
                sb.Children().Append(anim(ct, L"ScaleY", 1.22, t.ms, t.delay));
            }
            sb.Begin();
            twinkle_.push_back(sb);
        }
    }

    // Window title (title bar text and taskbar): LycanZip or LycanZip - file name
    void MainWindow::SetWindowTitle(std::wstring const& archiveName)
    {
        titleName_ = archiveName;
        std::wstring t = archiveName.empty() ? L"LycanZip" : L"LycanZip - " + archiveName;
        if (titlePct_ >= 0) t = std::to_wstring(titlePct_) + L"% - " + t;
        AppTitleText().Text(hstring{t});
        Title(hstring{t});
    }

    void MainWindow::SetTitleProgress(int percent)
    {
        percent = std::clamp(percent, -1, 100);
        if (percent == titlePct_) return;
        titlePct_ = percent;
        SetWindowTitle(titleName_);
    }

    void MainWindow::ApplyLoadingVisibility()
    {
        LoadingPanel().Visibility(loading_ && loadingShown_ && view_ == View::Home ? Visibility::Visible : Visibility::Collapsed);
    }

    void MainWindow::ShowLoading(std::wstring const& name)
    {
        RevealWindow();   // the loading view is the first thing a window that opens an archive shows
        LoadingText().Text(hstring{lycan::Tf(L"main.opening", name)});
        loadingShown_ = false;  // the spinner only appears when it takes a moment: a quick open must not flash it
        const bool first = !loading_;
        loading_ = true;
        if (!loadingTimer_)
        {
            loadingTimer_ = dispatcher_.CreateTimer();
            loadingTimer_.IsRepeating(false);
            loadingTimer_.Interval(std::chrono::milliseconds(250));
            loadingTimer_.Tick([weak = get_weak()](auto&&, auto&&) {
                auto self = weak.get();
                if (!self || !self->loading_) return;
                self->loadingTimer_.Stop();
                self->loadingShown_ = true;
                self->ApplyLoadingVisibility();
                if (self->LoadingPanel().Visibility() == Visibility::Visible) self->FadeIn(self->LoadingPanel(), 8, 180);
            });
        }
        loadingTimer_.Stop();
        loadingTimer_.Start();
        if (first) ShowView(view_);  // hides the home page
    }

    void MainWindow::EndLoading(bool goHome)
    {
        if (!loading_) return;
        loading_ = false;
        loadingShown_ = false;
        if (loadingTimer_) loadingTimer_.Stop();
        ApplyLoadingVisibility();
        if (goHome) ShowView(view_);  // the home page comes back (a failed open); a successful one switches the view itself
    }

    void MainWindow::ShowHome()
    {
        lycan_app::CloseImagePreview(this);  // the previews belong to the archive that is going away
        lycan_app::CloseAudioPreview(this);
        if (previewPane_) { previewPane_->CloseLarge(); previewPane_->Reset({}, {}); paneKey_.clear(); }
        SetWindowTitle(L"");
        archiveInfo_.clear();
        UnlockArchiveFile();
        current_.reset();
        NotifyStateChanged(true);
        tree_.reset();
        entries_ = nullptr;
        EntryList().ItemsSource(nullptr);
        EntryGrid().ItemsSource(nullptr);
        FolderTree().RootNodes().Clear();
        treeNodes_.clear();
        password_.clear();
        passwordVerified_ = false;
        ShowView(View::Home);
    }

    // ---------------------------------------------------------------- Dialogs

    // ---------------------------------------------------------------- Shared parts of dialogs
    Windows::UI::Color AccentColorNow()
    {
        try
        {
            if (auto b = Application::Current().Resources().Lookup(box_value(L"AccentTextFillColorPrimaryBrush")).try_as<Microsoft::UI::Xaml::Media::SolidColorBrush>())
                return b.Color();
        }
        catch (...) {}
        return Windows::UI::Color{255, 0x6E, 0xA8, 0xFF};
    }


    // The icon of a menu item (Segoe Fluent Icons, in the colour of the text unless the tone says otherwise). dark: the theme of the window
    FontIcon MenuIcon(const wchar_t* glyph, IconTone tone, bool dark)
    {
        FontIcon g;
        g.Glyph(glyph);
        Windows::UI::Color c{};
        switch (tone)
        {
        case IconTone::Danger:   c = dark ? Windows::UI::Color{255, 0xFF, 0x99, 0xA4} : Windows::UI::Color{255, 0xC4, 0x2B, 0x1C}; break;
        case IconTone::Security: c = dark ? Windows::UI::Color{255, 0xFC, 0xE1, 0x00} : Windows::UI::Color{255, 0x9D, 0x5D, 0x00}; break;
        case IconTone::Verify:   c = dark ? Windows::UI::Color{255, 0x6C, 0xCB, 0x5F} : Windows::UI::Color{255, 0x0F, 0x7B, 0x0F}; break;
        case IconTone::Accent:   c = AccentColorNow(); break;
        default: return g;
        }
        g.Foreground(Microsoft::UI::Xaml::Media::SolidColorBrush{lycan_app::StatusColor(c)});
        return g;
    }

    FontIcon MakeGlyph(const wchar_t* glyph, double size, Microsoft::UI::Xaml::Media::Brush const& brush)
    {
        FontIcon icon;
        icon.Glyph(glyph);
        icon.FontSize(size);
        icon.Foreground(brush ? brush : Microsoft::UI::Xaml::Media::SolidColorBrush{AccentColorNow()});
        icon.VerticalAlignment(VerticalAlignment::Center);
        return icon;
    }

    static StackPanel SectionHeader(const wchar_t* glyph, const wchar_t* text)
    {
        StackPanel p;
        p.Orientation(Orientation::Horizontal);
        p.Spacing(8);
        p.Children().Append(MakeGlyph(glyph, 14));
        TextBlock t;
        t.Text(text);
        t.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
        p.Children().Append(t);
        return p;
    }

    // A faint card background (a translucent grey that does not depend on the theme, works for dark and light)
    Border MakeCardBox(UIElement const& child, double padH, double padV)
    {
        Border b;
        b.CornerRadius(CornerRadiusHelper::FromUniformRadius(8));
        b.BorderThickness(ThicknessHelper::FromUniformLength(1));
        b.Padding(ThicknessHelper::FromLengths(padH, padV, padH, padV));
        b.BorderBrush(Microsoft::UI::Xaml::Media::SolidColorBrush{Windows::UI::Color{0x40, 0x80, 0x80, 0x80}});
        b.Background(Microsoft::UI::Xaml::Media::SolidColorBrush{Windows::UI::Color{0x14, 0x80, 0x80, 0x80}});
        b.Child(child);
        return b;
    }

    TextBlock Caption(std::wstring const& text, double opacity)
    {
        TextBlock t;
        t.Text(hstring{text});
        t.Opacity(lycan_app::HighContrast() ? (std::max)(opacity, 0.95) : opacity);   // dimmed text is hard to read on a contrast theme
        t.FontSize(12);
        t.TextWrapping(TextWrapping::Wrap);
        return t;
    }

    // The two lines of text in an option card: title + description
    StackPanel TitledOption(const wchar_t* title, const wchar_t* desc)
    {
        StackPanel p;
        p.Spacing(1);
        TextBlock t;
        t.Text(title);
        p.Children().Append(t);
        p.Children().Append(Caption(desc, 0.65));
        return p;
    }

    // The overview of the sources (number of files and total size) is computed in the background and the text is updated on the UI thread when done; with too many items it counts only up to a limit
    fire_and_forget ScanSources(std::vector<fs::path> inputs, TextBlock target, Microsoft::UI::Dispatching::DispatcherQueue dq)
    {
        co_await resume_background();
        uint64_t files = 0, bytes = 0, dirs = 0;
        bool truncated = false;
        auto t0 = std::chrono::steady_clock::now();
        for (auto const& in : inputs)
        {
            std::error_code ec;
            if (fs::is_directory(in, ec))
            {
                ++dirs;
                for (fs::recursive_directory_iterator it(in, fs::directory_options::skip_permission_denied, ec), end; it != end && !ec; it.increment(ec))
                {
                    std::error_code e2;
                    if (it->is_regular_file(e2)) { ++files; bytes += it->file_size(e2); }
                    else if (it->is_directory(e2)) ++dirs;
                    if (files + dirs > 100000 || std::chrono::steady_clock::now() - t0 > std::chrono::seconds(3)) { truncated = true; break; }
                }
            }
            else
            {
                ++files;
                bytes += fs::file_size(in, ec);
            }
            if (truncated) break;
        }
        std::wstring text = lycan::Tf(L"fmt.n_files", files);
        if (dirs) text += lycan::Tf(L"fmt.and_n_folders", dirs);
        text += lycan::Tf(L"main.total", FormatSize(bytes)) + (truncated ? lycan::Tr(L"main.many_items_only_part_were_counted") : L"");
        dq.TryEnqueue([target, text] { target.Text(hstring{text}); });
    }

    ContentDialog MainWindow::MakeDialog(hstring const& title, const wchar_t* glyph)
    {
        ContentDialog dlg;
        dlg.XamlRoot(RootGrid().XamlRoot());
        dlg.RequestedTheme(RootGrid().ActualTheme());  // a dialog does not inherit RequestedTheme by itself
        if (glyph)
        {
            StackPanel head;
            head.Orientation(Orientation::Horizontal);
            head.Spacing(10);
            head.Children().Append(MakeGlyph(glyph, 22));
            TextBlock t;
            t.Text(title);
            t.FontSize(20);
            t.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
            t.VerticalAlignment(VerticalAlignment::Center);
            head.Children().Append(t);
            dlg.Title(head);
        }
        else dlg.Title(box_value(title));
        return dlg;
    }

    // WinUI allows only one ContentDialog at a time for one XamlRoot: later ones wait in a queue
    Windows::Foundation::IAsyncOperation<ContentDialogResult> MainWindow::ShowDialogAsync(ContentDialog dlg)
    {
        auto strong = get_strong();
        co_await resume_ui{dispatcher_};
        if (sLeaveBackground) sLeaveBackground();  // a prompt needs the user: bring the hidden windows back first
        // Right after the window starts (for example an encrypted archive opened from the command line) XamlRoot may not be ready yet; dialogs must not overlap either
        while ((dialogOpen_ || !RootGrid().XamlRoot()) && !closing_)
        {
            co_await resume_after(50ms);
            co_await resume_ui{dispatcher_};
        }
        if (closing_) co_return ContentDialogResult::None;
        RevealWindow();
        dlg.XamlRoot(RootGrid().XamlRoot());
        dlg.RequestedTheme(RootGrid().ActualTheme());
        if (auto tag = lycan::i18n::UiLanguageTag(); !tag.empty()) dlg.Language(hstring{tag});  // a dialog does not inherit the language either
        dialogOpen_ = true;
        if (compact_)
        {
            // The compact window is only there for the question: the dialog is drawn straight on it (no dimming layer, no frame, no
            // rounded corners, as wide and as high as the window) instead of floating in a window that is bigger than it needs to be.
            auto res = dlg.Resources();
            const Media::SolidColorBrush none{Windows::UI::Color{0, 0, 0, 0}};
            res.Insert(box_value(L"ContentDialogSmokeFill"), none);
            res.Insert(box_value(L"ContentDialogBorderBrush"), none);
            res.Insert(box_value(L"ContentDialogBorderWidth"), box_value(ThicknessHelper::FromUniformLength(0)));
            res.Insert(box_value(L"OverlayCornerRadius"), box_value(CornerRadiusHelper::FromUniformRadius(0)));
            res.Insert(box_value(L"ContentDialogMinWidth"), box_value(static_cast<double>(dialogWidthDip_)));
            res.Insert(box_value(L"ContentDialogMaxWidth"), box_value(static_cast<double>(dialogWidthDip_)));
            // the height follows the content (a short question does not need a tall window); long content scrolls inside the dialog
            double height = 780;
            if (auto el = dlg.Content().try_as<UIElement>())
            {
                try
                {
                    el.Measure(Windows::Foundation::Size{static_cast<float>(dialogWidthDip_ - 48), 4000.0f});
                    height = std::clamp(static_cast<double>(el.DesiredSize().Height) + 210.0, 300.0, 780.0);
                }
                catch (...) {}
            }
            res.Insert(box_value(L"ContentDialogMinHeight"), box_value(height - 38));
            res.Insert(box_value(L"ContentDialogMaxHeight"), box_value(2000.0));
            compactDialogHeight_ = height;
            // content that grows while the dialog is open (the results of the checksums, a list that gets longer) makes the window grow too
            if (auto fe = dlg.Content().try_as<FrameworkElement>())
                fe.SizeChanged([weak = get_weak()](auto&&, SizeChangedEventArgs const& e) {
                    auto s = weak.get();
                    if (!s || !s->compact_ || !s->dialogOpen_) return;
                    const double want = std::clamp(static_cast<double>(e.NewSize().Height) + 210.0, 300.0, 780.0);
                    if (want > s->compactDialogHeight_ + 1) { s->compactDialogHeight_ = want; s->ResizeDip(s->dialogWidthDip_, static_cast<int>(want)); }
                });
            ResizeDip(dialogWidthDip_, static_cast<int>(height));  // the compact window is small, and dialogs (new archive, conflict ...) need more room
        }
        ContentDialogResult r = ContentDialogResult::None;
        try { r = co_await dlg.ShowAsync(); }
        catch (...) {}
        dialogOpen_ = false;
        if (compact_) ShrinkCompactSoon();
        co_return r;
    }

    // After a dialog closes, shrink the compact window back; wait a moment, and if another queued dialog follows at once, do not shrink
    fire_and_forget MainWindow::ShrinkCompactSoon()
    {
        auto strong = get_strong();
        co_await resume_after(250ms);
        co_await resume_ui{dispatcher_};
        if (compact_ && !dialogOpen_ && !closing_) ResizeDip(CompactWidthDip(), 300);
    }

    fire_and_forget MainWindow::ShowMessage(hstring title, hstring body)
    {
        auto strong = get_strong();
        lycan::diag::Warn(L"message shown to the user: {}", std::wstring(title));  // (the text may carry paths: only in the detailed log)
        lycan::diag::Info(L"message text: {}", std::wstring(body));
        OpGuard guard{this};  // until the user has seen the message (including waiting in the queue) the compact window must not close by itself; check again after it closes
        co_await resume_ui{dispatcher_};
        auto dlg = MakeDialog(title, L"\uE946");
        TextBlock tb;
        tb.Text(body);
        tb.TextWrapping(TextWrapping::Wrap);
        dlg.Content(tb);
        dlg.CloseButtonText(lycan::Tr(L"main.ok"));
        co_await ShowDialogAsync(dlg);
    }

    // ---------------------------------------------------------------- Start requests (command line / context menu / handed over by other instances)

    void MainWindow::ReportBadArguments(std::wstring const& error)
    {
        ShowMessage(lycan::Tr(L"main.invalid_command_line_arguments"), hstring{error});
    }

    void MainWindow::DeferShow()
    {
        deferredShow_ = true;
        revealTimer_ = dispatcher_.CreateTimer();
        revealTimer_.IsRepeating(false);
        revealTimer_.Interval(1000ms);
        revealTimer_.Tick([weak = get_weak()](auto&&, auto&&) { if (auto self = weak.get()) self->RevealWindow(); });
        revealTimer_.Start();
    }

    void MainWindow::RevealWindow()
    {
        if (!deferredShow_) return;
        deferredShow_ = false;
        if (revealTimer_) revealTimer_.Stop();
        if (previewOnly_) return;   // the preview window is the whole UI
        lycan::diag::Info(L"window: shown");
        AppWindow().Show();
        Activate();
    }

    void MainWindow::ActivateWindow()
    {
        RevealWindow();
        HWND h = Hwnd();
        if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
        Activate();
        SetForegroundWindow(h);
    }

    // Compact window: an action of the context menu needs only the progress, not the main window
    void MainWindow::EnterCompact()
    {
        compact_ = true;
        compactFlag_ = true;
        ContentRow().Height(GridLength{0, GridUnitType::Pixel});
        HomePanel().Visibility(Visibility::Collapsed);
        ArchivePanel().Visibility(Visibility::Collapsed);
        SettingsPanel().Visibility(Visibility::Collapsed);
        CompressPanel().Visibility(Visibility::Collapsed);
        SettingsButton().Visibility(Visibility::Collapsed);
        UpdateTitleDragRects();
        // Compact window: only the task card with the big moon (no title bar and frame), the size follows the screen scale
        TaskHeader().Visibility(Visibility::Collapsed);
        TaskPanel().Background(nullptr);
        TaskPanel().BorderThickness(ThicknessHelper::FromUniformLength(0));
        TaskPanel().Margin(ThicknessHelper::FromLengths(28, 0, 28, 20));
        TaskScroll().MaxHeight(900);
        TaskList().ItemTemplate(RootGrid().Resources().Lookup(box_value(L"TaskHeroTemplate")).as<DataTemplate>());
        ResizeDip(CompactWidthDip(), 300);
        SetCompactResizable(false);
        UpdateBackdrop();
        UpdateCompactOptions();
    }

    // The compact window has a fixed size (it grows by itself for a dialog and shrinks back): no dragging the edges, no maximizing; it can still be minimized
    void MainWindow::SetCompactResizable(bool resizable)
    {
        try
        {
            if (auto p = AppWindow().Presenter().try_as<Microsoft::UI::Windowing::OverlappedPresenter>())
            {
                p.IsResizable(resizable);
                p.IsMaximizable(resizable);
            }
        }
        catch (...) {}
    }

    void MainWindow::ExitCompact()
    {
        if (!compact_) return;
        compactHost_ = false;
        compact_ = false;
        compactFlag_ = false;
        SetCompactResizable(true);
        CompactDoneButton().Visibility(Visibility::Collapsed);
        if (closeTimer_) closeTimer_.Stop();
        ContentRow().Height(GridLength{1, GridUnitType::Star});
        SettingsButton().Visibility(Visibility::Visible);
        UpdateTitleDragRects();
        TaskHeader().Visibility(Visibility::Visible);
        TaskPanel().Background(Microsoft::UI::Xaml::Media::SolidColorBrush{Windows::UI::Color{0, 0, 0, 0}});  // restore: back to the card background of the theme
        TaskPanel().ClearValue(Border::BackgroundProperty());
        TaskPanel().ClearValue(Border::BorderThicknessProperty());
        CompactOptions().Visibility(Visibility::Collapsed);
        TaskPanel().Margin(ThicknessHelper::FromLengths(20, 0, 20, 16));
        UpdateTaskPanelMax();
        TaskList().ItemTemplate(RootGrid().Resources().Lookup(box_value(L"TaskRowTemplate")).as<DataTemplate>());
        ResizeToDefault();
        ShowView(current_ ? View::Archive : View::Home);
    }

    // The compact window closes by itself when "no flow is running and all tasks succeeded"; with a failure it stays for the user to see
    void MainWindow::CheckCompactAutoClose()
    {
        UpdateCompactDone();
        if (!compact_ || closing_) return;
        if (busyOps_ > 0 || dialogOpen_ || quickRunning_ || !pendingPaths_.empty()) return;

        bool any = false;
        for (auto const& t : queue_.Snapshot())
        {
            if (!t.Finished()) return;
            any = true;
            bool ok = t.state == lycan::TaskState::Completed &&
                      (t.kind != lycan::TaskKind::Extract || t.extract.ok()) &&
                      (t.kind != lycan::TaskKind::Compress || t.compress.ok()) && (t.kind != lycan::TaskKind::Update || t.update.ok());
            // a cancelled integrity test goes back to the finished screen: it follows the "close the window" option
            if (t.kind == lycan::TaskKind::Test && t.state == lycan::TaskState::Cancelled) ok = true;
            if (!ok) return;
        }

        if (!closeTimer_)
        {
            closeTimer_ = dispatcher_.CreateTimer();
            closeTimer_.IsRepeating(false);
            closeTimer_.Tick([weak = get_weak()](auto&&, auto&&) {
                if (auto self = weak.get())
                {
                    self->closeTimer_.Stop();
                    // if a new task / dialog comes while the timer waits, do not close
                    if (self->compact_ && !self->closing_ && self->busyOps_ == 0 && !self->dialogOpen_ && !self->quickRunning_ &&
                        self->pendingPaths_.empty())
                    {
                        bool allDone = true;
                        for (auto const& t : self->queue_.Snapshot()) if (!t.Finished()) allDone = false;
                        if (!allDone) {}
                        else if (self->notifier_.BalloonActive()) { self->closeTimer_.Interval(1000ms); self->closeTimer_.Start(); }  // a notification is still shown; it disappears when the process ends
                        else self->FadeOutAndClose();
                    }
                }
            });
        }
        if (any && !settings_.closeCompactWhenDone) return;  // the user wants to read the result: stay open
        closeTimer_.Interval(any ? 1200ms : 50ms);  // after success wait a moment so that "Done" can be seen; if nothing was done (a dialog was cancelled) close at once
        closeTimer_.Stop();
        closeTimer_.Start();
    }

    // After the compact window is done it closes at once: no fade (nothing that delays or decorates closing a window)
    void MainWindow::FadeOutAndClose()
    {
        if (closing_) return;
        Close();
    }

    void MainWindow::HandleRequest(lycan::LaunchRequest const& req, bool fromLaunch)
    {
        using lycan::LaunchAction;
        lycan::diag::Info(L"request: action {}, {} path(s){}", static_cast<int>(req.action), req.paths.size(), fromLaunch ? L", from the launch" : L"");
        if (req.action == LaunchAction::None)
        {
            if (!fromLaunch) ActivateWindow();
            else if (!MaybeShowWelcome()) { CheckInstalledCopiesAsync(); CheckForUpdateAsync(false); }  // the program was started on its own: the welcome window the first time, and a good moment to say that the menus still point to the old folder (or that LycanZip is installed twice)
            return;
        }
        if (req.IsPreviewOnly())
        {
            if (previewOnly_) PreviewOnlyAsync(req.paths[0], req.action == LaunchAction::PreviewAudio);
            else { ActivateWindow(); OpenArchiveAsync(req.paths[0], 0, L""); }  // a normal window got the request: just open the archive
            return;
        }
        if (previewOnly_) { previewOnly_ = false; AppWindow().Show(); }  // another kind of request reached the hidden host: it becomes a normal window
        if (fromLaunch && (req.action == LaunchAction::Add || req.action == LaunchAction::AddEncrypted)) cFromCompact_ = true;  // creating an archive needs the full page; shrink back to the compact window after the compression starts
        else if (fromLaunch && req.IsQuickAction())
        {
            compactExtract_ = req.IsArchiveQuickAction();
            compactTest_ = req.action == LaunchAction::Test;
            EnterCompact();
        }
        else if (!fromLaunch) ActivateWindow();

        switch (req.action)
        {
        case LaunchAction::Open:
            ExitCompact();
            if (IsArchivePath(req.paths[0])) { OpenArchiveChecked(req.paths[0]); break; }
            if (req.paths.size() == 1 && IsExePath(req.paths[0])) { HandleDroppedPaths(req.paths); break; }
            [[fallthrough]];  // not an archive: becomes "create an archive"
        case LaunchAction::Add:
        case LaunchAction::AddEncrypted:
        case LaunchAction::Compress:
        {
            // On a multi-select Explorer starts a process for each file: collect the paths of the same batch of requests for a short time, then handle them at once
            LaunchAction kind = req.action == LaunchAction::Open ? LaunchAction::Add : req.action;
            if (pendingKind_ != LaunchAction::None && (pendingKind_ != kind || pendingFormat_ != req.format || pendingPreset_ != req.preset || pendingDest_ != req.dest)) FlushPending();
            pendingKind_ = kind;
            pendingFormat_ = req.format;
            pendingPreset_ = req.preset;
            pendingDest_ = req.dest;
            for (auto const& p : req.paths)
                if (std::find(pendingPaths_.begin(), pendingPaths_.end(), p) == pendingPaths_.end()) pendingPaths_.push_back(p);
            if (!aggTimer_)
            {
                aggTimer_ = dispatcher_.CreateTimer();
                aggTimer_.IsRepeating(false);
                aggTimer_.Tick([weak = get_weak()](auto&&, auto&&) { if (auto self = weak.get()) self->FlushPending(); });
            }
            aggTimer_.Interval(500ms);
            aggTimer_.Stop();
            aggTimer_.Start();
            break;
        }
        case LaunchAction::CompressEach:
            for (auto const& p : req.paths) AutoCompress({p}, req.format, req.preset, false, req.dest);   // one archive each: a name that is taken gets (2)
            CheckCompactAutoClose();
            break;
        case LaunchAction::ExtractHere:
        case LaunchAction::ExtractFolder:
        case LaunchAction::Test:
            for (auto const& p : req.paths)
            {
                QuickJob job;
                job.archive = p;
                job.dest = req.dest;   // the right-drag menu: into the folder the files were dropped on (empty = next to the archive)
                job.folderMode = req.action == LaunchAction::ExtractFolder;
                job.test = req.action == LaunchAction::Test;
                quickQueue_.push_back(std::move(job));
            }
            RunQuickQueue();
            break;
        case LaunchAction::ExtractTo:
        case LaunchAction::ExtractMerge:
            ExtractToRequestAsync(req);
            break;
        case LaunchAction::HashSave:
            if (fromLaunch)
            {
                // no window yet: it is shown only when the calculation takes long, or when it has something to ask
                compactHost_ = true;
                compactExtract_ = false;
                compactTest_ = false;
                EnterCompact();
                if (revealTimer_) revealTimer_.Stop();
            }
            HashSaveAsync(req.paths, fromLaunch);
            break;
        case LaunchAction::Hash:
        case LaunchAction::Verify:
            if (fromLaunch)
            {
                // the window only hosts the dialog: the small window (the dialog makes it as big as it needs), not the home page behind it
                compactHost_ = true;
                compactExtract_ = false;
                compactTest_ = false;
                EnterCompact();
            }
            if (req.action == LaunchAction::Hash) HashRequestAsync(req.paths, fromLaunch);
            else QueueVerify(req.paths, fromLaunch);
            break;
        case LaunchAction::Convert:
            cConvertPw_.clear();
            cConvertCp_.clear();
            OpenConvertPage(req.paths);
            break;
        case LaunchAction::OpenContent:
            ExitCompact();
            OpenArchiveAsync(req.paths[0], 0, L"");  // whatever the extension is: treat the file as an archive
            break;
        default: break;
        }
        if ((pendingKind_ == LaunchAction::None || compact_) && req.action != LaunchAction::HashSave) RevealWindow();   // the screen that fits the request is set (a request that ends on the compress page shows the window when the page is up)
    }

    // Ask for the destination once (unless the menu gave one), then extract every archive there (merge = contents straight into it)
    fire_and_forget MainWindow::ExtractToRequestAsync(lycan::LaunchRequest req)
    {
        auto strong = get_strong();
        OpGuard guard{this};  // the compact window must not close while the question is open
        fs::path dest = req.dest;
        if (dest.empty())
            dest = fs::path(std::wstring(co_await lycan_app::PickFolderAsync(AppWindow().Id(), lycan_app::kPurposeExtractDest, req.paths[0].parent_path(),
                                                                             lycan::Tr(L"main.choose_folder_extract"))));
        if (dest.empty()) co_return;  // ~OpGuard checks whether the compact window can close
        RememberRecentExtract(dest);
        for (auto const& p : req.paths)
        {
            QuickJob job;
            job.archive = p;
            job.dest = dest;
            job.noFolder = req.action == lycan::LaunchAction::ExtractMerge;
            quickQueue_.push_back(std::move(job));
        }
        RunQuickQueue();
    }

    // The folders chosen with "Extract to...": the context menu offers them under "Extract to a recent folder"
    void MainWindow::RememberRecentExtract(fs::path const& dir)
    {
        lycan::RememberExtractDir(settings_.recentExtract, dir.wstring());
        SaveSettings();
    }

    void MainWindow::FlushPending()
    {
        if (aggTimer_) aggTimer_.Stop();
        if (pendingPaths_.empty()) { CheckCompactAutoClose(); return; }
        auto paths = std::move(pendingPaths_);
        pendingPaths_.clear();
        auto kind = pendingKind_;
        const auto format = pendingFormat_;
        const auto preset = pendingPreset_;
        const auto dest = pendingDest_;
        pendingKind_ = lycan::LaunchAction::None;
        std::sort(paths.begin(), paths.end());  // the processes arrive in no fixed order: sorting keeps the resulting names stable
        switch (kind)
        {
        case lycan::LaunchAction::Compress: AutoCompress(std::move(paths), format, preset, true, dest); break;
        case lycan::LaunchAction::AddEncrypted:
            StartCompress(std::move(paths));
            // the password is the point of this entry; focus after the page has been laid out
            {
                auto t = dispatcher_.CreateTimer();
                t.IsRepeating(false);
                t.Interval(400ms);
                t.Tick([weak = get_weak(), t](auto&&, auto&&) { if (auto self = weak.get()) { t.Stop(); self->CPwText().Focus(FocusState::Programmatic); } });
                t.Start();
            }
            break;
        default: StartCompress(std::move(paths)); break;
        }
        CheckCompactAutoClose();
        RevealWindow();
    }

    fs::path MainWindow::DefaultArchiveOutput(std::vector<fs::path> const& inputs, const wchar_t* ext, bool unique, fs::path const& outDir)
    {
        std::wstring baseName = inputs.size() == 1 ? inputs[0].stem().wstring() : inputs[0].parent_path().filename().wstring();
        if (inputs.size() == 1 && fs::is_directory(inputs[0])) baseName = inputs[0].filename().wstring();
        if (baseName.empty()) baseName = L"Archive";
        const fs::path dir = outDir.empty() ? inputs[0].parent_path() : outDir;
        return unique ? UniquePath(dir / (baseName + ext)) : dir / (baseName + ext);
    }

    fire_and_forget MainWindow::AutoCompress(std::vector<fs::path> paths, lycan::ArchiveType type, std::wstring preset, bool askExisting, fs::path outDir)
    {
        auto strong = get_strong();
        OpGuard guard{this};   // the compact window stays while the question is open
        lycan::CompressOptions co;
        co.tempDir = lycan_app::ScratchOverride();
        co.skipThumbsDb = settings_.skipThumbsDb;
        co.type = type;
        co.profile = settings_.defaultProfile;
        bool sumsA = settings_.sumsArchive, sumsF = settings_.sumsFiles;   // a template has its own; without one the settings say
        if (!preset.empty())
        {
            // a preset from the built-in list or presets.ini; it decides the format too
            std::vector<lycan::CompressPreset> all = lycan::BuiltinPresets();
            for (auto& u : lycan::LoadPresets(lycan::DefaultPresetsPath())) all.push_back(std::move(u));
            for (auto const& p : all) if (p.name == preset) { lycan::ApplyPreset(p, co); sumsA = p.sumsArchive; sumsF = p.sumsFiles; break; }
        }
        // Note: compute the output name first, then move. The order in which arguments are evaluated in one call is not fixed; paths could be moved away before DefaultArchiveOutput reads it
        const std::wstring ext = std::wstring(L".") + lycan::FormatName(co.type);
        fs::path output = DefaultArchiveOutput(paths, ext.c_str(), !askExisting, outDir);
        std::vector<fs::path> sources = paths;
        if (askExisting)
        {
            std::error_code ec;
            if (fs::exists(output, ec))
            {
                // the name is taken: make a new archive under another name, replace the old one, or add to it (the update modes)
                const bool canUpdate = co.type == lycan::ArchiveType::Zip || co.type == lycan::ArchiveType::SevenZip || co.type == lycan::ArchiveType::Tar;
                const int32_t choice = co_await ChooseExistingOutputAsync(output, canUpdate);
                if (choice == 0) co_return;
                if (choice == 1) output = UniquePath(output);
                else if (choice == 2) co.overwrite = true;
                else
                {
                    lycan::UpdateOptions uo;
                    uo.compress = co;
                    uo.mode = static_cast<lycan::UpdateMode>(choice - 3);
                    uo.keepBackup = settings_.keepBackupOnModify;
                    std::vector<lycan::UpdateOp> ops = {lycan::UpdateOp::Add(L"", paths)};
                    if (uo.mode == lycan::UpdateMode::Synchronize && !co_await ConfirmSyncAsync(output, ops, uo)) co_return;
                    std::lock_guard lk(metaM_);
                    queue_.EnqueueUpdate(output, std::move(ops), uo);
                    co_return;
                }
            }
        }
        {
            std::lock_guard lk(metaM_);
            uint64_t id = queue_.EnqueueCompress(std::move(output), std::move(paths), co);
            CompressMeta m;
            m.sources = std::move(sources);
            m.testAfter = settings_.testAfterCompress;
            m.sumsArchive = sumsA;
            m.sumsFiles = sumsF;
            compressMeta_[id] = std::move(m);
        }
    }

    fire_and_forget MainWindow::RunQuickQueue()
    {
        auto strong = get_strong();
        if (quickRunning_) co_return;
        quickRunning_ = true;
        while (!quickQueue_.empty() && !closing_)
        {
            QuickJob job = std::move(quickQueue_.front());
            quickQueue_.pop_front();
            try { co_await QuickExtractOne(std::move(job)); }
            catch (...) {}
        }
        quickRunning_ = false;
        CheckCompactAutoClose();
    }

    // Right click "Extract here / to a folder of the same name": no file list view; open -> ask for the password if needed -> queue the extraction task directly
    Windows::Foundation::IAsyncAction MainWindow::QuickExtractOne(QuickJob job)
    {
        auto strong = get_strong();
        const fs::path path = job.archive;
        co_await resume_ui{dispatcher_};
        hstring name{path.filename().wstring()};

        std::wstring password;
        bool wrong = false, triedKnown = false;
        std::shared_ptr<lycan::ArchiveSource> src;
        auto opens = [path](std::wstring const& pw) {
            lycan::OpenOptions oo;
            oo.tempDir = lycan_app::ScratchOverride();
            oo.password = [pw](std::wstring& out) { out = pw; return true; };
            try { lycan::ArchiveSource::Open(path, oo); return true; } catch (...) { return false; }
        };
        for (;;)
        {
            std::wstring errorText;
            bool needPassword = false;
            co_await resume_background();
            try
            {
                lycan::OpenOptions oo;
        oo.tempDir = lycan_app::ScratchOverride();
                if (!password.empty()) oo.password = [password](std::wstring& out) { out = password; return true; };
                src = lycan::ArchiveSource::Open(path, oo);
            }
            catch (lycan::Error const& e)
            {
                if (e.wrongPassword()) { needPassword = true; wrong = true; }
                else if (e.cancelled()) needPassword = true;
                else errorText = e.message().Text();
            }
            catch (std::exception const& e) { errorText = FromUtf8(e.what()); }
            co_await resume_ui{dispatcher_};

            if (src) break;
            if (needPassword && !triedKnown)   // encrypted names: a password of this run or a saved one first
            {
                triedKnown = true;
                auto known = co_await TryKnownPasswordsAsync(opens);
                if (!known.empty()) { password = std::wstring(known); wrong = false; continue; }
            }
            if (needPassword)
            {
                auto pw = co_await AskPasswordAsync(name, wrong, opens);
                if (pw.empty()) co_return;
                password = std::wstring(pw);
                continue;
            }
            ShowMessage(lycan::Tr(L"main.cannot_open_archive"), hstring{errorText.empty() ? lycan::Tf(L"main.file_format_unrecognized_file", path.wstring()) : errorText});
            co_return;
        }

        // Encrypted content (zip AES, 7z data encryption): verify the password first and ask again if wrong; with encrypted headers it was verified while opening
        if (src->Caps().encrypted && password.empty())
        {
            auto checks = [src](std::wstring const& pw) { return src->CheckPassword(pw); };
            password = std::wstring(co_await TryKnownPasswordsAsync(checks));   // this run's / saved ones first
            bool wrongPw = false;
            while (password.empty())
            {
                auto pw = co_await AskPasswordAsync(name, wrongPw, checks);
                if (pw.empty()) co_return;
                co_await resume_background();
                bool ok = src->CheckPassword(std::wstring(pw));
                co_await resume_ui{dispatcher_};
                if (ok) { password = std::wstring(pw); break; }
                wrongPw = true;
            }
        }
        if (!password.empty()) PasswordWorked(password);

        if (job.test)
        {
            lycan::OpenOptions to;
            to.tempDir = lycan_app::ScratchOverride();
            if (!password.empty()) to.password = [password](std::wstring& out) { out = password; return true; };
            queue_.EnqueueTest(path, to);
            co_return;
        }

        lycan::ExtractOptions eo;
        eo.skipMacMetadata = settings_.skipMacMetadata;
        eo.keepDamaged = settings_.keepDamaged;
        const lycan::ExtractMode mode = job.noFolder ? lycan::ExtractMode::NoFolder : job.folderMode ? lycan::ExtractMode::Folder : settings_.extractMode;
        eo.smart = mode == lycan::ExtractMode::Auto;
        eo.overwrite = lycan::ToOverwriteMode(settings_.conflict);
        if (eo.overwrite == lycan::OverwriteMode::Ask) { auto rule = std::make_shared<std::atomic<int>>(0); eo.onConflict = [this, rule](lycan::ConflictInfo const& ci) { return AskConflict(ci, rule); }; }
        ApplyLargeGuard(eo);
        lycan::OpenOptions oo;
        oo.tempDir = lycan_app::ScratchOverride();
        if (!password.empty())
        {
            eo.password = [password](std::wstring& out) { out = password; return true; };
            oo.password = eo.password;
        }
        const fs::path base = job.dest.empty() ? path.parent_path() : job.dest;
        fs::path dest = mode == lycan::ExtractMode::Folder ? base / lycan::ArchiveBaseName(path) : base;

        TaskMeta meta;
        meta.archive = path;
        meta.wholeArchive = true;
        std::lock_guard lk(metaM_);
        uint64_t id = queue_.EnqueueExtract(path, dest, eo, oo, {});
        taskMeta_[id] = std::move(meta);
    }

    // ---------------------------------------------------------------- Drag and drop

    void MainWindow::OnDragOver(Windows::Foundation::IInspectable const&, DragEventArgs const& e)
    {
        if (dragOutActive_) return;  // files dragged out of an archive must not be received by our own window again
        if (e.DataView().Contains(Windows::ApplicationModel::DataTransfer::StandardDataFormats::StorageItems()))
        {
            e.AcceptedOperation(Windows::ApplicationModel::DataTransfer::DataPackageOperation::Copy);
            e.DragUIOverride().Caption(lycan::Tr(L"main.release_process"));
            SetDragGlow(true);
        }
    }

    void MainWindow::OnDragLeave(Windows::Foundation::IInspectable const&, DragEventArgs const&) { SetDragGlow(false); }

    fire_and_forget MainWindow::OnDrop(Windows::Foundation::IInspectable const&, DragEventArgs const& e)
    {
        auto strong = get_strong();
        if (dragOutActive_) co_return;
        auto deferral = e.GetDeferral();
        SetDragGlow(false);
        PlaySfx(L"drop");
        std::vector<fs::path> paths;
        try
        {
            auto items = co_await e.DataView().GetStorageItemsAsync();
            for (auto const& it : items) paths.emplace_back(std::wstring(it.Path()));
        }
        catch (...) {}
        deferral.Complete();
        if (paths.empty()) co_return;
        HandleDroppedPaths(std::move(paths));
    }

    // ---------------------------------------------------------------- Opening an archive

    fire_and_forget MainWindow::OnOpenArchiveClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        auto strong = get_strong();
        auto files = co_await lycan_app::PickFilesAsync(AppWindow().Id(), lycan_app::kPurposeOpenArchive, false, true);
        if (files.Size() > 0) OpenFromUi(fs::path(std::wstring(files.GetAt(0))));
    }

    lycan::OpenOptions MainWindow::MakeOpenOptions()
    {
        lycan::OpenOptions oo;
        oo.tempDir = lycan_app::ScratchOverride();
        oo.nameCodepage = currentCp_;
        if (!password_.empty())
            oo.password = [pw = password_](std::wstring& out) { out = pw; return true; };
        return oo;
    }

    // Encrypted content (zip AES / 7z data encryption ...): ask and "verify" the password first, and ask again if wrong, instead of finding out only when the extraction fails
    Windows::Foundation::IAsyncOperation<bool> MainWindow::EnsurePasswordAsync()
    {
        auto strong = get_strong();
        if (!current_ || !current_->Caps().encrypted || passwordVerified_) co_return true;
        auto src = current_;
        hstring name{currentPath_.filename().wstring()};
        // a password of this run or a saved one first
        auto checks = [src](std::wstring const& pw) { return src->CheckPassword(pw); };
        if (auto known = co_await TryKnownPasswordsAsync(checks); !known.empty() && current_ == src)
        {
            password_ = std::wstring(known);
            passwordVerified_ = true;
            if (tree_ && view_ == View::Archive) RefreshList(true);
            ShowStatusNote(lycan::Tr(L"main.used_saved_password"));
            co_return true;
        }
        bool wrong = false;
        for (;;)
        {
            auto pw = co_await AskPasswordAsync(name, wrong, checks);
            if (pw.empty()) co_return false;  // the user cancelled
            SetBusy(true);
            co_await resume_background();
            bool ok = src->CheckPassword(std::wstring(pw));
            co_await resume_ui{dispatcher_};
            SetBusy(false);
            if (ok)
            {
                PasswordWorked(std::wstring(pw));
                password_ = std::wstring(pw);
                passwordVerified_ = true;
                if (tree_ && view_ == View::Archive) RefreshList(true);   // the lock icons give way to the icons of the file types
                co_return true;
            }
            wrong = true;
        }
    }

    fire_and_forget MainWindow::OpenArchiveAsync(fs::path path, unsigned codepage, std::wstring password, bool previousWasWrong)
    {
        auto strong = get_strong();
        struct PendingGuard
        {
            MainWindow* w;
            explicit PendingGuard(MainWindow* x, fs::path const& p) : w(x) { ++w->openPending_; w->openingPath_ = p; w->NotifyStateChanged(true); }
            ~PendingGuard() { if (--w->openPending_ == 0) w->openingPath_.clear(); w->NotifyStateChanged(true); }
        } pending{this, path};
        // nothing on screen to keep (the home page of a fresh window): the loading view; otherwise the ring over the content
        const bool fresh = !current_ && view_ == View::Home && !compact_;
        if (fresh) ShowLoading(path.filename().wstring());
        else SetBusy(true);
        std::shared_ptr<lycan::ArchiveSource> src;
        std::wstring errorText;
        lycan::Message tempMsg;  // the scratch file of a tar.gz did not fit
        bool needPassword = false, wrong = previousWasWrong;

        co_await resume_background();
        const auto openStart = std::chrono::steady_clock::now();
        if (const wchar_t* d = _wgetenv(L"LYCAN_TEST_OPEN_DELAY")) Sleep(static_cast<DWORD>(_wtoi(d)));  // tests: a slow open, to see the loading view
        try
        {
            lycan::OpenOptions oo;
        oo.tempDir = lycan_app::ScratchOverride();
            oo.nameCodepage = codepage;
            if (!password.empty()) oo.password = [password](std::wstring& out) { out = password; return true; };
            src = lycan::ArchiveSource::Open(path, oo);
        }
        catch (lycan::Error const& e)
        {
            if (e.wrongPassword()) { needPassword = true; wrong = true; }
            else if (e.cancelled()) needPassword = true;  // a password is needed (encrypted headers)
            else if (e.tempSpace()) tempMsg = e.message();
            else errorText = e.message().Text();
        }
        catch (std::exception const& e) { errorText = FromUtf8(e.what()); }

        co_await resume_ui{dispatcher_};
        SetBusy(false);
        {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - openStart).count();
            if (src) lycan::diag::Info(L"opened {}: {}, {} item(s), {} ms", lycan::diag::Name(path), src->FormatName(), src->Entries().size(), ms);
            else if (needPassword) lycan::diag::Info(L"opening {}: a password is needed{}", lycan::diag::Name(path), wrong ? L" (the one given was wrong)" : L"");
            else if (tempMsg.empty()) lycan::diag::Warn(L"could not open {} ({} ms): {}", lycan::diag::Name(path), ms, errorText);
            else lycan::diag::Warn(L"could not open {}: the scratch folder has too little room", lycan::diag::Name(path));
        }

        if (!tempMsg.empty())
        {
            uint64_t need = 0, have = 0;
            std::wstring dir;
            lycan::ParseTempSpaceMessage(tempMsg, need, have, dir);
            if (loading_) { loadingShown_ = false; ApplyLoadingVisibility(); }
            const hstring base = co_await AskTempLocationAsync(need, have, dir);
            if (!base.empty())
            {
                OpenArchiveAsync(path, codepage, password, previousWasWrong);  // the place was chosen: it is used from now on
                co_return;
            }
            restorePos_ = {};
            EndLoading(true);
            co_return;
        }

        if (needPassword)
        {
            auto opens = [path, codepage](std::wstring const& pw) {
                lycan::OpenOptions oo;
                oo.tempDir = lycan_app::ScratchOverride();
                oo.nameCodepage = codepage;
                oo.password = [pw](std::wstring& out) { out = pw; return true; };
                try { lycan::ArchiveSource::Open(path, oo); return true; } catch (...) { return false; }
            };
            if (password.empty() && !wrong)   // encrypted names: a password of this run or a saved one first
            {
                auto known = co_await TryKnownPasswordsAsync(opens);
                if (!known.empty())
                {
                    openNote_ = lycan::Tr(L"main.used_saved_password");
                    OpenArchiveAsync(path, codepage, std::wstring(known), false);
                    co_return;
                }
            }
            if (loading_) { loadingShown_ = false; ApplyLoadingVisibility(); }  // the question comes first, without the spinner behind it
            auto pw = co_await AskPasswordAsync(hstring{path.filename().wstring()}, wrong, opens);
            if (!pw.empty()) OpenArchiveAsync(path, codepage, std::wstring(pw), false);
            else
            {
                restorePos_ = {};
                EndLoading(true);
            }
            co_return;
        }
        if (!src)
        {
            restorePos_ = {};
            EndLoading(true);
            ShowMessage(lycan::Tr(L"main.cannot_open_archive"), hstring{errorText.empty() ? lycan::Tr(L"main.file_format_unrecognized_file_damaged") : errorText});
            co_return;
        }
        password_ = password;
        passwordVerified_ = !password.empty();  // an archive with encrypted headers: if it could be opened the password is right
        if (!password.empty()) PasswordWorked(password);
        ShowArchive(std::move(src), path, codepage);
    }

    void MainWindow::ShowArchive(std::shared_ptr<lycan::ArchiveSource> src, fs::path path, unsigned codepage)
    {
        EndLoading(false);
        if (current_ && currentPath_ != path)
        {
            // this window now shows another archive ("open in this window"): its previews would go on showing the old one
            lycan_app::CloseImagePreview(this);
            lycan_app::CloseAudioPreview(this);
            if (previewPane_) previewPane_->CloseLarge();
        }
        current_ = src;
        currentPath_ = path;
        currentCp_ = codepage;
        LockArchiveFile(path);
        UpdateArchiveIcon();
        UpdateExtractButton();
        thumbSessionKey_.clear();
        thumbNoteShown_ = false;

        uint64_t total = 0;
        for (auto const& e : src->Entries())
            if (!e.isDir) total += e.size;
        {
            auto caps = src->Caps();
            archiveInfo_ = src->FormatName() + lycan::Tf(L"fmt.status_items_size", src->Entries().size(), FormatSize(total));
            if (caps.solid) archiveInfo_ += lycan::Tr(L"main.solid_archive");
            if (caps.encrypted) archiveInfo_ += lycan::Tr(L"main.encrypted");
        }
        SetWindowTitle(path.filename().wstring());

        // View state: starts from the default of the settings (default: one level browsing)
        tree_ = std::make_shared<lycan::ArchiveTree>(src->Entries(), settings_.hideMacMetadata);
        tree_->SetMethodNames(src->MethodNames());
        flat_ = settings_.browseFlat;
        viewMode_ = settings_.viewMode;
        treeWanted_ = settings_.showFolderTree;
        sortKey_ = lycan::SortKey::Name;
        sortAsc_ = true;
        query_.clear();
        if (keptView_.valid && restoreFor_ == path)  // this archive was just changed: keep the view the user had
        {
            flat_ = keptView_.flat;
            viewMode_ = keptView_.mode;
            sortKey_ = keptView_.sort;
            sortAsc_ = keptView_.asc;
            treeWanted_ = keptView_.tree;
            query_ = keptView_.query;
        }
        keptView_ = {};
        syncingUi_ = true;
        SearchBox().Text(hstring{query_});
        FlatToggle().IsChecked(flat_);
        TreeToggle().IsChecked(treeWanted_);
        syncingUi_ = false;
        BuildFolderTree();
        UpdateTreeVisibility();
        NavigateTo(tree_->Root(), true);
        // The archive has only one folder (the outermost one is often just a wrapper): open one level down automatically, at most three times
        for (int depth = 0; depth < 3 && !flat_; ++depth)
        {
            auto kids = tree_->Children(curNode_);
            if (kids.size() != 1 || !tree_->Node(kids[0]).isDir) break;
            NavigateTo(kids[0], true);
        }
        // After switching the file name encoding go back to the folder where we were (same archive, the item with the same order number goes up to the original depth)
        if (restorePos_.entry != lycan::kNoEntry && restorePos_.archive == path)
        {
            uint32_t found = lycan::kNoNode;
            for (uint32_t n = 0; n < tree_->NodeCount(); ++n)
                if (tree_->Node(n).entryIndex == restorePos_.entry) { found = n; break; }
            if (found != lycan::kNoNode)
            {
                auto depthOf = [&](uint32_t n) { auto const& p = tree_->Node(n).path; return static_cast<int>(std::count(p.begin(), p.end(), L'/')) + 1; };
                for (int d = depthOf(found); d > restorePos_.depth && tree_->Node(found).parent != lycan::kNoNode; --d) found = tree_->Node(found).parent;
                if (found != tree_->Root()) NavigateTo(found, true);
            }
        }
        restorePos_ = {};
        // after a change: the folder that was shown, and the items that were added / renamed selected
        if (restoreFor_ == path)
        {
            if (!flat_)
            {
                const uint32_t n = restoreFolder_.empty() ? tree_->Root() : tree_->Find(restoreFolder_);
                if (n != lycan::kNoNode && tree_->Node(n).isDir) NavigateTo(n, true);
            }
            if (!restoreSelect_.empty() && entries_)
            {
                bool first = true;
                for (auto const& it : entries_)
                {
                    auto row = it.as<LycanZip::EntryRow>();
                    if (std::find(restoreSelect_.begin(), restoreSelect_.end(), tree_->Node(row.NodeId()).path) == restoreSelect_.end()) continue;
                    ActiveList().SelectedItems().Append(row);
                    if (first) { ActiveList().ScrollIntoView(row); first = false; }
                }
            }
        }
        restoreFor_.clear();
        restoreFolder_.clear();
        restoreSelect_.clear();

        auto caps = src->Caps();

        // File name encoding
        auto ne = src->NameEncoding();
        {
            // repacking: removing the password of a zip / 7z, upgrading the names of an old zip / tar
            lycan::ArchiveType writable;
            const bool canWrite = lycan::WritableTypeOf(*src, writable);
            RemovePasswordItem().Visibility(caps.encrypted && canWrite && caps.canModify ? Visibility::Visible : Visibility::Collapsed);
            UpgradeItem().Visibility(ne.applicable && caps.canModify ? Visibility::Visible : Visibility::Collapsed);
        }
        EncodingPanel().Visibility(ne.applicable ? Visibility::Visible : Visibility::Collapsed);
        EncodingStrip().Visibility(Visibility::Collapsed);
        if (ne.applicable)
        {
            updatingEncoding_ = true;
            int idx = 0;
            for (uint32_t i = 0; i < EncodingCombo().Items().Size(); ++i)
            {
                int32_t tag = unbox_value<int32_t>(EncodingCombo().Items().GetAt(i).as<ComboBoxItem>().Tag());
                if (codepage ? static_cast<unsigned>(tag) == ne.codepage : tag == 0) { idx = static_cast<int>(i); break; }
            }
            EncodingCombo().SelectedIndex(idx);
            updatingEncoding_ = false;
            if (ne.autoDetected)
            {
                std::wstring message = lycan::Tf(L"main.currently_using_if_file_names_look", lycan::CodepageName(ne.codepage));
                std::vector<std::pair<std::pair<hstring, hstring>, std::function<void()>>> links;
                if (caps.canModify)
                {
                    message += lycan::Tr(L"main.upgrade_avoids_problem");
                    links.push_back({{lycan::Tr(L"main.upgrade_archive"), lycan::Tr(L"main.upgrade_archive_tip")}, [this]() {
                        EncodingStrip().Visibility(Visibility::Collapsed);
                        OnUpgradeClick(nullptr, nullptr);
                    }});
                }
                ShowNotice(EncodingStrip(), ne.ambiguous, ne.ambiguous ? lycan::Tr(L"main.filename_encoding_uncertain") : lycan::Tr(L"main.filename_encoding_detected"),
                           hstring{message}, links);
            }
        }

        ShowSumsBanner();
        listFxUntil_ = GetTickCount64() + 400;
        PlaySfx(L"open");
        ShowView(View::Archive);
        NotifyStateChanged(true);
        if (!openNote_.empty()) { ShowStatusNote(openNote_); openNote_.clear(); }
        MaybeOfferWelcome();
    }

    // The welcome window opens by itself only when the program is started on its own. The Microsoft Store does not start a program after it is
    // installed, so the first start is often a double-click on an archive: then the first full window (not the compact windows of the context
    // menu, not a preview) offers it in a one-line notice above the list. "Not now" counts as seen; closing the notice asks again next time.
    void MainWindow::MaybeOfferWelcome()
    {
        static bool offered = false;   // once per process (several windows)
        if (offered || compact_ || previewOnly_ || settings_.welcomeDone || lycan_app::WelcomeIsOpen()) return;
        if (GetEnvironmentVariableW(L"LYCAN_NO_WELCOME", nullptr, 0) > 0 && GetEnvironmentVariableW(L"LYCAN_FORCE_WELCOME", nullptr, 0) == 0) return;
        offered = true;
        std::vector<std::pair<std::pair<hstring, hstring>, std::function<void()>>> links;
        links.push_back({{lycan::Tr(L"main.welcome_offer_start"), {}}, [this]() { WelcomeStrip().Visibility(Visibility::Collapsed); ShowWelcomeAsync(); }});
        links.push_back({{lycan::Tr(L"main.welcome_offer_no"), {}}, [this]() {
            WelcomeStrip().Visibility(Visibility::Collapsed);
            settings_.welcomeDone = true;
            SaveSettings();
        }});
        ShowNotice(WelcomeStrip(), false, lycan::Tr(L"main.welcome_offer_title"), lycan::Tr(L"main.welcome_offer_hint"), links);
    }

    void MainWindow::OnBackClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) { ShowHome(); }

    void MainWindow::OnEncodingChanged(Windows::Foundation::IInspectable const&, SelectionChangedEventArgs const&)
    {
        if (updatingEncoding_ || !current_) return;
        auto item = EncodingCombo().SelectedItem().try_as<ComboBoxItem>();
        if (!item) return;
        unsigned cp = static_cast<unsigned>(unbox_value<int32_t>(item.Tag()));
        if (cp == currentCp_) return;
        tarNamesOkFor_.clear();   // another encoding: a change of a tar asks again
        restorePos_ = {};
        if (tree_ && curNode_ != tree_->Root())
        {
            for (uint32_t n : tree_->Descendants(curNode_))
                if (tree_->Node(n).entryIndex != lycan::kNoEntry) { restorePos_.entry = tree_->Node(n).entryIndex; break; }
            auto const& p = tree_->Node(curNode_).path;
            restorePos_.depth = static_cast<int>(std::count(p.begin(), p.end(), L'/')) + 1;
            restorePos_.archive = currentPath_;
        }
        OpenArchiveAsync(currentPath_, cp, password_);
    }

    // ---------------------------------------------------------------- Archive view: folder tree / navigation / sorting / search / selection

    static uint32_t NodeIdOf(TreeViewNode const& tn) { return tn.Content().as<LycanZip::EntryRow>().NodeId(); }

    // The picture for a row: the cached icon of its kind of file (when the setting is on and it is loaded already)
    void MainWindow::ApplyShellIcon(LycanZip::EntryRow const& row)
    {
        auto* r = get_self<implementation::EntryRow>(row);
        if (!settings_.shellIcons) { r->SetShellIcons(nullptr, nullptr); return; }
        const std::wstring key{r->IconKey()};
        if (key.empty()) return;
        const double scale = GetDpiForWindow(Hwnd()) / 96.0;
        auto find = [&](int px) -> Microsoft::UI::Xaml::Media::ImageSource {
            auto it = shellIconCache_.find(key + L"|" + std::to_wstring(px));
            return it == shellIconCache_.end() ? nullptr : it->second;
        };
        auto smallIcon = find(lycan_app::ShellIconPx(18, scale));
        auto largeIcon = viewMode_ == lycan::ViewMode::Icons ? find(lycan_app::ShellIconPx(static_cast<int>(IconDipFor(settings_.thumbSize)), scale)) : nullptr;
        if (smallIcon || largeIcon) r->SetShellIcons(smallIcon, largeIcon);
    }

    // Looks at the rows and the tree for kinds of file that have no icon yet and loads them (a background thread), then gives the rows their pictures
    void MainWindow::RefreshShellIcons()
    {
        if (!settings_.shellIcons) return;
        const double scale = GetDpiForWindow(Hwnd()) / 96.0;
        const int pxSmall = lycan_app::ShellIconPx(18, scale), pxLarge = lycan_app::ShellIconPx(static_cast<int>(IconDipFor(settings_.thumbSize)), scale);
        std::set<std::wstring> missing;   // "key|px"
        auto need = [&](std::wstring const& key) {
            if (key.empty()) return;
            if (!shellIconCache_.count(key + L"|" + std::to_wstring(pxSmall))) missing.insert(key + L"|" + std::to_wstring(pxSmall));
            if (viewMode_ == lycan::ViewMode::Icons && !shellIconCache_.count(key + L"|" + std::to_wstring(pxLarge))) missing.insert(key + L"|" + std::to_wstring(pxLarge));
        };
        need(L"<dir>");
        if (entries_) for (auto const& it : entries_) need(std::wstring{it.as<LycanZip::EntryRow>().IconKey()});
        if (!missing.empty()) LoadShellIconsAsync(std::vector<std::wstring>(missing.begin(), missing.end()));
    }

    winrt::fire_and_forget MainWindow::LoadShellIconsAsync(std::vector<std::wstring> keys)
    {
        auto strong = get_strong();
        for (auto& k : keys) shellIconPending_.insert(k);
        if (shellIconBusy_) co_return;   // the one that is running picks the new keys up when it comes back
        shellIconBusy_ = true;
        while (!shellIconPending_.empty())
        {
            std::vector<std::wstring> todo(shellIconPending_.begin(), shellIconPending_.end());
            shellIconPending_.clear();
            winrt::apartment_context ui;
            co_await winrt::resume_background();
            std::vector<std::pair<std::wstring, lycan_app::IconPixels>> loaded;
            for (auto const& k : todo)
            {
                const auto bar = k.rfind(L'|');
                loaded.emplace_back(k, lycan_app::LoadShellIconPixels(k.substr(0, bar), _wtoi(k.c_str() + bar + 1)));
            }
            co_await ui;
            for (auto& [k, px] : loaded)
                if (auto img = lycan_app::MakeIconBitmap(px)) shellIconCache_.insert_or_assign(k, img);
        }
        shellIconBusy_ = false;
        // everything that is on screen gets its picture
        if (entries_) for (auto const& it : entries_) ApplyShellIcon(it.as<LycanZip::EntryRow>());
        for (auto const& [id, tn] : treeNodes_) if (tn.Content()) ApplyShellIcon(tn.Content().as<LycanZip::EntryRow>());
    }

    void MainWindow::BuildFolderTree()
    {
        FolderTree().RootNodes().Clear();
        treeNodes_.clear();
        if (!tree_) return;
        auto root = MakeTreeNode(tree_->Root());
        FolderTree().RootNodes().Append(root);
        RealizeTreeChildren(root);
        root.IsExpanded(true);
    }

    // The nodes of the folder tree create their children only when expanded (a large archive may have tens of thousands of folders)
    TreeViewNode MainWindow::MakeTreeNode(uint32_t id)
    {
        auto const& n = tree_->Node(id);
        TreeViewNode tn;
        hstring name = id == tree_->Root() ? hstring{currentPath_.filename().wstring()} : hstring{n.name};
        auto row = make<implementation::EntryRow>(id, name, hstring{}, hstring{}, hstring{}, hstring{}, true, false, false);
        if (id == tree_->Root()) get_self<implementation::EntryRow>(row)->SetRootIcon(FileIconImage(currentPath_));
        else ApplyShellIcon(row);
        tn.Content(row);
        bool hasDirs = std::any_of(n.children.begin(), n.children.end(), [&](uint32_t c) { return tree_->Node(c).isDir; });
        tn.HasUnrealizedChildren(hasDirs);
        treeNodes_[id] = tn;
        return tn;
    }

    void MainWindow::RealizeTreeChildren(TreeViewNode const& tn)
    {
        if (!tree_ || !tn.HasUnrealizedChildren()) return;
        for (uint32_t c : tree_->Children(NodeIdOf(tn)))
            if (tree_->Node(c).isDir) tn.Children().Append(MakeTreeNode(c));
        tn.HasUnrealizedChildren(false);
    }

    void MainWindow::OnTreeExpanding(TreeView const&, TreeViewExpandingEventArgs const& args) { RealizeTreeChildren(args.Node()); }

    void MainWindow::OnTreeItemInvoked(TreeView const&, TreeViewItemInvokedEventArgs const& args)
    {
        auto item = args.InvokedItem();
        if (auto tn = item.try_as<TreeViewNode>()) NavigateTo(NodeIdOf(tn), false);
        else if (auto row = item.try_as<LycanZip::EntryRow>()) NavigateTo(row.NodeId(), false);
    }

    // Makes the tree selection follow the current folder (when navigating from the breadcrumb / a double click / up one level)
    void MainWindow::ExpandTreeTo(uint32_t node)
    {
        std::vector<uint32_t> chain;
        for (uint32_t x = node;; x = tree_->Node(x).parent)
        {
            chain.push_back(x);
            if (x == tree_->Root()) break;
        }
        std::reverse(chain.begin(), chain.end());
        TreeViewNode last{nullptr};
        for (size_t i = 0; i < chain.size(); ++i)
        {
            auto it = treeNodes_.find(chain[i]);
            if (it == treeNodes_.end()) break;
            last = it->second;
            if (i + 1 < chain.size())
            {
                RealizeTreeChildren(last);
                last.IsExpanded(true);
            }
        }
        if (!last) return;
        // Setting the selection must wait until the control is loaded (right after an archive is opened the tree is not shown yet and the template is not applied; setting it then would crash XAML)
        dispatcher_.TryEnqueue(Microsoft::UI::Dispatching::DispatcherQueuePriority::Low, [weak = get_weak(), last] {
            auto self = weak.get();
            if (!self || !self->tree_ || !self->FolderTree().IsLoaded()) return;
            self->syncingUi_ = true;
            self->FolderTree().SelectedNode(last);
            self->syncingUi_ = false;
        });
    }

    void MainWindow::NavigateTo(uint32_t node, bool syncTree)
    {
        if (!tree_ || node >= tree_->NodeCount() || !tree_->Node(node).isDir) return;
        curNode_ = node;
        if (!query_.empty())
        {
            query_.clear();
            syncingUi_ = true;
            SearchBox().Text(L"");
            syncingUi_ = false;
        }
        if (syncTree) ExpandTreeTo(node);
        listFxUntil_ = GetTickCount64() + 350;
        RefreshList();
    }

    static int EstimateNameLines(std::wstring const& name, double width);

    void MainWindow::RefreshList(bool keepSelection)
    {
        if (!tree_) return;
        std::set<uint32_t> keep;
        if (keepSelection)
        {
            if (!keepOverride_.empty()) keep.insert(keepOverride_.begin(), keepOverride_.end());
            else for (auto n : SelectedNodes()) keep.insert(n);
            keepOverride_.clear();
        }

        const ULONGLONG t0 = GetTickCount64();
        const bool showPath = flat_ || !query_.empty();
        std::vector<uint32_t> ids = !query_.empty() ? tree_->Search(curNode_, query_, sortKey_, sortAsc_)
                                    : flat_         ? tree_->Descendants(curNode_, sortKey_, sortAsc_, false)  // list everything: only files
                                                    : tree_->Children(curNode_, sortKey_, sortAsc_);
        std::vector<Windows::Foundation::IInspectable> rows;
        rows.reserve(ids.size());
        const bool thumbMode = ThumbsActive();
        const ThumbDims td = ThumbDimsFor(settings_.thumbSize);
        bool anyImage = false;   // thumbnails are shown and this folder has pictures
        if (thumbMode)
            for (uint32_t id : ids)
                if (auto const& n = tree_->Node(id); !n.isDir && lycan_app::IsPreviewableImageName(n.name)) { anyImage = true; break; }
        // The cells of the icon view: all the same size (so that they line up), the size that was chosen (Settings > View): a picture shows its thumbnail as big as the
        // picture area, any other file its icon nearly as big; the name gets as many lines as the longest name needs (at most 2, the narrow cells 3)
        (void)anyImage;
        const double cellW = td.item, cellBox = td.boxH, iconDip = IconDipFor(settings_.thumbSize);
        int nameLines = 1;
        if (viewMode_ == lycan::ViewMode::Icons)
        {
            const int maxLines = cellW <= 104.0 ? 3 : 2;
            for (uint32_t id : ids)
            {
                nameLines = std::max(nameLines, EstimateNameLines(tree_->Node(id).name, cellW - 8.0));
                if (nameLines >= maxLines) { nameLines = maxLines; break; }
            }
        }
        else nameLines = 3;
        const bool solidArchive = current_ && current_->Caps().solid;
        for (uint32_t id : ids)
        {
            auto const& n = tree_->Node(id);
            auto row = make<implementation::EntryRow>(
                id, hstring{n.name}, showPath ? hstring{tree_->ParentPath(id)} : hstring{},
                n.isDir ? hstring{lycan::Tf(L"fmt.n_items_short", n.children.size())} : hstring{FormatSize(n.size)},
                n.isDir ? hstring{} : hstring{FormatSize(n.packedSize)},
                n.mtimeFileTime ? hstring{FormatTime(n.mtimeFileTime)} : hstring{}, n.isDir, n.isEncrypted && !passwordVerified_, showPath);   // after the password was given the lock is gone
            // the columns that are off by default: ratio (a solid archive does not tell the packed size of an item), CRC, method, the lock
            hstring ratioText, crcText, methodText;
            if (!n.isDir)
            {
                ratioText = (n.size == 0 || n.packedSize == 0 || solidArchive) ? hstring{L"\u2014"} : hstring{std::to_wstring(std::lround(100.0 * static_cast<double>(n.packedSize) / static_cast<double>(n.size))) + L"%"};
                if (n.hasCrc) { wchar_t b[16]; swprintf_s(b, L"%08X", n.crc); crcText = b; }
                methodText = hstring{tree_->MethodName(n.methodIdx)};
            }
            row.SetExtra(ratioText, crcText, methodText, n.isEncrypted);
            ApplyShellIcon(row);
            row.SetLayout(cellW, cellBox, nameLines, iconDip);
            rows.push_back(row);
        }
        const ULONGLONG t1 = GetTickCount64();
        entries_ = single_threaded_vector<Windows::Foundation::IInspectable>(std::move(rows));
        ApplyViewMode();  // after switching the view, the other control must release its data source
        auto list = ActiveList();
        list.ItemsSource(entries_);
        const ULONGLONG t2 = GetTickCount64();
        if (!keep.empty())
            for (auto const& it : entries_)
                if (keep.count(it.as<LycanZip::EntryRow>().NodeId())) list.SelectedItems().Append(it);

        RefreshShellIcons();   // the kinds of file that have no icon yet are loaded now
        UpdateListColumns();
        if (ids.size() >= 5000 || GetTickCount64() - t0 >= 300) lycan::diag::Info(L"list{}: {} rows, {} ms to get and build them, {} ms to hand them to the list, {} ms for the rest", query_.empty() ? L"" : L" (search)", ids.size(), t1 - t0, t2 - t1, GetTickCount64() - t2);
        auto label = [&](TextBlock const& tb, const wchar_t* base, lycan::SortKey k) {
            std::wstring s = base;
            if (sortKey_ == k) s += sortAsc_ ? L" ▲" : L" ▼";
            tb.Text(hstring{s});
        };
        label(SortNameText(), lycan::Tr(L"main.name"), lycan::SortKey::Name);
        label(SortSizeText(), lycan::Tr(L"main.size"), lycan::SortKey::Size);
        label(SortPackedText(), lycan::Tr(L"main.packed"), lycan::SortKey::PackedSize);
        label(SortModifiedText(), lycan::Tr(L"main.modified"), lycan::SortKey::Modified);
        label(SortRatioText(), lycan::Tr(L"main.ratio"), lycan::SortKey::Ratio);
        label(SortEncText(), lycan::Tr(L"main.col_encrypted"), lycan::SortKey::Encrypted);
        label(SortMethodText(), lycan::Tr(L"main.col_method"), lycan::SortKey::Method);
        label(SortCrcText(), lycan::Tr(L"main.col_crc"), lycan::SortKey::Crc);
        label(SortTypeText(), lycan::Tr(L"main.type"), lycan::SortKey::Type);
        label(SortPathText(), lycan::Tr(L"main.location"), lycan::SortKey::Path);

        // Breadcrumb
        crumbNodes_.clear();
        for (uint32_t x = curNode_;; x = tree_->Node(x).parent)
        {
            crumbNodes_.push_back(x);
            if (x == tree_->Root()) break;
        }
        std::reverse(crumbNodes_.begin(), crumbNodes_.end());
        std::vector<Windows::Foundation::IInspectable> names;
        for (uint32_t x : crumbNodes_)
            names.push_back(make<implementation::CrumbItem>(x == tree_->Root() ? hstring{currentPath_.filename().wstring()} : hstring{tree_->Node(x).name}, x == curNode_));
        Crumbs().ItemsSource(single_threaded_vector<Windows::Foundation::IInspectable>(std::move(names)));
        UpButton().IsEnabled(curNode_ != tree_->Root());
        UpdateViewMenuChecks();

        std::wstring status = lycan::Tf(L"fmt.n_items", ids.size());
        if (!query_.empty()) status = lycan::Tf(L"main.found_matching_including", ids.size(), query_);
        else if (flat_) status += lycan::Tr(L"main.list_all_suffix");
        const std::wstring noThumbs = thumbMode && anyImage ? ThumbUnavailableReason() : std::wstring{};
        if (!noThumbs.empty()) status += std::wstring{lycan::Tr(L"fmt.dot_gap")} + noThumbs;  // a permanent note: nothing is broken, this kind of archive makes no thumbnails
        status = archiveInfo_ + std::wstring{lycan::Tr(L"fmt.bar_gap")} + status;
        ArchiveStatus().Text(hstring{status});
        Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(ArchiveStatus(), box_value(hstring{status}));
        UpdateSelectionUi();
        if (!noThumbs.empty() && !thumbNoteShown_)
        {
            thumbNoteShown_ = true;
            ShowStatusNote(lycan::Tf(L"main.image_thumbnails_on_but_double", noThumbs));
        }
    }

    // How many lines a name needs in a cell of this width (an estimate: CJK characters are about twice as wide as Latin ones; a little room is kept for the word breaks)
    static int EstimateNameLines(std::wstring const& name, double width)
    {
        double w = 0;
        for (wchar_t c : name) w += (c >= 0x2E80 || (c >= 0xFF00 && c <= 0xFFEF)) ? 14.0 : (c == L' ' ? 4.0 : 7.6);
        return std::max(1, static_cast<int>(std::ceil(w / (width * 0.9))));
    }

    // The size of the icon of a file that is not a picture in the icon view: nearly as big as the picture area
    double MainWindow::IconDipFor(unsigned size)
    {
        const ThumbDims td = ThumbDimsFor(size);
        return std::round((std::min)(td.boxH, td.item - 16.0) * 0.8);
    }

    // Size of the thumbnails of the icon view: item width, height of the thumbnail area, upper limit of the decoded width
    MainWindow::ThumbDims MainWindow::ThumbDimsFor(unsigned size)
    {
        switch (size)
        {
            case 0: return {104.0, 60.0, 128};
            case 2: return {208.0, 156.0, 320};
            case 3: return {312.0, 234.0, 480};   // 1.5 times the large one (a 4K screen)
            default: return {136.0, 96.0, 192};
        }
    }

    std::wstring MainWindow::ThumbUnavailableReason() const
    {
        if (!current_) return L"";
        auto caps = current_->Caps();
        if (caps.solid) return lycan::Tr(L"main.archive_solid_so_no_thumbnails");
        if (!caps.randomAccess) return lycan::Tr(L"main.archive_format_cannot_read_single");
        return L"";
    }

    // The reading session is made when a file is first needed (an encrypted archive waits until the password is verified)
    bool MainWindow::EnsureThumbSession()
    {
        if (!current_ || !ThumbUnavailableReason().empty()) return false;
        if (current_->Caps().encrypted && !passwordVerified_) return false;
        const std::wstring key = currentPath_.wstring() + L"|" + password_;
        if (key != thumbSessionKey_)
        {
            thumbs_.Reset(currentPath_, MakeOpenOptions(), true);
            thumbSessionKey_ = key;
        }
        return true;
    }

    void MainWindow::BindThumb(LycanZip::EntryRow const& row)
    {
        if (!row || !tree_ || !ThumbsActive() || row.NodeId() >= tree_->NodeCount()) return;
        auto const& n = tree_->Node(row.NodeId());
        if (n.isDir || (n.isEncrypted && !passwordVerified_) || n.entryIndex == lycan::kNoEntry || !lycan_app::CanPreviewImage(n.name) || lycan::IsMacMetadataPath(n.path)) return;
        if (!EnsureThumbSession()) return;
        const uint32_t px = ThumbDimsFor(settings_.thumbSize).px;
        if (auto img = thumbs_.Get(n.entryIndex, px)) { row.SetThumb(img.as<Microsoft::UI::Xaml::Media::ImageSource>()); return; }
        thumbs_.Request(n.entryIndex, n.size, px, [row](Windows::Foundation::IInspectable const& i) {
            if (auto src = i.try_as<Microsoft::UI::Xaml::Media::ImageSource>()) row.SetThumb(src);
        });
    }

    void MainWindow::OnCrumbClicked(BreadcrumbBar const&, BreadcrumbBarItemClickedEventArgs const& args)
    {
        auto i = static_cast<size_t>(args.Index());
        if (i < crumbNodes_.size()) NavigateTo(crumbNodes_[i], true);
    }

    void MainWindow::OnUpClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        if (tree_ && curNode_ != tree_->Root()) NavigateTo(tree_->Node(curNode_).parent, true);
    }

    void MainWindow::OnSortClick(Windows::Foundation::IInspectable const& sender, RoutedEventArgs const&)
    {
        if (!tree_) return;
        static constexpr lycan::SortKey kKeys[] = {lycan::SortKey::Name, lycan::SortKey::Size, lycan::SortKey::PackedSize, lycan::SortKey::Modified, lycan::SortKey::Type, lycan::SortKey::Path,
                                                   lycan::SortKey::Ratio, lycan::SortKey::Encrypted, lycan::SortKey::Crc, lycan::SortKey::Method};
        int i = _wtoi(unbox_value<hstring>(sender.as<FrameworkElement>().Tag()).c_str());
        lycan::SortKey k = kKeys[std::clamp(i, 0, 9)];
        if (k == sortKey_) sortAsc_ = !sortAsc_;
        else { sortKey_ = k; sortAsc_ = true; }
        RefreshList(true);  // sorting must not clear the items the user has ticked
    }

    void MainWindow::OnSearchTextChanged(Windows::Foundation::IInspectable const&, TextChangedEventArgs const&)
    {
        if (syncingUi_ || !tree_) return;
        if (!searchTimer_)
        {
            searchTimer_ = dispatcher_.CreateTimer();
            searchTimer_.Interval(250ms);  // search only after typing pauses, so a large archive does not recompute at every key
            searchTimer_.IsRepeating(false);
            searchTimer_.Tick([weak = get_weak()](auto&&, auto&&) {
                auto self = weak.get();
                if (!self || !self->tree_) return;
                std::wstring q{self->SearchBox().Text()};
                while (!q.empty() && iswspace(q.back())) q.pop_back();
                size_t b = 0;
                while (b < q.size() && iswspace(q[b])) ++b;
                q.erase(0, b);
                if (q == self->query_) return;
                self->query_ = q;
                self->RefreshList();
            });
        }
        searchTimer_.Stop();
        searchTimer_.Start();
    }

    void MainWindow::OnFlatToggleClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        if (syncingUi_ || !tree_) return;
        flat_ = FlatToggle().IsChecked();
        RefreshList();
    }

    // View menu: show the clutter macOS adds (__MACOSX, ._ files, .DS_Store)
    void MainWindow::OnMacMetaItemClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        if (syncingUi_) return;
        settings_.hideMacMetadata = !MacMetaItem().IsChecked();
        const bool was = loadingSettings_;
        loadingSettings_ = true;
        HideMacMetaToggle().IsOn(settings_.hideMacMetadata);
        loadingSettings_ = was;
        SaveSettings();
        RebuildTree();
    }

    // A new tree of the open archive (the entries are the same, only the hidden ones differ); stays in the same folder
    void MainWindow::RebuildTree()
    {
        if (!current_ || !tree_) return;
        const std::wstring where = curNode_ < tree_->NodeCount() ? tree_->Node(curNode_).path : std::wstring();
        lycan_app::CloseImagePreview(this);  // the previews hold node numbers of the old tree
        lycan_app::CloseAudioPreview(this);
        tree_ = std::make_shared<lycan::ArchiveTree>(current_->Entries(), settings_.hideMacMetadata);
        tree_->SetMethodNames(current_->MethodNames());
        BuildFolderTree();
        uint32_t n = where.empty() ? tree_->Root() : tree_->Find(where);
        if (n == lycan::kNoNode || !tree_->Node(n).isDir) n = tree_->Root();
        NavigateTo(n, true);
    }

    void MainWindow::OnTreeToggleClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        if (syncingUi_) return;
        treeWanted_ = TreeToggle().IsChecked();
        UpdateTreeVisibility();
    }

    void MainWindow::OnRootSizeChanged(Windows::Foundation::IInspectable const&, SizeChangedEventArgs const&)
    {
        UpdateTreeVisibility();
        UpdateCompressLayout();
        UpdateTaskPanelMax();
        UpdateListColumns();
        LayoutHomeScene();
        LayoutArchiveStrip();
        UpdateToolbarText();
    }

    // Settings tabs: one group of settings at a time (the header and the tab strip stay on top)
    void MainWindow::OnSettingsTabChanged(SelectorBar const& bar, SelectorBarSelectionChangedEventArgs const&)
    {
        const auto sel = bar.SelectedItem();
        const std::pair<SelectorBarItem, UIElement> tabs[] = {
            {TabItemGeneral(), TabGeneral()}, {TabItemCompress(), TabCompress()}, {TabItemView(), TabView()},
            {TabItemShell(), TabShell()},     {TabItemPasswords(), TabPasswords()}, {TabItemKeys(), TabKeys()}, {TabItemAbout(), TabAbout()}};
        for (auto const& t : tabs)
        {
            const bool show = sel == t.first;
            const bool wasHidden = t.second.Visibility() == Visibility::Collapsed;
            t.second.Visibility(show ? Visibility::Visible : Visibility::Collapsed);
            if (show && wasHidden) FadeIn(t.second, 8, 180);   // the tab's content fades in
        }
        SettingsScroll().ChangeView(nullptr, 0.0, nullptr, true);
        if (sel == TabItemPasswords()) RebuildPasswordList();   // the saved list is read only when it is looked at
    }

    // Search in the settings. The texts are read from the page itself (what the user sees, after translation, plus the English behind a
    // translated text), so a new card is found without a list to keep up to date. Every word of the query must be in the card or in the
    // heading of its group; cards whose title has every word come first.
    std::vector<MainWindow::SettingHit> MainWindow::FindSettings(std::wstring const& query)
    {
        std::vector<std::wstring> words;
        for (size_t i = 0; i < query.size();)
        {
            while (i < query.size() && std::iswspace(query[i])) ++i;
            size_t j = i;
            while (j < query.size() && !std::iswspace(query[j])) ++j;
            if (j > i) words.push_back(query.substr(i, j - i));
            i = j;
        }
        if (words.empty()) return {};
        auto has = [](std::wstring const& hay, std::wstring const& w) {
            return FindNLSStringEx(LOCALE_NAME_USER_DEFAULT, FIND_FROMSTART | LINGUISTIC_IGNORECASE | LINGUISTIC_IGNOREDIACRITIC | NORM_IGNOREWIDTH | NORM_IGNOREKANATYPE,
                                   hay.c_str(), static_cast<int>(hay.size()), w.c_str(), static_cast<int>(w.size()), nullptr, nullptr, nullptr, 0) >= 0;
        };
        auto hasAll = [&](std::wstring const& hay) { return std::all_of(words.begin(), words.end(), [&](auto const& w) { return has(hay, w); }); };
        auto withEnglish = [](std::wstring const& t) {
            const std::wstring en = lycan::i18n::EnglishOf(t);
            return en.empty() ? t : t + L"\n" + en;
        };

        // The texts of a card: text blocks, the strings of buttons, check boxes and choices, headers; hidden parts are skipped
        std::function<void(DependencyObject const&, std::vector<std::wstring>&)> collect = [&](DependencyObject const& d, std::vector<std::wstring>& out) {
            if (!d) return;
            if (auto u = d.try_as<UIElement>(); u && u.Visibility() == Visibility::Collapsed) return;
            auto str = [&](Windows::Foundation::IInspectable const& v) {
                if (auto s = v.try_as<Windows::Foundation::IReference<hstring>>(); s && !s.Value().empty()) out.emplace_back(s.Value());
            };
            if (auto tb = d.try_as<TextBlock>()) { if (!tb.Text().empty()) out.emplace_back(tb.Text()); return; }
            if (auto ts = d.try_as<ToggleSwitch>()) { str(ts.Header()); return; }
            if (auto nb = d.try_as<NumberBox>()) { str(nb.Header()); return; }
            if (auto cb = d.try_as<ComboBox>())
            {
                str(cb.Header());
                for (auto const& it : cb.Items())
                {
                    if (auto item = it.try_as<ContentControl>()) str(item.Content());
                    else str(it);
                }
                return;
            }
            if (auto cc = d.try_as<ContentControl>())
            {
                auto content = cc.Content();
                if (auto child = content.try_as<DependencyObject>()) collect(child, out);
                else str(content);
                return;
            }
            if (auto panel = d.try_as<Controls::Panel>()) { for (auto const& c : panel.Children()) collect(c, out); return; }
            if (auto border = d.try_as<Controls::Border>()) collect(border.Child(), out);
        };

        const auto cardStyle = RootGrid().Resources().TryLookup(box_value(L"SettingCard")).try_as<Microsoft::UI::Xaml::Style>();
        const std::pair<SelectorBarItem, Controls::Panel> tabs[] = {
            {TabItemGeneral(), TabGeneral()}, {TabItemCompress(), TabCompress()}, {TabItemView(), TabView()},
            {TabItemShell(), TabShell()},     {TabItemPasswords(), TabPasswords()}, {TabItemKeys(), TabKeys()}, {TabItemAbout(), TabAbout()}};
        std::vector<SettingHit> first, rest;
        for (auto const& [tabItem, tabPanel] : tabs)
        {
            std::wstring heading;   // the group the cards below belong to (an icon and a bold text)
            std::function<void(DependencyObject const&)> walk = [&](DependencyObject const& d) {
                if (auto u = d.try_as<UIElement>(); u && u.Visibility() == Visibility::Collapsed) return;
                if (auto border = d.try_as<Controls::Border>())
                {
                    if (cardStyle && border.Style() == cardStyle)
                    {
                        std::vector<std::wstring> texts;
                        collect(border.Child(), texts);
                        if (texts.empty()) return;
                        std::wstring all = withEnglish(heading);
                        for (auto const& t : texts) all += L"\n" + withEnglish(t);
                        if (!hasAll(all)) return;
                        SettingHit hit{tabItem, border, texts.front() + lycan::Tr(L"fmt.dot_gap") + std::wstring(tabItem.Text())};
                        (hasAll(withEnglish(texts.front())) ? first : rest).push_back(std::move(hit));
                        return;
                    }
                    walk(border.Child());
                    return;
                }
                if (auto sp = d.try_as<StackPanel>(); sp && sp.Orientation() == Orientation::Horizontal && sp.Children().Size() == 2 && sp.Children().GetAt(0).try_as<FontIcon>())
                {
                    if (auto tb = sp.Children().GetAt(1).try_as<TextBlock>()) { heading = tb.Text(); return; }
                }
                if (auto panel = d.try_as<Controls::Panel>()) for (auto const& c : panel.Children()) walk(c);
            };
            for (auto const& c : tabPanel.Children()) walk(c);
        }
        first.insert(first.end(), std::make_move_iterator(rest.begin()), std::make_move_iterator(rest.end()));
        return first;
    }

    // Shows the tab of a setting, scrolls its card into view and outlines it for a moment
    void MainWindow::GoToSetting(SettingHit const& hit)
    {
        if (SettingsTabs().SelectedItem() != hit.tab) SettingsTabs().SelectedItem(hit.tab);
        dispatcher_.TryEnqueue(Microsoft::UI::Dispatching::DispatcherQueuePriority::Low, [weak = get_weak(), card = hit.card] {
            auto s = weak.get();
            if (!s) return;
            BringIntoViewOptions opt;
            opt.VerticalAlignmentRatio(0.2);
            opt.AnimationDesired(s->AnimOn());
            card.StartBringIntoView(opt);
            if (s->settingFlashCard_ && s->settingFlashCard_ != card)
            {
                s->settingFlashCard_.ClearValue(Controls::Border::BorderBrushProperty());
                s->settingFlashCard_.ClearValue(Controls::Border::BorderThicknessProperty());
            }
            card.BorderBrush(Application::Current().Resources().Lookup(box_value(L"AccentFillColorDefaultBrush")).as<Microsoft::UI::Xaml::Media::Brush>());
            card.BorderThickness(ThicknessHelper::FromUniformLength(2));
            s->settingFlashCard_ = card;
            if (!s->settingFlashTimer_)
            {
                s->settingFlashTimer_ = s->dispatcher_.CreateTimer();
                s->settingFlashTimer_.IsRepeating(false);
                s->settingFlashTimer_.Interval(1600ms);
                s->settingFlashTimer_.Tick([weak = s->get_weak()](auto&&, auto&&) {
                    auto self = weak.get();
                    if (!self || !self->settingFlashCard_) return;
                    self->settingFlashCard_.ClearValue(Controls::Border::BorderBrushProperty());   // back to the style's border
                    self->settingFlashCard_.ClearValue(Controls::Border::BorderThicknessProperty());
                    self->settingFlashCard_ = nullptr;
                });
            }
            s->settingFlashTimer_.Stop();
            s->settingFlashTimer_.Start();
        });
    }

    void MainWindow::OnSettingsSearchTextChanged(AutoSuggestBox const& box, AutoSuggestBoxTextChangedEventArgs const& args)
    {
        if (args.Reason() != AutoSuggestionBoxTextChangeReason::UserInput) return;
        settingHits_ = FindSettings(std::wstring(box.Text()));
        auto items = single_threaded_vector<Windows::Foundation::IInspectable>();
        for (auto const& h : settingHits_) items.Append(box_value(hstring{h.label}));
        if (settingHits_.empty() && !std::all_of(box.Text().begin(), box.Text().end(), [](wchar_t c) { return std::iswspace(c); }))
            items.Append(box_value(hstring{lycan::Tr(L"main.no_settings_match")}));   // not a setting: choosing it does nothing
        box.ItemsSource(items);
    }

    // Esc: the box closes its list itself; with the list closed it clears the search, and in an empty box it leaves the settings. Handled
    // here: the AutoSuggestBox would take the Esc (the shortcut of the window never sees it) and put back the last typed text.
    void MainWindow::OnSettingsSearchKeyDown(Windows::Foundation::IInspectable const&, Input::KeyRoutedEventArgs const& e)
    {
        if (e.Key() != Windows::System::VirtualKey::Escape) return;
        auto box = SettingsSearch();
        if (box.IsSuggestionListOpen()) return;
        e.Handled(true);
        if (box.Text().empty()) { OnSettingsBackClick(nullptr, nullptr); return; }
        box.Text(L"");
        settingHits_.clear();
        box.ItemsSource(nullptr);
    }

    void MainWindow::OnSettingsSearchSubmitted(AutoSuggestBox const& box, AutoSuggestBoxQuerySubmittedEventArgs const& args)
    {
        if (settingHits_.empty()) return;
        size_t pick = 0;   // Enter without choosing: the first one
        if (auto chosen = args.ChosenSuggestion())
        {
            const auto label = unbox_value_or<hstring>(chosen, L"");
            auto it = std::find_if(settingHits_.begin(), settingHits_.end(), [&](auto const& h) { return h.label == std::wstring_view(label); });
            if (it == settingHits_.end()) return;
            pick = static_cast<size_t>(it - settingHits_.begin());
        }
        box.IsSuggestionListOpen(false);
        GoToSetting(settingHits_[pick]);
    }

    // The left column: the folder tree and / or the preview pane (alone, either takes the whole column). When the window is too narrow
    // both are hidden automatically to leave the room for the list; the compact window has no preview pane.
    void MainWindow::UpdateTreeVisibility()
    {
        const bool wide = RootGrid().ActualWidth() >= 640;
        const bool tree = treeWanted_ && wide;
        const bool pane = settings_.previewPane && wide && !compact_ && !previewOnly_;
        const bool left = tree || pane;
        TreePane().Visibility(tree ? Visibility::Visible : Visibility::Collapsed);
        PreviewPaneHost().Visibility(pane ? Visibility::Visible : Visibility::Collapsed);
        PaneSplitter().Visibility(tree && pane ? Visibility::Visible : Visibility::Collapsed);
        TreeRow().Height(tree ? GridLength{1, GridUnitType::Star} : GridLength{0, GridUnitType::Pixel});
        PaneSplitRow().Height({tree && pane ? 8.0 : 0.0, GridUnitType::Pixel});
        PaneRow().Height(!pane ? GridLength{0, GridUnitType::Pixel} : tree ? GridLength{PaneHeight(), GridUnitType::Pixel} : GridLength{1, GridUnitType::Star});
        TreeCol().Width({left ? static_cast<double>(settings_.treeWidth) : 0.0, GridUnitType::Pixel});
        SplitCol().Width({left ? 10.0 : 0.0, GridUnitType::Pixel});
        TreeSplitter().Visibility(left ? Visibility::Visible : Visibility::Collapsed);
        if (previewPane_)
        {
            const bool wasActive = paneShown_;
            paneShown_ = pane;
            previewPane_->SetActive(pane);
            if (pane && !wasActive) SchedulePanePreview();
        }
    }

    // The height of the preview pane under the tree: the setting, at most 70 % of the left column
    double MainWindow::PaneHeight()
    {
        double h = static_cast<double>(settings_.previewPaneHeight);
        const double column = LeftPane().ActualHeight();
        if (column > 0) h = std::min(h, std::max(120.0, column * 0.7));
        return std::max(120.0, h);
    }

    void MainWindow::InitPreviewPane()
    {
        previewPane_ = std::make_unique<lycan_app::PreviewPane>();
        PreviewPaneHost().Child(previewPane_->Root());
        auto weak = get_weak();
        previewPane_->onPlayAudio = [weak](uint32_t node) { if (auto s = weak.get()) s->PreviewAudio(node); };
        previewPane_->onEnlarge = [weak] { if (auto s = weak.get()) s->OpenLargePreview(); };
        previewPane_->onPassword = [weak]() -> fire_and_forget {
            auto s = weak.get();
            if (!s) co_return;
            if (co_await s->EnsurePasswordAsync()) s->SchedulePanePreview();
        };
        // a taller / shorter column keeps the pane within 70 % of it (only the row: the width is the tree splitter's, also while it is dragged)
        LeftPane().SizeChanged([weak](auto&&, SizeChangedEventArgs const& e) {
            auto s = weak.get();
            if (!s || e.PreviousSize().Height == e.NewSize().Height || s->paneDragging_) return;
            if (s->TreePane().Visibility() == Visibility::Visible && s->PreviewPaneHost().Visibility() == Visibility::Visible)
                s->PaneRow().Height({s->PaneHeight(), GridUnitType::Pixel});
        });

        // the splitter between the tree and the pane: drag up / down, double click restores the default height
        auto splitter = PaneSplitter();
        splitter.PointerEntered([weak](auto&&, auto&&) { if (auto s = weak.get()) s->PaneSplitterLine().Opacity(0.6); });
        splitter.PointerExited([weak](auto&&, auto&&) { if (auto s = weak.get()) if (!s->paneDragging_) s->PaneSplitterLine().Opacity(0.0); });
        splitter.PointerPressed([weak](Windows::Foundation::IInspectable const& sender, Input::PointerRoutedEventArgs const& e) {
            auto s = weak.get();
            if (!s) return;
            s->paneDragging_ = true;
            s->paneDragStartY_ = e.GetCurrentPoint(s->RootGrid()).Position().Y;
            s->paneDragStartH_ = s->PaneRow().ActualHeight();
            sender.as<UIElement>().CapturePointer(e.Pointer());
            s->PaneSplitterLine().Opacity(1.0);
            e.Handled(true);
        });
        splitter.PointerMoved([weak](Windows::Foundation::IInspectable const&, Input::PointerRoutedEventArgs const& e) {
            auto s = weak.get();
            if (!s || !s->paneDragging_) return;
            const double dy = e.GetCurrentPoint(s->RootGrid()).Position().Y - s->paneDragStartY_;
            const double maxH = std::max(120.0, s->LeftPane().ActualHeight() * 0.7);
            s->PaneRow().Height({std::clamp(s->paneDragStartH_ - dy, 120.0, maxH), GridUnitType::Pixel});   // dragging up makes the pane taller
        });
        auto finish = [weak](Windows::Foundation::IInspectable const& sender, Input::PointerRoutedEventArgs const& e) {
            auto s = weak.get();
            if (!s || !s->paneDragging_) return;
            s->paneDragging_ = false;
            sender.as<UIElement>().ReleasePointerCapture(e.Pointer());
            s->PaneSplitterLine().Opacity(0.0);
            s->settings_.previewPaneHeight = static_cast<unsigned>(std::clamp(std::lround(s->PaneRow().ActualHeight()), 120L, 1200L));
            s->SaveSettings();
        };
        splitter.PointerReleased(finish);
        splitter.DoubleTapped([weak](auto&&, auto&&) {
            if (auto s = weak.get()) { s->settings_.previewPaneHeight = 260; s->SaveSettings(); s->UpdateTreeVisibility(); }
        });
    }

    // The selection changed: the pane follows once it stops moving (an arrow key held down reads nothing on the way)
    void MainWindow::SchedulePanePreview()
    {
        if (!previewPane_ || (!paneShown_ && !previewPane_->LargeOpen())) return;
        if (!paneTimer_)
        {
            paneTimer_ = dispatcher_.CreateTimer();
            paneTimer_.IsRepeating(false);
            paneTimer_.Tick([weak = get_weak()](auto&&, auto&&) { if (auto s = weak.get()) s->UpdatePanePreview(); });
        }
        // a solid archive unpacks from the start of its block: wait a little longer before starting
        paneTimer_.Interval(current_ && current_->Caps().solid ? 250ms : 150ms);
        paneTimer_.Stop();
        paneTimer_.Start();
    }

    void MainWindow::UpdatePanePreview()
    {
        if (!previewPane_) return;
        if (!current_ || !tree_ || view_ != View::Archive) { previewPane_->Show({}); return; }
        const std::wstring key = currentPath_.wstring() + L"|" + password_ + L"|" + std::to_wstring(currentCp_) + L"|" +
                                 std::to_wstring(reinterpret_cast<uintptr_t>(current_.get()));
        if (key != paneKey_)
        {
            previewPane_->Reset(currentPath_, MakeOpenOptions());
            paneKey_ = key;
        }
        const double scale = RootGrid().XamlRoot() ? RootGrid().XamlRoot().RasterizationScale() : 1.0;
        auto icon = [&](std::wstring const& iconKey) -> Microsoft::UI::Xaml::Media::ImageSource {
            auto px = lycan_app::LoadShellIconPixels(iconKey, lycan_app::ShellIconPx(56, scale));
            return px.empty() ? nullptr : lycan_app::MakeIconBitmap(px);
        };
        auto extKey = [](std::wstring const& name) {
            const auto dot = name.find_last_of(L'.');
            if (dot == std::wstring::npos || dot == 0) return std::wstring(L".");
            std::wstring e = name.substr(dot);
            std::transform(e.begin(), e.end(), e.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
            return e;
        };
        auto ratio = [](uint64_t packed, uint64_t size) { return std::to_wstring(size ? std::lround(100.0 * static_cast<double>(packed) / static_cast<double>(size)) : 0) + L"%"; };

        lycan_app::PaneInfo info;
        const auto nodes = SelectedNodes();
        auto const& all = current_->Entries();
        if (nodes.empty())
        {   // nothing selected: the archive itself
            uint64_t files = 0, dirs = 0, unpacked = 0, packed = 0;
            uint32_t encrypted = 0;
            for (auto const& e : all)
            {
                if (e.isDir) { ++dirs; continue; }
                ++files;
                unpacked += e.size;
                packed += e.packedSize;
                if (e.isEncrypted) ++encrypted;
            }
            info.kind = lycan_app::PaneKind::Archive;
            info.title = currentPath_.filename().wstring();
            info.icon = icon(extKey(info.title));
            info.rows.emplace_back(lycan::Tr(L"main.format"), current_->FormatName());
            info.rows.emplace_back(lycan::Tr(L"main.properties"), lycan::Tf(L"main.file_s_folder_s", files, dirs));
            info.rows.emplace_back(lycan::Tr(L"main.original_size"), FormatSize(unpacked));
            if (packed) info.rows.emplace_back(lycan::Tr(L"main.packed_size"), FormatSize(packed) + L" (" + ratio(packed, unpacked) + L")");
            info.rows.emplace_back(lycan::Tr(L"main.solid"), current_->Caps().solid ? lycan::Tr(L"main.yes") : lycan::Tr(L"main.no"));
            if (encrypted) info.rows.emplace_back(lycan::Tr(L"main.info_encrypted_label"), lycan::Tf(L"main.yes_encrypted_file_s", encrypted));
        }
        else if (nodes.size() > 1)
        {
            uint64_t bytes = 0;
            for (uint32_t i : tree_->EntryIndices(nodes))
                if (!all[i].isDir) bytes += all[i].size;
            info.kind = lycan_app::PaneKind::Multi;
            info.title = lycan::Tf(L"main.selected", nodes.size(), FormatSize(bytes));
            info.glyph = L"";
        }
        else if (auto const& n = tree_->Node(nodes[0]); n.isDir)
        {
            info.kind = lycan_app::PaneKind::Folder;
            info.title = n.name;
            info.icon = icon(L"<dir>");
            info.rows.emplace_back(lycan::Tr(L"main.properties"), lycan::Tf(L"fmt.n_files", n.fileCount));
            info.rows.emplace_back(lycan::Tr(L"main.size"), FormatSize(n.totalSize));
        }
        else
        {
            info.kind = lycan_app::PaneKind::File;
            info.title = n.name;
            info.node = nodes[0];
            info.entryIndex = n.entryIndex;
            info.size = n.size;
            info.locked = n.isEncrypted && !passwordVerified_;
            info.icon = icon(extKey(n.name));
            info.rows.emplace_back(lycan::Tr(L"main.size"), FormatSize(n.size));
            if (n.packedSize) info.rows.emplace_back(lycan::Tr(L"main.packed"), FormatSize(n.packedSize) + L" (" + ratio(n.packedSize, n.size) + L")");
            if (n.mtimeFileTime) info.rows.emplace_back(lycan::Tr(L"main.modified"), FormatTime(n.mtimeFileTime));
            if (auto m = tree_->MethodName(n.methodIdx); !m.empty()) info.rows.emplace_back(lycan::Tr(L"main.col_method"), m);
            if (n.hasCrc)
            {
                wchar_t crc[16];
                swprintf_s(crc, L"%08X", n.crc);
                info.rows.emplace_back(lycan::Tr(L"main.col_crc"), crc);
            }
            if (n.isEncrypted) info.rows.emplace_back(lycan::Tr(L"main.info_encrypted_label"), lycan::Tr(L"main.yes"));
            if (n.entryIndex == lycan::kNoEntry) info.kind = lycan_app::PaneKind::Folder;   // (cannot happen for a file; nothing to read)
        }
        previewPane_->Show(std::move(info));
    }

    void MainWindow::OnPaneToggleClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        if (syncingUi_) return;
        settings_.previewPane = PaneToggle().IsChecked();
        SaveSettings();
        loadingSettings_ = true;   // the settings page shows the same switch
        PreviewPaneToggle().IsOn(settings_.previewPane);
        loadingSettings_ = false;
        UpdateTreeVisibility();
    }

    // The larger preview window: the same content as the pane, following the selection
    void MainWindow::OpenLargePreview()
    {
        if (!previewPane_) return;
        lycan_app::PreviewPane::LargeOptions o;
        o.rect = settings_.previewWindowRect;
        o.theme = RootGrid().RequestedTheme();
        o.owner = Hwnd();
        o.onClosed = [weak = get_weak()](std::string const& rect) {
            if (auto s = weak.get(); s && !rect.empty() && rect != s->settings_.previewWindowRect)
            {
                s->settings_.previewWindowRect = rect;
                s->SaveSettings();
            }
        };
        previewPane_->OpenLarge(std::move(o));
    }

    // The splitter of the folder tree (drag to change the width, double click to restore) and the context menu of the column headers (choose which columns to show)
    void MainWindow::InitTreeSplitter()
    {
        auto splitter = TreeSplitter();
        auto weak = get_weak();
        splitter.PointerEntered([weak](auto&&, auto&&) { if (auto s = weak.get()) s->TreeSplitterLine().Opacity(0.6); });
        splitter.PointerExited([weak](auto&&, auto&&) { if (auto s = weak.get()) if (!s->treeDragging_) s->TreeSplitterLine().Opacity(0.0); });
        splitter.PointerPressed([weak](Windows::Foundation::IInspectable const& sender, Input::PointerRoutedEventArgs const& e) {
            auto s = weak.get();
            if (!s) return;
            s->treeDragging_ = true;
            s->treeDragStartX_ = e.GetCurrentPoint(s->RootGrid()).Position().X;
            s->treeDragStartW_ = s->settings_.treeWidth;
            sender.as<UIElement>().CapturePointer(e.Pointer());
            s->TreeSplitterLine().Opacity(1.0);
            e.Handled(true);
        });
        splitter.PointerMoved([weak](Windows::Foundation::IInspectable const&, Input::PointerRoutedEventArgs const& e) {
            auto s = weak.get();
            if (!s || !s->treeDragging_) return;
            const double dx = e.GetCurrentPoint(s->RootGrid()).Position().X - s->treeDragStartX_;
            const double maxW = std::min(420.0, std::max(140.0, s->RootGrid().ActualWidth() * 0.5));
            const double w = std::clamp(s->treeDragStartW_ + dx, 140.0, maxW);
            s->TreeCol().Width({w, GridUnitType::Pixel});
            s->treeWidth_ = w;
        });
        auto finish = [weak](Windows::Foundation::IInspectable const& sender, Input::PointerRoutedEventArgs const& e) {
            auto s = weak.get();
            if (!s || !s->treeDragging_) return;
            s->treeDragging_ = false;
            sender.as<UIElement>().ReleasePointerCapture(e.Pointer());
            s->TreeSplitterLine().Opacity(0.0);
            s->settings_.treeWidth = static_cast<unsigned>(std::lround(s->treeWidth_));
            s->SaveSettings();
        };
        splitter.PointerReleased(finish);
        splitter.DoubleTapped([weak](auto&&, auto&&) {
            if (auto s = weak.get()) { s->settings_.treeWidth = 200; s->SaveSettings(); s->UpdateTreeVisibility(); }
        });

        // Right click on a column header: tick the columns to show (the name is always shown)
        ListHeader().ContextRequested([weak](UIElement const& sender, Input::ContextRequestedEventArgs const& args) {
            auto s = weak.get();
            if (!s) return;
            args.Handled(true);
            MenuFlyout fly;
            struct Col { const wchar_t* text; unsigned bit; };
            for (Col c : {Col{lycan::Tr(L"main.type"), 1}, Col{lycan::Tr(L"main.size"), 2}, Col{lycan::Tr(L"main.packed"), 4}, Col{lycan::Tr(L"main.ratio"), 16}, Col{lycan::Tr(L"main.modified"), 8},
                          Col{lycan::Tr(L"main.col_encrypted"), 32}, Col{lycan::Tr(L"main.col_method"), 128}, Col{lycan::Tr(L"main.col_crc"), 64}})
            {
                ToggleMenuFlyoutItem it;
                it.Text(c.text);
                it.IsChecked((s->settings_.listColumns & c.bit) != 0);
                it.Click([weak, bit = c.bit](auto&&, auto&&) {
                    if (auto self = weak.get())
                    {
                        self->settings_.listColumns ^= bit;
                        self->SaveSettings();
                        self->UpdateListColumns();
                    }
                });
                fly.Items().Append(it);
            }
            fly.Items().Append(MenuFlyoutSeparator{});
            MenuFlyoutItem reset;
            reset.Text(lycan::Tr(L"main.reset_column_widths"));
            reset.Click([weak](auto&&, auto&&) { if (auto self = weak.get()) self->ResetColWidths(); });
            fly.Items().Append(reset);
            lycan_app::RefreshCursorWhenOpened(fly);
            Windows::Foundation::Point pt{};
            if (args.TryGetPosition(sender, pt))
            {
                Primitives::FlyoutShowOptions opt;
                opt.Position(pt);
                fly.ShowAt(sender, opt);
            }
            else fly.ShowAt(sender.as<FrameworkElement>());
        });
    }

    void MainWindow::OnFindAccelerator(Input::KeyboardAccelerator const&, Input::KeyboardAcceleratorInvokedEventArgs const& args)
    {
        if (view_ == View::Settings)
        {
            SettingsSearch().Focus(FocusState::Programmatic);
            args.Handled(true);
            return;
        }
        if (view_ != View::Archive) return;
        SearchBox().Focus(FocusState::Programmatic);
        args.Handled(true);
    }

    ListViewBase MainWindow::ActiveList()
    {
        return viewMode_ == lycan::ViewMode::Details ? EntryList().as<ListViewBase>() : EntryGrid().as<ListViewBase>();
    }

    // A ListView for the details (with column headers); list / icons share one GridView, only the template and the panel change, so selection, sorting and right click logic are exactly the same
    void MainWindow::ApplyViewMode()
    {
        const bool details = viewMode_ == lycan::ViewMode::Details;
        EntryList().Visibility(details ? Visibility::Visible : Visibility::Collapsed);
        HeaderViewport().Visibility(details ? Visibility::Visible : Visibility::Collapsed);
        EntryGrid().Visibility(details ? Visibility::Collapsed : Visibility::Visible);
        (details ? EntryGrid().as<ListViewBase>() : EntryList().as<ListViewBase>()).ItemsSource(nullptr);
        if (details) return;
        const bool icons = viewMode_ == lycan::ViewMode::Icons;
        if (!icons)
        {
            // list: a GridViewItem is very tall by default (about 96px), make it the same row height as the details
            try
            {
                Style style{xaml_typename<GridViewItem>()};
                style.BasedOn(Application::Current().Resources().Lookup(box_value(L"DefaultGridViewItemStyle")).as<Style>());
                style.Setters().Append(Setter(FrameworkElement::MinHeightProperty(), box_value(settings_.compactRows ? 30.0 : 40.0)));
                style.Setters().Append(Setter(FrameworkElement::MinWidthProperty(), box_value(0.0)));
                EntryGrid().ItemContainerStyle(style);
            }
            catch (...) {}
        }
        else
        {
            // icons: leave a gap of 4 between items (the gap belongs to no item, so pressing and dragging there is a band selection); remove the default minimum size and padding
            try
            {
                Style style{xaml_typename<GridViewItem>()};
                style.BasedOn(Application::Current().Resources().Lookup(box_value(L"DefaultGridViewItemStyle")).as<Style>());
                style.Setters().Append(Setter(FrameworkElement::MarginProperty(), box_value(ThicknessHelper::FromUniformLength(4))));
                style.Setters().Append(Setter(Control::PaddingProperty(), box_value(ThicknessHelper::FromUniformLength(0))));
                style.Setters().Append(Setter(FrameworkElement::MinWidthProperty(), box_value(0.0)));
                style.Setters().Append(Setter(FrameworkElement::MinHeightProperty(), box_value(0.0)));
                EntryGrid().ItemContainerStyle(style);
            }
            catch (...) { EntryGrid().ItemContainerStyle(nullptr); }
        }
        auto res = RootGrid().Resources();
        EntryGrid().ItemTemplate(res.Lookup(box_value(icons ? L"IconItemTemplate" : L"ListItemTemplate")).as<DataTemplate>());
        EntryGrid().ItemsPanel(res.Lookup(box_value(icons ? L"IconPanel" : L"ListPanel")).as<ItemsPanelTemplate>());
        // icons: laid out horizontally, scrolls vertically; list: top to bottom, then to the right, scrolls horizontally (like the "List" of Explorer)
        ScrollViewer::SetHorizontalScrollBarVisibility(EntryGrid(), icons ? ScrollBarVisibility::Disabled : ScrollBarVisibility::Auto);
        ScrollViewer::SetHorizontalScrollMode(EntryGrid(), icons ? ScrollMode::Disabled : ScrollMode::Enabled);
        ScrollViewer::SetVerticalScrollBarVisibility(EntryGrid(), icons ? ScrollBarVisibility::Auto : ScrollBarVisibility::Disabled);
        ScrollViewer::SetVerticalScrollMode(EntryGrid(), icons ? ScrollMode::Enabled : ScrollMode::Disabled);
    }

    void MainWindow::UpdateViewMenuChecks()
    {
        syncingUi_ = true;
        ViewDetailsItem().IsChecked(viewMode_ == lycan::ViewMode::Details);
        ViewListItem().IsChecked(viewMode_ == lycan::ViewMode::List);
        ThumbsItem().IsChecked(settings_.iconThumbnails);
        ThumbsItem().Text(ThumbUnavailableReason().empty() ? hstring{lycan::Tr(L"main.show_image_thumbnails_icon_view")}
                                                           : hstring{lycan::Tf(L"main.show_image_thumbnails_icon_view_not_supported_by_archive", current_ && current_->Caps().solid ? lycan::Tr(L"main.solid_compression") : lycan::Tr(L"main.format_limitation"))});
        MacMetaItem().IsChecked(!settings_.hideMacMetadata);
        const bool icons = viewMode_ == lycan::ViewMode::Icons;
        ThumbSmallItem().IsChecked(icons && settings_.thumbSize == 0);
        ThumbMediumItem().IsChecked(icons && settings_.thumbSize == 1);
        ThumbLargeItem().IsChecked(icons && settings_.thumbSize == 2);
        ThumbHugeItem().IsChecked(icons && settings_.thumbSize == 3);
        SortNameItem().IsChecked(sortKey_ == lycan::SortKey::Name);
        SortSizeItem().IsChecked(sortKey_ == lycan::SortKey::Size);
        SortPackedItem().IsChecked(sortKey_ == lycan::SortKey::PackedSize);
        SortModifiedItem().IsChecked(sortKey_ == lycan::SortKey::Modified);
        SortTypeItem().IsChecked(sortKey_ == lycan::SortKey::Type);
        SortAscItem().IsChecked(sortAsc_);
        SortDescItem().IsChecked(!sortAsc_);
        syncingUi_ = false;
    }

    // The "View" menu: view mode / sort key / ascending or descending (the first letter of Tag is the category)
    void MainWindow::OnViewMenuClick(Windows::Foundation::IInspectable const& sender, RoutedEventArgs const&)
    {
        if (syncingUi_ || !tree_) return;
        std::wstring tag{unbox_value<hstring>(sender.as<FrameworkElement>().Tag())};
        if (tag.size() < 2) return;
        ApplyViewChoice(tag[0], tag[1] - L'0');
    }

    void MainWindow::ApplyViewChoice(wchar_t category, int n)
    {
        if (!tree_) return;
        keepOverride_ = SelectedNodes();  // must be taken before viewMode_ changes (afterwards ActiveList() points to the other control)
        static constexpr lycan::SortKey kKeys[] = {lycan::SortKey::Name, lycan::SortKey::Size, lycan::SortKey::PackedSize, lycan::SortKey::Modified, lycan::SortKey::Type};
        const std::wstring tag{category};
        if (tag[0] == L'v' || tag[0] == L'i')   // v = details / list / icons; i = the icon view in a size (small .. huge)
        {
            viewMode_ = tag[0] == L'i' ? lycan::ViewMode::Icons : static_cast<lycan::ViewMode>(std::clamp(n, 0, 2));
            bool save = false;
            if (tag[0] == L'i' && settings_.thumbSize != static_cast<unsigned>(std::clamp(n, 0, 3)))
            {
                settings_.thumbSize = static_cast<unsigned>(std::clamp(n, 0, 3));   // the size is a setting of its own (the default view of the settings page uses it too)
                save = true;
            }
            if (settings_.rememberViewMode && settings_.viewMode != viewMode_)
            {
                settings_.viewMode = viewMode_;  // remember the view mode chosen last
                save = true;
            }
            if (save) { SaveSettings(); LoadSettingsToUi(); }
        }
        else if (tag[0] == L't') { settings_.iconThumbnails = !settings_.iconThumbnails; thumbNoteShown_ = false; SaveSettings(); }
        else if (tag[0] == L's') sortKey_ = kKeys[std::clamp(n, 0, 4)];
        else if (tag[0] == L'd') sortAsc_ = n != 0;
        RefreshList(true);  // keep the selection
    }

    // Row height: compact / comfortable. It has to be ItemContainerStyle to take effect at once; it must be BasedOn the default style, otherwise the items lose their template and become empty
    void MainWindow::ApplyDensity()
    {
        try
        {
            Style style{xaml_typename<ListViewItem>()};
            style.BasedOn(Application::Current().Resources().Lookup(box_value(L"DefaultListViewItemStyle")).as<Style>());
            style.Setters().Append(Setter(FrameworkElement::MinHeightProperty(), box_value(settings_.compactRows ? 30.0 : 40.0)));
            style.Setters().Append(Setter(Control::HorizontalContentAlignmentProperty(), box_value(HorizontalAlignment::Stretch)));
            style.Setters().Append(Setter(Control::PaddingProperty(), box_value(ThicknessHelper::FromLengths(8, 0, 8, 0))));  // the default left and right padding is too wide; line up with the 8 of the column headers
            EntryList().ItemContainerStyle(style);
        }
        catch (...) {}  // if the default style is not found keep things as they are (default row height)
    }

    // ---- Selection

    // Checkbox (shown when the mouse is over it or it is selected): the pointer events are attached once to each item container, Click once to the checkbox
    void MainWindow::OnContainerChanging(ListViewBase const&, ContainerContentChangingEventArgs const& args)
    {
        if (args.InRecycleQueue())
        {
            // an item that left the screen: release the thumbnail and cancel reads that have not started (the thumbnail stays in the cache, which has a limit)
            if (auto row = args.Item().try_as<LycanZip::EntryRow>())
            {
                row.SetThumb(nullptr);
                if (tree_ && row.NodeId() < tree_->NodeCount()) thumbs_.Cancel(tree_->Node(row.NodeId()).entryIndex);
            }
            return;
        }
        auto item = args.ItemContainer();
        if (GetTickCount64() < listFxUntil_) FadeIn(item, 6, 170, static_cast<int>(std::min<uint32_t>(static_cast<uint32_t>(args.ItemIndex()), 10)) * 15);
        auto rowOf = [](Windows::Foundation::IInspectable const& s) {
            auto it = s.try_as<Primitives::SelectorItem>();
            return it ? it.Content().try_as<LycanZip::EntryRow>() : nullptr;
        };
        if (!item.Tag())
        {
            item.Tag(box_value(L"hooked"));
            item.PointerEntered([rowOf](Windows::Foundation::IInspectable const& s, Input::PointerRoutedEventArgs const&) {
                if (auto row = rowOf(s)) get_self<implementation::EntryRow>(row)->IsHover(true);
            });
            item.PointerExited([rowOf](Windows::Foundation::IInspectable const& s, Input::PointerRoutedEventArgs const&) {
                if (auto row = rowOf(s)) get_self<implementation::EntryRow>(row)->IsHover(false);
            });
        }
        if (auto row = rowOf(item)) get_self<implementation::EntryRow>(row)->IsHover(false);  // when a container is reused clear the old hover state
        auto hook = [weak = get_weak()](ListViewBase const&, ContainerContentChangingEventArgs const& a) {
            auto self = weak.get();
            if (!self || a.InRecycleQueue()) return;
            auto root = a.ItemContainer().ContentTemplateRoot().try_as<FrameworkElement>();
            if (root) self->ApplyRowColumns(root, a.Item().try_as<LycanZip::EntryRow>());
            self->BindThumb(a.Item().try_as<LycanZip::EntryRow>());
            auto cb = root ? root.FindName(L"RowCheck").try_as<CheckBox>() : nullptr;
            if (!cb || cb.Tag()) return;
            cb.Tag(box_value(L"hooked"));
            cb.Click([weak](Windows::Foundation::IInspectable const& s, RoutedEventArgs const&) {
                auto me = weak.get();
                auto box = s.try_as<CheckBox>();
                if (!me || !box) return;
                auto row = RowFromSource(box);
                if (!row) return;
                auto sel = me->ActiveList().SelectedItems();
                uint32_t idx = 0;
                bool present = sel.IndexOf(row, idx);
                if (box.IsChecked() && box.IsChecked().Value()) { if (!present) sel.Append(row); }
                else if (present) sel.RemoveAt(idx);
            });
        };
        if (args.Phase() == 0) args.RegisterUpdateCallback(hook);
        else hook(nullptr, args);
    }

    void MainWindow::OnEntrySelectionChanged(Windows::Foundation::IInspectable const&, SelectionChangedEventArgs const& args)
    {
        for (auto const& it : args.RemovedItems())
            if (auto row = it.try_as<LycanZip::EntryRow>()) get_self<implementation::EntryRow>(row)->IsSelected(false);
        for (auto const& it : args.AddedItems())
            if (auto row = it.try_as<LycanZip::EntryRow>()) get_self<implementation::EntryRow>(row)->IsSelected(true);
        UpdateSelectionUi();
    }

    void MainWindow::ShowStatusNote(std::wstring const& text)
    {
        StatusNoteText().Text(hstring{text});
        StatusNote().Visibility(Visibility::Visible);
        SelectionBar().Visibility(Visibility::Collapsed);
        statusNoteActive_ = true;
        lastNoteSelection_ = SelectedNodes();
        if (!statusNoteTimer_)
        {
            statusNoteTimer_ = dispatcher_.CreateTimer();
            statusNoteTimer_.IsRepeating(false);
            statusNoteTimer_.Interval(std::chrono::seconds(15));
            statusNoteTimer_.Tick([weak = get_weak()](auto&&, auto&&) { if (auto s = weak.get()) { s->HideStatusNote(); s->UpdateSelectionUi(); } });
        }
        statusNoteTimer_.Stop();
        statusNoteTimer_.Start();
    }

    void MainWindow::HideStatusNote()
    {
        if (statusNoteTimer_) statusNoteTimer_.Stop();
        statusNoteActive_ = false;
        StatusNote().Visibility(Visibility::Collapsed);
    }

    void MainWindow::UpdateSelectionUi()
    {
        SchedulePanePreview();
        if (statusNoteActive_ && lastNoteSelection_ == SelectedNodes()) return;  // a message is shown and the selection did not change: keep the message
        HideStatusNote();
        if (!tree_ || !entries_ || !current_) { SelectionBar().Visibility(Visibility::Collapsed); return; }
        auto nodes = SelectedNodes();
        const size_t count = nodes.size();
        SelectionBar().Visibility(count ? Visibility::Visible : Visibility::Collapsed);
        if (count)
        {
            uint64_t bytes = 0;
            auto const& all = current_->Entries();
            for (uint32_t i : tree_->EntryIndices(nodes))
                if (!all[i].isDir) bytes += all[i].size;
            SelectionText().Text(hstring{lycan::Tf(L"main.selected", count, FormatSize(bytes))});
        }
        if (count == 0) SelectAllBox().IsChecked(false);
        else if (count == entries_.Size()) SelectAllBox().IsChecked(true);
        else SelectAllBox().IsChecked(nullptr);  // partly selected: the indeterminate state
    }

    void MainWindow::OnSelectAllClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        if (!entries_) return;
        if (SelectedNodes().size() == entries_.Size()) ActiveList().SelectedItems().Clear();
        else ActiveList().SelectAll();
        UpdateSelectionUi();
    }

    void MainWindow::OnClearSelectionClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        ActiveList().SelectedItems().Clear();
    }

    void MainWindow::OnCopyPathClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) { CopyPaths(SelectedNodes()); }

    void MainWindow::CopyPaths(std::vector<uint32_t> const& nodes)
    {
        if (!tree_ || nodes.empty()) return;
        std::wstring text;
        for (uint32_t n : nodes) text += (text.empty() ? L"" : L"\r\n") + tree_->Node(n).path;
        Windows::ApplicationModel::DataTransfer::DataPackage dp;
        dp.SetText(hstring{text});
        Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(dp);
    }

    // ---- Open / double click / keyboard

    LycanZip::EntryRow MainWindow::RowFromSource(Windows::Foundation::IInspectable const& source)
    {
        auto d = source.try_as<DependencyObject>();
        while (d)
        {
            if (auto item = d.try_as<Primitives::SelectorItem>()) return item.Content().try_as<LycanZip::EntryRow>();
            d = Media::VisualTreeHelper::GetParent(d);
        }
        return nullptr;
    }

    void MainWindow::OpenNode(uint32_t node)
    {
        if (!tree_ || node >= tree_->NodeCount()) return;
        auto const& n = tree_->Node(node);
        if (n.isDir) NavigateTo(node, true);
        else if (settings_.doubleClickPreview && n.entryIndex != lycan::kNoEntry && lycan_app::IsPreviewableImageName(n.name))
        {
            if (lycan_app::IsImageDecoderAvailable(n.name)) PreviewNode(node);
            else
            {
                // a known picture format, but Windows has no decoder (or it is not installed): open with the associated program directly and say why below
                const std::wstring name = n.name;
                OpenEntryFileAsync(node);
                ShowStatusNote(lycan::Tf(L"main.opened_with_associated_app_instead", name, lycan_app::MissingDecoderHint(name)));
            }
        }
        else if (settings_.doubleClickAudioPreview && n.entryIndex != lycan::kNoEntry && lycan_app::IsPreviewableAudioName(n.name)) PreviewAudio(node);
        else OpenEntryFileAsync(node);
    }

    void MainWindow::OnEntryDoubleTapped(Windows::Foundation::IInspectable const&, Input::DoubleTappedRoutedEventArgs const& args)
    {
        for (auto d = args.OriginalSource().try_as<DependencyObject>(); d && !d.try_as<Primitives::SelectorItem>(); d = Media::VisualTreeHelper::GetParent(d))
            if (d.try_as<CheckBox>()) return;  // repeated clicks on the checkbox do not count as "open"
        if (auto row = RowFromSource(args.OriginalSource()))
        {
            args.Handled(true);
            OpenNode(row.NodeId());
        }
    }

    void MainWindow::OnEntryKeyDown(Windows::Foundation::IInspectable const&, Input::KeyRoutedEventArgs const& args)
    {
        if (!tree_) return;
        if (args.Key() == Windows::System::VirtualKey::Enter)
        {
            auto nodes = SelectedNodes();
            if (nodes.size() == 1) { args.Handled(true); OpenNode(nodes[0]); }
        }
        else if (args.Key() == Windows::System::VirtualKey::Back)
        {
            args.Handled(true);
            OnUpClick(nullptr, nullptr);
        }
        else if (args.Key() == Windows::System::VirtualKey::Delete)
        {
            auto nodes = SelectedNodes();
            if (!nodes.empty()) { args.Handled(true); DeleteNodesAsync(std::move(nodes)); }
        }
        else if (args.Key() == Windows::System::VirtualKey::F2)
        {
            auto nodes = SelectedNodes();
            if (nodes.size() == 1) { args.Handled(true); RenameNodeAsync(nodes[0]); }
        }
    }

    // "Open with the default program": first extract this one file to a temporary folder (keeping Zone.Identifier), when done hand it to the shell to open
    fire_and_forget MainWindow::OpenEntryFileAsync(uint32_t node)
    {
        auto strong = get_strong();
        if (!current_ || !tree_ || node >= tree_->NodeCount()) co_return;
        auto const& n = tree_->Node(node);
        if (n.isDir || n.entryIndex == lycan::kNoEntry) co_return;
        const uint32_t entryIndex = n.entryIndex;
        const std::wstring name = n.name;
        const std::wstring parent = tree_->ParentPath(node);
        const uint32_t parentNode = n.parent;        // copied: `n` must not be used after a dialog (the tree may be rebuilt)
        const fs::path archivePath = currentPath_;

        auto safe = lycan::SanitizeEntryPath(name);
        if (safe.rejected)
        {
            ShowMessage(lycan::Tr(L"main.cannot_open"), hstring{lycan::Tf(L"main.file_s_name_unsafe_was_rejected", safe.reason.Text())});
            co_return;
        }
        // Programs, installers and scripts: ask first (unless turned off), and extract their folder so that they find
        // the files they depend on. Everything else: just the one file.
        const bool exec = lycan::IsExecutableName(name);
        bool withFolder = false;
        // nothing else in its folder: there is no scope to choose (and nothing extra to extract)
        const bool alone = parentNode != lycan::kNoNode && tree_->Node(parentNode).fileCount <= 1;
        if (exec)
        {
            withFolder = settings_.execScope == lycan::OpenScope::Folder;
            if (settings_.warnExecutable)
            {
                auto dlg = MakeDialog(lycan::Tr(L"main.run_program"), L"");
                StackPanel sp;
                sp.Spacing(12);
                TextBlock tb;
                tb.Text(hstring{lycan::Tf(L"main.comes_from_archive_will_run", name)});
                tb.TextWrapping(TextWrapping::Wrap);
                tb.MaxWidth(440);
                sp.Children().Append(tb);
                RadioButtons scope;
                if (!alone) scope.Header(box_value(lycan::Tr(L"main.which_files_should_be_extracted")));
                scope.Items().Append(TitledOption(lycan::Tr(L"main.files_same_folder_recommended"), lycan::Tr(L"main.needed_when_program_uses_files")));
                scope.Items().Append(TitledOption(lycan::Tr(L"main.extract_only_file"), lycan::Tr(L"main.faster_but_program_may_not_find")));
                scope.SelectedIndex(withFolder ? 0 : 1);
                if (!alone) sp.Children().Append(scope);
                CheckBox never;
                never.Content(box_value(lycan::Tr(L"main.do_not_warn_again_can_be")));
                sp.Children().Append(never);
                dlg.Content(sp);
                dlg.PrimaryButtonText(lycan::Tr(L"main.extract_run"));
                dlg.CloseButtonText(lycan::Tr(L"main.cancel"));
                dlg.DefaultButton(ContentDialogButton::Close);
                if (co_await ShowDialogAsync(dlg) != ContentDialogResult::Primary) co_return;
                if (!alone)
                {
                    withFolder = scope.SelectedIndex() == 0;
                    settings_.execScope = withFolder ? lycan::OpenScope::Folder : lycan::OpenScope::FileOnly;
                }
                if (never.IsChecked().Value()) settings_.warnExecutable = false;
                LoadSettingsToUi();
                SaveSettings();
                if (!current_ || !tree_ || currentPath_ != archivePath || node >= tree_->NodeCount()) co_return;  // the archive changed while the question was open
            }
        }

        // The folder the program is in (with subfolders). More than 1 GB: ask before extracting that much.
        std::vector<uint32_t> entries{entryIndex};
        if (withFolder && !alone && parentNode != lycan::kNoNode)
        {
            const uint64_t folderBytes = tree_->Node(parentNode).totalSize;
            bool useFolder = true;
            if (folderBytes > kExecFolderWarnBytes)
            {
                auto dlg = MakeDialog(lycan::Tr(L"main.folder_large"), L"");
                TextBlock tb;
                tb.Text(hstring{lycan::Tf(L"main.folder_program_extract_all", FormatSize(folderBytes))});
                tb.TextWrapping(TextWrapping::Wrap);
                tb.MaxWidth(420);
                dlg.Content(tb);
                dlg.PrimaryButtonText(lycan::Tr(L"main.extract_whole_folder"));
                dlg.SecondaryButtonText(lycan::Tr(L"main.extract_only_file"));
                dlg.CloseButtonText(lycan::Tr(L"main.cancel"));
                dlg.DefaultButton(ContentDialogButton::Secondary);
                const auto r = co_await ShowDialogAsync(dlg);
                if (r == ContentDialogResult::None) co_return;
                useFolder = r == ContentDialogResult::Primary;
                if (!current_ || !tree_ || currentPath_ != archivePath || node >= tree_->NodeCount()) co_return;
            }
            if (useFolder) entries = tree_->EntryIndices({parentNode});
        }
        if (!co_await EnsurePasswordAsync()) co_return;

        static std::atomic<uint32_t> seq{0};
        std::error_code ec;
        if (lycan_app::OpenTempRoot().empty()) ec = std::make_error_code(std::errc::no_such_file_or_directory);
        fs::path dir = lycan_app::OpenTempRoot() /
                       (std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++seq));
        if (ec)
        {
            ShowMessage(lycan::Tr(L"main.cannot_open"), lycan::Tr(L"main.temporary_folder_not_found"));
            co_return;
        }

        lycan::ExtractOptions eo;
        eo.skipMacMetadata = settings_.skipMacMetadata;
        eo.keepDamaged = settings_.keepDamaged;
        eo.overwrite = lycan::OverwriteMode::Overwrite;  // a new folder, no conflict possible
        eo.stripPrefix = parent.empty() ? std::wstring() : parent + L"/";
        if (!password_.empty()) eo.password = [pw = password_](std::wstring& out) { out = pw; return true; };

        TaskMeta meta;
        meta.archive = currentPath_;
        meta.openFile = dir / safe.relative;
        std::lock_guard lk(metaM_);
        uint64_t id = queue_.EnqueueExtract(currentPath_, dir, eo, MakeOpenOptions(), entries);
        taskMeta_[id] = std::move(meta);
    }

    // ---------------------------------------------------------------- Columns of the details view

    // The scroll viewer inside a list control (the list keeps its own vertical scroll bar; the horizontal one is switched on in XAML)
    static ScrollViewer FindScrollViewer(DependencyObject const& root)
    {
        const int n = Microsoft::UI::Xaml::Media::VisualTreeHelper::GetChildrenCount(root);
        for (int i = 0; i < n; ++i)
        {
            auto c = Microsoft::UI::Xaml::Media::VisualTreeHelper::GetChild(root, i);
            if (auto sv = c.try_as<ScrollViewer>()) return sv;
            if (auto r = FindScrollViewer(c)) return r;
        }
        return nullptr;
    }

    // The names of the columns in settings.ini (list_col_widths=name=320,type=70)
    static const char* const kColKeys[] = {"name", "path", "type", "size", "packed", "ratio", "modified", "encrypted", "method", "crc"};

    void MainWindow::LoadColWidths()
    {
        for (int c = 0; c < LcCount; ++c) colW_[c] = kLcDefault[c];
        const std::string& text = settings_.listColWidths;
        size_t pos = 0;
        while (pos < text.size())
        {
            size_t end = text.find(',', pos);
            if (end == std::string::npos) end = text.size();
            const std::string item = text.substr(pos, end - pos);
            pos = end + 1;
            const size_t eq = item.find('=');
            if (eq == std::string::npos) continue;
            for (int c = 0; c < LcCount; ++c)
                if (item.compare(0, eq, kColKeys[c]) == 0)
                {
                    try { colW_[c] = std::clamp(static_cast<double>(std::stoi(item.substr(eq + 1))), kLcMin[c], kLcMax); } catch (...) {}
                }
        }
    }

    // What the user dragged: only the columns that are not at their default width
    std::string MainWindow::ColWidthsText() const
    {
        std::string out;
        for (int c = 0; c < LcCount; ++c)
            if (std::abs(colW_[c] - kLcDefault[c]) >= 1.0)
                out += (out.empty() ? "" : ",") + std::string(kColKeys[c]) + "=" + std::to_string(static_cast<int>(colW_[c] + 0.5));
        return out;
    }

    // Which columns are shown: the ticked ones, and the folder column in "list everything" and in the search results. No column is hidden because the
    // window is narrow any more: the list scrolls sideways (the header with it).
    MainWindow::ListCols MainWindow::ComputeListCols()
    {
        ListCols c;
        c.path = flat_ || !query_.empty();
        c.type = (settings_.listColumns & 1) != 0;
        c.size = (settings_.listColumns & 2) != 0;
        c.packed = (settings_.listColumns & 4) != 0;
        c.modified = (settings_.listColumns & 8) != 0;
        c.ratio = (settings_.listColumns & 16) != 0;
        c.encrypted = (settings_.listColumns & 32) != 0;
        c.crc = (settings_.listColumns & 64) != 0;
        c.method = (settings_.listColumns & 128) != 0;
        return c;
    }

    void MainWindow::ApplyRowColumns(FrameworkElement const& root, LycanZip::EntryRow const&)
    {
        auto grid = root.try_as<Grid>();
        if (!grid || grid.ColumnDefinitions().Size() != static_cast<uint32_t>(LcCount + 1)) return;
        auto cols = grid.ColumnDefinitions();
        grid.Width(rowW_ > 0 ? rowW_ : std::numeric_limits<double>::quiet_NaN());   // the width of all columns; the list scrolls when it is wider than the window
        for (int c = 0; c < LcCount; ++c) cols.GetAt(static_cast<uint32_t>(c + 1)).Width(GridLength{curW_[c], GridUnitType::Pixel});
        const auto& v = colVis_;
        auto show = [&](const wchar_t* name, bool on) {
            if (auto el = root.FindName(name).try_as<UIElement>()) el.Visibility(on ? Visibility::Visible : Visibility::Collapsed);
        };
        show(L"RowFolder", v.path);
        show(L"RowType", v.type);
        show(L"RowSize", v.size);
        show(L"RowPacked", v.packed);
        show(L"RowRatio", v.ratio);
        show(L"RowModified", v.modified);
        show(L"RowEnc", v.encrypted);
        show(L"RowMethod", v.method);
        show(L"RowCrc", v.crc);
    }

    // The header follows the list when it scrolls sideways
    void MainWindow::HookListScroll()
    {
        if (listScroll_) return;
        auto sv = FindScrollViewer(EntryList());
        if (!sv) return;
        listScroll_ = sv;
        sv.ViewChanged([weak = get_weak()](auto&&, auto&&) { if (auto s = weak.get()) s->HeaderShift().X(-s->listScroll_.HorizontalOffset()); });
        HeaderShift().X(-sv.HorizontalOffset());
    }

    void MainWindow::UpdateListColumns()
    {
        const ListCols v = ComputeListCols();
        const ListCols before = colVis_;
        const bool first = !colsInit_;
        colVis_ = v;
        colsInit_ = true;
        HookListScroll();

        // The widths (DIP): what the user chose, or the defaults. The name takes what is left when everything fits; otherwise the row is as wide as all columns.
        const bool on[LcCount] = {true, v.path, v.type, v.size, v.packed, v.ratio, v.modified, v.encrypted, v.method, v.crc};
        double w[LcCount], sum = 0;
        for (int c = 0; c < LcCount; ++c) { w[c] = on[c] ? colW_[c] : 0.0; sum += w[c]; }
        const double viewport = EntryList().ActualWidth();
        const double fixedPart = 24 + 16;   // the check box column, and the padding left and right of the row (the header has the same)
        double content = fixedPart + sum;
        if (viewport > 0 && content < viewport)
        {
            w[LcName] += viewport - content;
            content = viewport;
        }
        bool changed = first || !(before == v) || std::abs(rowW_ - (content - 16)) >= 0.5;
        for (int c = 0; c < LcCount && !changed; ++c) changed = std::abs(w[c] - curW_[c]) >= 0.5;
        for (int c = 0; c < LcCount; ++c) curW_[c] = w[c];
        rowW_ = content - 16;

        // Column headers
        auto px = [](double x) { return GridLength{x, GridUnitType::Pixel}; };
        auto vis = [](bool b) { return b ? Visibility::Visible : Visibility::Collapsed; };
        ListHeader().Width(content);
        HeaderNameCol().Width(px(curW_[LcName]));
        HeaderPathCol().Width(px(curW_[LcPath]));
        HeaderTypeCol().Width(px(curW_[LcType]));
        HeaderSizeCol().Width(px(curW_[LcSize]));
        HeaderPackedCol().Width(px(curW_[LcPacked]));
        HeaderRatioCol().Width(px(curW_[LcRatio]));
        HeaderModifiedCol().Width(px(curW_[LcModified]));
        HeaderEncCol().Width(px(curW_[LcEncrypted]));
        HeaderMethodCol().Width(px(curW_[LcMethod]));
        HeaderCrcCol().Width(px(curW_[LcCrc]));
        SortPathButton().Visibility(vis(v.path));
        SortTypeButton().Visibility(vis(v.type));
        SortSizeButton().Visibility(vis(v.size));
        SortPackedButton().Visibility(vis(v.packed));
        SortModifiedButton().Visibility(vis(v.modified));
        SortRatioButton().Visibility(vis(v.ratio));
        SortEncButton().Visibility(vis(v.encrypted));
        SortMethodButton().Visibility(vis(v.method));
        SortCrcButton().Visibility(vis(v.crc));
        for (int c = 0; c < LcCount; ++c)
            if (colGrip_[c]) colGrip_[c].Visibility(vis(on[c]));

        if (!changed || viewMode_ != lycan::ViewMode::Details) return;
        // Rows that exist already (rows that were not virtualized in are handled in ContainerContentChanging)
        if (auto panel = EntryList().ItemsPanelRoot())
            for (auto const& child : panel.Children())
                if (auto item = child.try_as<Primitives::SelectorItem>())
                    if (auto root = item.ContentTemplateRoot().try_as<FrameworkElement>())
                        ApplyRowColumns(root, item.Content().try_as<LycanZip::EntryRow>());
    }

    // The header: a grip at the right edge of every column to drag its width (double click = the default width), and the clip that keeps
    // the scrolled header inside the list
    void MainWindow::InitListHeader()
    {
        LoadColWidths();
        for (int c = 0; c < LcCount; ++c) curW_[c] = kLcDefault[c];
        for (int c = 0; c < LcCount; ++c)
        {
            Border grip;
            grip.Width(10);
            grip.HorizontalAlignment(HorizontalAlignment::Right);
            grip.Margin(ThicknessHelper::FromLengths(0, -2, -5, -2));
            grip.Background(Microsoft::UI::Xaml::Media::SolidColorBrush{Windows::UI::Color{0, 0, 0, 0}});
            Microsoft::UI::Xaml::Shapes::Rectangle line;
            line.Width(2);
            line.RadiusX(1);
            line.RadiusY(1);
            line.HorizontalAlignment(HorizontalAlignment::Center);
            line.Margin(ThicknessHelper::FromLengths(0, 4, 0, 4));
            line.Opacity(0);
            line.Fill(Application::Current().Resources().Lookup(box_value(L"AccentFillColorDefaultBrush")).as<Microsoft::UI::Xaml::Media::Brush>());
            grip.Child(line);
            Automation::AutomationProperties::SetName(grip, lycan::Tr(L"main.resize_column"));
            Grid::SetColumn(grip, static_cast<int>(c) + 1);
            auto weak = get_weak();
            grip.PointerEntered([weak, line](auto&&, auto&&) { if (auto s = weak.get()) if (s->colDrag_ < 0) line.Opacity(0.6); });
            grip.PointerExited([weak, line, c](auto&&, auto&&) { if (auto s = weak.get()) if (s->colDrag_ != c) line.Opacity(0); });
            grip.PointerPressed([weak, line, c](Windows::Foundation::IInspectable const& sender, Input::PointerRoutedEventArgs const& e) {
                auto s = weak.get();
                if (!s) return;
                s->colDrag_ = c;
                s->colDragX_ = e.GetCurrentPoint(nullptr).Position().X;
                s->colDragW_ = s->curW_[c];
                sender.as<UIElement>().CapturePointer(e.Pointer());
                line.Opacity(1.0);
                e.Handled(true);
            });
            grip.PointerMoved([weak, c](auto&&, Input::PointerRoutedEventArgs const& e) {
                auto s = weak.get();
                if (!s || s->colDrag_ != c) return;
                const double dx = e.GetCurrentPoint(nullptr).Position().X - s->colDragX_;
                s->colW_[c] = std::clamp(s->colDragW_ + dx, kLcMin[c], kLcMax);
                s->UpdateListColumns();
                e.Handled(true);
            });
            auto finish = [weak, line, c](Windows::Foundation::IInspectable const& sender, Input::PointerRoutedEventArgs const& e) {
                auto s = weak.get();
                if (!s || s->colDrag_ != c) return;
                s->colDrag_ = -1;
                sender.as<UIElement>().ReleasePointerCaptures();
                line.Opacity(0);
                s->settings_.listColWidths = s->ColWidthsText();
                s->SaveSettings();
                e.Handled(true);
            };
            grip.PointerReleased(finish);
            grip.PointerCanceled(finish);
            grip.DoubleTapped([weak, c](auto&&, Input::DoubleTappedRoutedEventArgs const& e) {
                auto s = weak.get();
                if (!s) return;
                s->colW_[c] = kLcDefault[c];
                s->settings_.listColWidths = s->ColWidthsText();
                s->SaveSettings();
                s->UpdateListColumns();
                e.Handled(true);
            });
            ListHeader().Children().Append(grip);
            colGrip_[c] = grip;
        }
        HeaderViewport().SizeChanged([weak = get_weak()](auto&&, SizeChangedEventArgs const& e) {
            auto s = weak.get();
            if (!s) return;
            Microsoft::UI::Xaml::Media::RectangleGeometry clip;
            clip.Rect(Windows::Foundation::Rect{0, 0, e.NewSize().Width, e.NewSize().Height});
            s->HeaderViewport().Clip(clip);
        });
    }

    void MainWindow::ResetColWidths()
    {
        for (int c = 0; c < LcCount; ++c) colW_[c] = kLcDefault[c];
        settings_.listColWidths.clear();
        SaveSettings();
        UpdateListColumns();
    }

    // ---------------------------------------------------------------- Band selection with the mouse

    void MainWindow::OnBandPressed(Windows::Foundation::IInspectable const& sender, Input::PointerRoutedEventArgs const& e)
    {
        auto list = sender.try_as<ListViewBase>();
        if (!list || bandActive_ || !tree_) return;
        auto pt = e.GetCurrentPoint(list);
        if (pt.PointerDeviceType() != Microsoft::UI::Input::PointerDeviceType::Mouse || !pt.Properties().IsLeftButtonPressed()) return;

        // Pressing on a scroll bar, checkbox, icon or text is handled as usual (select / drag out / scroll).
        // Pressing on an empty part of an item (for example right of the name): note it first, and the band selection starts after a small move; a simple click still selects that row.
        bool onItem = false, onContent = false, onSelectedItem = false;
        for (auto const& hit : Microsoft::UI::Xaml::Media::VisualTreeHelper::FindElementsInHostCoordinates(e.GetCurrentPoint(nullptr).Position(), list))
        {
            if (hit.try_as<Primitives::ScrollBar>() || hit.try_as<Primitives::ButtonBase>() || hit.try_as<CheckBox>()) return;
            if (auto si = hit.try_as<Primitives::SelectorItem>()) { onItem = true; onSelectedItem = si.IsSelected(); }
            else if (hit.try_as<TextBlock>() || hit.try_as<FontIcon>() || hit.try_as<Image>()) onContent = true;
        }
        if (onItem && onContent) return;
        if (onItem && onSelectedItem) return;  // grabbing a selected row anywhere drags the selection (the band starts on unselected rows)
        if (onItem)
        {
            bandPending_ = true;
            bandPressPos_ = pt.Position();
            bandList_ = list;
            return;
        }

        bandScroll_ = FindScrollViewer(list);
        if (!bandScroll_) return;
        bandList_ = list;
        const auto mods = e.KeyModifiers();
        const bool additive = (static_cast<uint32_t>(mods) & (static_cast<uint32_t>(Windows::System::VirtualKeyModifiers::Control) |
                                                              static_cast<uint32_t>(Windows::System::VirtualKeyModifiers::Shift))) != 0;
        bandBase_.clear();
        bandHit_.clear();
        if (additive)
        {
            for (auto const& it : list.SelectedItems())
            {
                uint32_t idx;
                if (list.Items().IndexOf(it, idx)) bandBase_.insert(static_cast<int32_t>(idx));
            }
        }
        else list.SelectedItems().Clear();  // a click on an empty part = clear the selection (as in Explorer)

        const auto p = pt.Position();
        bandStart_ = {static_cast<float>(p.X + bandScroll_.HorizontalOffset()), static_cast<float>(p.Y + bandScroll_.VerticalOffset())};
        bandLast_ = p;
        bandActive_ = true;
        bandVisible_ = false;
        list.CapturePointer(e.Pointer());
        e.Handled(true);

        if (!bandTimer_)
        {
            bandTimer_ = dispatcher_.CreateTimer();
            bandTimer_.Interval(30ms);
            bandTimer_.Tick([weak = get_weak()](auto&&, auto&&) { if (auto s = weak.get()) if (s->bandActive_) s->UpdateBand(s->bandLast_); });
        }
        bandTimer_.Start();
    }

    void MainWindow::OnBandMoved(Windows::Foundation::IInspectable const& sender, Input::PointerRoutedEventArgs const& e)
    {
        if (bandPending_ && !bandActive_)
        {
            auto list = sender.try_as<ListViewBase>();
            auto pt = e.GetCurrentPoint(list);
            if (!list || list != bandList_ || !pt.Properties().IsLeftButtonPressed()) { bandPending_ = false; return; }
            const auto p = pt.Position();
            if (std::abs(p.X - bandPressPos_.X) < 3 && std::abs(p.Y - bandPressPos_.Y) < 3) return;
            bandPending_ = false;
            bandScroll_ = FindScrollViewer(list);
            if (!bandScroll_) return;
            const auto mods = e.KeyModifiers();
            const bool additive = (static_cast<uint32_t>(mods) & (static_cast<uint32_t>(Windows::System::VirtualKeyModifiers::Control) |
                                                                  static_cast<uint32_t>(Windows::System::VirtualKeyModifiers::Shift))) != 0;
            bandBase_.clear();
            bandHit_.clear();
            if (additive)
            {
                for (auto const& it : list.SelectedItems())
                {
                    uint32_t idx;
                    if (list.Items().IndexOf(it, idx)) bandBase_.insert(static_cast<int32_t>(idx));
                }
            }
            else list.SelectedItems().Clear();
            bandStart_ = {static_cast<float>(bandPressPos_.X + bandScroll_.HorizontalOffset()), static_cast<float>(bandPressPos_.Y + bandScroll_.VerticalOffset())};
            bandLast_ = p;
            bandActive_ = true;
            bandVisible_ = false;
            list.CapturePointer(e.Pointer());
            if (!bandTimer_)
            {
                bandTimer_ = dispatcher_.CreateTimer();
                bandTimer_.Interval(30ms);
                bandTimer_.Tick([weak = get_weak()](auto&&, auto&&) { if (auto s = weak.get()) if (s->bandActive_) s->UpdateBand(s->bandLast_); });
            }
            bandTimer_.Start();
        }
        if (!bandActive_) return;
        auto list = sender.try_as<ListViewBase>();
        if (!list || list != bandList_) return;
        bandLast_ = e.GetCurrentPoint(list).Position();
        UpdateBand(bandLast_);
    }

    // The item under the pointer starts its own drag recognizer a few pixels after the press and takes the pointer capture from
    // the band. While the button is still down that is not the end of the band: take the capture back (the item drag itself
    // is cancelled by DragItemsStarting while the band runs).
    void MainWindow::OnBandCaptureLost(Windows::Foundation::IInspectable const& sender, Input::PointerRoutedEventArgs const& e)
    {
        auto list = sender.try_as<ListViewBase>();
        if (bandActive_ && !bandRecapturing_ && list && list == bandList_ && e.GetCurrentPoint(list).Properties().IsLeftButtonPressed())
        {
            bandRecapturing_ = true;
            list.CapturePointer(e.Pointer());
            bandRecapturing_ = false;
            return;
        }
        OnBandReleased(sender, e);
    }

    void MainWindow::OnBandReleased(Windows::Foundation::IInspectable const& sender, Input::PointerRoutedEventArgs const& e)
    {
        bandPending_ = false;
        if (!bandActive_) return;
        if (bandList_) bandList_.ReleasePointerCaptures();
        EndBand();
    }

    void MainWindow::EndBand()
    {
        bandActive_ = false;
        bandVisible_ = false;
        if (bandTimer_) bandTimer_.Stop();
        BandLayer().Visibility(Visibility::Collapsed);
        bandHit_.clear();
        bandBase_.clear();
    }

    // Updates the band selection by the pointer position: near the edge it scrolls automatically; items inside the band (the ones that are realized) are selected, outside ones are unselected
    void MainWindow::UpdateBand(Windows::Foundation::Point pos)
    {
        auto list = bandList_;
        if (!list || !bandScroll_) return;
        const double w = list.ActualWidth(), h = list.ActualHeight();

        // auto scroll
        constexpr double kEdge = 28, kStep = 22;
        double dx = 0, dy = 0;
        if (pos.Y < kEdge) dy = -kStep; else if (pos.Y > h - kEdge) dy = kStep;
        if (pos.X < kEdge) dx = -kStep; else if (pos.X > w - kEdge) dx = kStep;
        if (dx != 0 || dy != 0)
        {
            const double nx = std::clamp(bandScroll_.HorizontalOffset() + dx, 0.0, bandScroll_.ScrollableWidth());
            const double ny = std::clamp(bandScroll_.VerticalOffset() + dy, 0.0, bandScroll_.ScrollableHeight());
            bandScroll_.ChangeView(nx, ny, nullptr, true);
        }

        const double offX = bandScroll_.HorizontalOffset(), offY = bandScroll_.VerticalOffset();
        const double cx = pos.X + offX, cy = pos.Y + offY;  // current position (content coordinates)
        const double l = std::min<double>(bandStart_.X, cx), t = std::min<double>(bandStart_.Y, cy);
        const double r = std::max<double>(bandStart_.X, cx), b = std::max<double>(bandStart_.Y, cy);
        if (!bandVisible_ && (r - l) < 4 && (b - t) < 4) return;  // not really dragged yet
        bandVisible_ = true;

        // selection rectangle (view coordinates, clamped to the list area)
        const double vl = std::max(0.0, l - offX), vt = std::max(0.0, t - offY);
        const double vr = std::min(w, r - offX), vb = std::min(h, b - offY);
        BandLayer().Visibility(Visibility::Visible);
        auto accent = AccentColorNow();
        BandRect().Fill(Microsoft::UI::Xaml::Media::SolidColorBrush{Windows::UI::Color{0x38, accent.R, accent.G, accent.B}});
        BandRect().Stroke(Microsoft::UI::Xaml::Media::SolidColorBrush{Windows::UI::Color{0xC0, accent.R, accent.G, accent.B}});
        Canvas::SetLeft(BandRect(), vl);
        Canvas::SetTop(BandRect(), vt);
        BandRect().Width(std::max(0.0, vr - vl));
        BandRect().Height(std::max(0.0, vb - vt));

        auto panel = list.ItemsPanelRoot();
        if (!panel) return;
        const Windows::Foundation::Rect whole{0, 0, static_cast<float>(w), static_cast<float>(h)};
        for (auto const& child : panel.Children())
        {
            auto item = child.try_as<Primitives::SelectorItem>();
            if (!item) continue;
            const int32_t idx = list.IndexFromContainer(item);
            if (idx < 0) continue;
            auto fe = item.as<FrameworkElement>();
            auto bounds = item.TransformToVisual(list).TransformBounds(Windows::Foundation::Rect{0, 0, static_cast<float>(fe.ActualWidth()), static_cast<float>(fe.ActualHeight())});
            const double il = bounds.X + offX, it = bounds.Y + offY, ir = il + bounds.Width, ib = it + bounds.Height;
            const bool hit = il < r && ir > l && it < b && ib > t;
            if (hit) bandHit_.insert(idx); else bandHit_.erase(idx);
            const bool want = hit || bandBase_.count(idx) || bandHit_.count(idx);
            if (item.IsSelected() != want)
            {
                Microsoft::UI::Xaml::Data::ItemIndexRange range{idx, 1};
                if (want) list.SelectRange(range); else list.DeselectRange(range);
            }
        }
        (void)whole;
    }

    // ---------------------------------------------------------------- Keyboard shortcuts

    bool MainWindow::FocusInTextInput()
    {
        auto f = Input::FocusManager::GetFocusedElement(RootGrid().XamlRoot());
        return f && (f.try_as<TextBox>() || f.try_as<PasswordBox>() || f.try_as<RichEditBox>());
    }

    void MainWindow::ChooseViewMode(int n)
    {
        if (!tree_) return;
        keepOverride_ = SelectedNodes();
        viewMode_ = static_cast<lycan::ViewMode>(std::clamp(n, 0, 2));
        RefreshList(true);
    }

    void MainWindow::InitShortcuts()
    {
        using Windows::System::VirtualKey;
        using Windows::System::VirtualKeyModifiers;
        const auto N = VirtualKeyModifiers::None;
        const auto C = VirtualKeyModifiers::Control;
        const auto CS = static_cast<VirtualKeyModifiers>(static_cast<uint32_t>(VirtualKeyModifiers::Control) | static_cast<uint32_t>(VirtualKeyModifiers::Shift));
        const auto A = VirtualKeyModifiers::Menu;
        const auto AS = static_cast<VirtualKeyModifiers>(static_cast<uint32_t>(VirtualKeyModifiers::Menu) | static_cast<uint32_t>(VirtualKeyModifiers::Shift));

        struct Def
        {
            const wchar_t* group;
            const wchar_t* label;
            const wchar_t* keys;
            VirtualKey key;                  // None = only shown in the table (handled elsewhere)
            VirtualKeyModifiers mods;
            std::function<bool(MainWindow&)> act;  // returns true = handled (otherwise the key is passed on to the control as usual)
        };
        auto inArchive = [](MainWindow& w) { return w.view_ == View::Archive && w.current_ && w.tree_ && !w.dialogOpen_; };
        auto hasSel = [](MainWindow& w) { return !w.SelectedNodes().empty(); };

        std::vector<Def> defs;
        defs.push_back({lycan::Tr(L"main.global"), lycan::Tr(L"main.open_archive_on_new_archive_page"), L"Ctrl + O", VirtualKey::O, C, [](MainWindow& w) {
            if (w.dialogOpen_) return false;
            if (w.view_ == View::Compress) w.AddSourcesFromPicker(false); else w.OnOpenArchiveClick(nullptr, nullptr);
            return true; }});
        defs.push_back({lycan::Tr(L"main.global"), lycan::Tr(L"main.new_archive_choose_files"), L"Ctrl + N", VirtualKey::N, C, [](MainWindow& w) { if (w.dialogOpen_) return false; w.OnNewFromFilesClick(nullptr, nullptr); return true; }});
        defs.push_back({lycan::Tr(L"main.global"), lycan::Tr(L"main.new_archive_choose_folder"), L"Ctrl + Shift + N", VirtualKey::N, CS, [](MainWindow& w) { if (w.dialogOpen_) return false; w.OnNewFromFolderClick(nullptr, nullptr); return true; }});
        defs.push_back({lycan::Tr(L"main.global"), lycan::Tr(L"main.open_close_settings"), L"Ctrl + ,", static_cast<VirtualKey>(188), C, [](MainWindow& w) { if (w.dialogOpen_) return false; w.OnSettingsClick(nullptr, nullptr); return true; }});
        defs.push_back({lycan::Tr(L"main.global"), lycan::Tr(L"main.search_settings_on_page"), L"Ctrl + F", VirtualKey::None, N, nullptr});
        defs.push_back({lycan::Tr(L"main.global"), lycan::Tr(L"main.go_back_close_current_page"), L"Esc", VirtualKey::Escape, N, [](MainWindow& w) {
            if (w.dialogOpen_) return false;
            if (w.view_ == View::Settings)
            {
                w.OnSettingsBackClick(nullptr, nullptr);   // (Esc in the settings search: OnSettingsSearchKeyDown)
                return true;
            }
            if (w.view_ == View::Compress) { w.LeaveCompressPage(); return true; }
            if (w.view_ == View::Archive && !w.FocusInTextInput() && !w.SelectedNodes().empty()) { w.ActiveList().SelectedItems().Clear(); return true; }
            return false; }});
        defs.push_back({lycan::Tr(L"main.global"), lycan::Tr(L"main.show_collapse_task_panel"), L"Ctrl + J", VirtualKey::J, C, [](MainWindow& w) {
            if (w.TaskPanel().Visibility() != Visibility::Visible) return false;
            w.OnTaskCollapseClick(nullptr, nullptr); return true; }});
        defs.push_back({lycan::Tr(L"main.global"), lycan::Tr(L"main.clear_finished_tasks"), L"Ctrl + Shift + Delete", VirtualKey::Delete, CS, [](MainWindow& w) {
            if (w.TaskPanel().Visibility() != Visibility::Visible) return false;
            w.OnClearFinishedClick(nullptr, nullptr); return true; }});

        defs.push_back({lycan::Tr(L"main.global"), lycan::Tr(L"main.paste_files_folders_new_archive"), L"Ctrl + V", VirtualKey::V, C, [](MainWindow& w) { return w.TryPasteFiles(); }});
        defs.push_back({lycan::Tr(L"main.archive_view"), lycan::Tr(L"main.search"), L"Ctrl + F", VirtualKey::None, N, nullptr});
        defs.push_back({lycan::Tr(L"main.archive_view"), lycan::Tr(L"main.select_all"), L"Ctrl + A", VirtualKey::A, C, [inArchive](MainWindow& w) {
            if (!inArchive(w) || w.FocusInTextInput()) return false;
            w.ActiveList().SelectAll(); return true; }});
        defs.push_back({lycan::Tr(L"main.archive_view"), lycan::Tr(L"main.open_selected_item_file_enter"), L"Enter", VirtualKey::None, N, nullptr});
        defs.push_back({lycan::Tr(L"main.archive_view"), lycan::Tr(L"main.rename_selected_item_zip_7z_tar"), L"F2", VirtualKey::None, N, nullptr});
        defs.push_back({lycan::Tr(L"main.archive_view"), lycan::Tr(L"main.delete_selected_items_from_archive"), L"Delete", VirtualKey::None, N, nullptr});
        defs.push_back({lycan::Tr(L"main.archive_view"), lycan::Tr(L"main.up_one_level"), L"Backspace  /  Alt + ↑", VirtualKey::Up, A, [inArchive](MainWindow& w) { if (!inArchive(w)) return false; w.OnUpClick(nullptr, nullptr); return true; }});
        defs.push_back({lycan::Tr(L"main.archive_view"), lycan::Tr(L"main.extract_here_only_selected_items"), L"Ctrl + E", VirtualKey::E, C, [inArchive, hasSel](MainWindow& w) {
            if (!inArchive(w)) return false;
            if (hasSel(w)) w.OnExtractSelHereClick(nullptr, nullptr); else w.OnExtractHereClick(nullptr, nullptr);
            return true; }});
        defs.push_back({lycan::Tr(L"main.archive_view"), lycan::Tr(L"main.extract_only_selection_when"), L"Ctrl + Shift + E", VirtualKey::E, CS, [inArchive, hasSel](MainWindow& w) {
            if (!inArchive(w)) return false;
            if (hasSel(w)) w.OnExtractSelToClick(nullptr, nullptr); else w.OnExtractToClick(nullptr, nullptr);
            return true; }});
        defs.push_back({lycan::Tr(L"main.archive_view"), lycan::Tr(L"main.test_integrity"), L"Ctrl + T", VirtualKey::T, C, [inArchive](MainWindow& w) { if (!inArchive(w)) return false; w.OnTestClick(nullptr, nullptr); return true; }});
        defs.push_back({lycan::Tr(L"main.archive_view"), lycan::Tr(L"main.copy_path"), L"Ctrl + Shift + C", VirtualKey::C, CS, [inArchive, hasSel](MainWindow& w) {
            if (!inArchive(w) || w.FocusInTextInput() || !hasSel(w)) return false;
            w.CopyPaths(w.SelectedNodes()); return true; }});
        defs.push_back({lycan::Tr(L"main.archive_view"), lycan::Tr(L"main.image_preview_selected_image_first"), L"Alt + P", VirtualKey::P, A, [inArchive](MainWindow& w) {
            if (!inArchive(w)) return false;
            w.PreviewImages(); return true; }});
        defs.push_back({lycan::Tr(L"main.archive_view"), lycan::Tr(L"main.audio_preview_selected_audio_file"), L"Alt + Shift + P", VirtualKey::P, AS, [inArchive](MainWindow& w) {
            if (!inArchive(w)) return false;
            w.PreviewAudio(); return true; }});
        defs.push_back({lycan::Tr(L"main.archive_view"), lycan::Tr(L"main.show_hide_preview_pane"), L"Ctrl + Shift + P", VirtualKey::P, CS, [inArchive](MainWindow& w) {
            if (!inArchive(w)) return false;
            w.PaneToggle().IsChecked(!w.settings_.previewPane); w.OnPaneToggleClick(nullptr, nullptr); return true; }});
        defs.push_back({lycan::Tr(L"main.archive_view"), lycan::Tr(L"main.properties_with_nothing_selected"), L"Alt + Enter", VirtualKey::Enter, A, [inArchive](MainWindow& w) {
            if (!inArchive(w)) return false;
            auto nodes = w.SelectedNodes();
            if (nodes.empty()) { w.ShowArchiveInfoAsync(); return true; }  // nothing selected: the archive itself
            if (nodes.size() != 1) return false;
            w.ShowProperties(nodes); return true; }});
        defs.push_back({lycan::Tr(L"main.archive_view"), lycan::Tr(L"main.view_details"), L"Ctrl + 1", VirtualKey::Number1, C, [inArchive](MainWindow& w) { if (!inArchive(w)) return false; w.ChooseViewMode(0); return true; }});
        defs.push_back({lycan::Tr(L"main.archive_view"), lycan::Tr(L"main.view_list"), L"Ctrl + 2", VirtualKey::Number2, C, [inArchive](MainWindow& w) { if (!inArchive(w)) return false; w.ChooseViewMode(1); return true; }});
        defs.push_back({lycan::Tr(L"main.archive_view"), lycan::Tr(L"main.view_icons"), L"Ctrl + 3", VirtualKey::Number3, C, [inArchive](MainWindow& w) { if (!inArchive(w)) return false; w.ChooseViewMode(2); return true; }});
        defs.push_back({lycan::Tr(L"main.archive_view"), lycan::Tr(L"main.list_all_flatten_all_levels"), L"Ctrl + Shift + F", VirtualKey::F, CS, [inArchive](MainWindow& w) {
            if (!inArchive(w)) return false;
            w.FlatToggle().IsChecked(!w.flat_); w.OnFlatToggleClick(nullptr, nullptr); return true; }});
        defs.push_back({lycan::Tr(L"main.archive_view"), lycan::Tr(L"main.show_hide_folder_tree"), L"Ctrl + B", VirtualKey::B, C, [inArchive](MainWindow& w) {
            if (!inArchive(w)) return false;
            w.TreeToggle().IsChecked(!w.treeWanted_); w.OnTreeToggleClick(nullptr, nullptr); return true; }});
        defs.push_back({lycan::Tr(L"main.archive_view"), lycan::Tr(L"main.close_archive_return_home_page"), L"Ctrl + W", VirtualKey::W, C, [inArchive](MainWindow& w) { if (!inArchive(w)) return false; w.OnBackClick(nullptr, nullptr); return true; }});

        defs.push_back({lycan::Tr(L"main.new_archive_page"), lycan::Tr(L"main.start_compressing"), L"Ctrl + Enter", VirtualKey::Enter, C, [](MainWindow& w) {
            if (w.view_ != View::Compress || w.dialogOpen_) return false;
            w.OnCompressStart(); return true; }});
        defs.push_back({lycan::Tr(L"main.new_archive_page"), lycan::Tr(L"main.add_folder_button"), L"Ctrl + Shift + O", VirtualKey::O, CS, [](MainWindow& w) {
            if (w.view_ != View::Compress || w.dialogOpen_) return false;
            w.AddSourcesFromPicker(true); return true; }});
        defs.push_back({lycan::Tr(L"main.new_archive_page"), lycan::Tr(L"main.save_as_new_template"), L"Ctrl + S", VirtualKey::S, C, [](MainWindow& w) {
            if (w.view_ != View::Compress || w.dialogOpen_) return false;
            w.OnPresetSaveNew(); return true; }});
        defs.push_back({lycan::Tr(L"main.image_preview_window"), lycan::Tr(L"main.previous_next"), lycan::Tr(L"main.space_pageup_pagedown_mouse_wheel"), VirtualKey::None, N, nullptr});
        defs.push_back({lycan::Tr(L"main.image_preview_window"), lycan::Tr(L"main.first_last"), L"Home / End", VirtualKey::None, N, nullptr});
        defs.push_back({lycan::Tr(L"main.image_preview_window"), lycan::Tr(L"main.reading_mode_keys"), L"B  /  D  /  S  /  O", VirtualKey::None, N, nullptr});
        defs.push_back({lycan::Tr(L"main.image_preview_window"), lycan::Tr(L"main.view_mode_fit_window_actual_size"), L"Ctrl + 0  /  Ctrl + 1  /  Ctrl + 2  /  Ctrl + 3  F", VirtualKey::None, N, nullptr});
        defs.push_back({lycan::Tr(L"main.image_preview_window"), lycan::Tr(L"main.zoom_out_when_zoomed_drag_use"), lycan::Tr(L"main.ctrl_wheel"), VirtualKey::None, N, nullptr});
        defs.push_back({lycan::Tr(L"main.image_preview_window"), lycan::Tr(L"main.rotate_changes_display_only"), L"R / Shift + R", VirtualKey::None, N, nullptr});
        defs.push_back({lycan::Tr(L"main.image_preview_window"), lycan::Tr(L"main.full_screen_thumbnail_strip"), L"F11  /  T  Ctrl + B", VirtualKey::None, N, nullptr});
        defs.push_back({lycan::Tr(L"main.image_preview_window"), lycan::Tr(L"main.copy_image_copy_path"), L"Ctrl + C  /  Ctrl + Shift + C", VirtualKey::None, N, nullptr});
        defs.push_back({lycan::Tr(L"main.image_preview_window"), lycan::Tr(L"main.save_image_as_open_with_associated"), L"Ctrl + S  Ctrl + E  /  Enter", VirtualKey::None, N, nullptr});
        defs.push_back({lycan::Tr(L"main.image_preview_window"), lycan::Tr(L"main.animations_gif_animated_webp_play"), L"P", VirtualKey::None, N, nullptr});
        defs.push_back({lycan::Tr(L"main.image_preview_window"), lycan::Tr(L"main.image_info_size_format_photo_info"), L"I  /  Alt + Enter", VirtualKey::None, N, nullptr});
        defs.push_back({lycan::Tr(L"main.image_preview_window"), lycan::Tr(L"main.menu_button_at_top_right_right"), lycan::Tr(L"main.right_click"), VirtualKey::None, N, nullptr});
        defs.push_back({lycan::Tr(L"main.image_preview_window"), lycan::Tr(L"main.close_leaves_full_screen_first"), L"Esc", VirtualKey::None, N, nullptr});
        defs.push_back({lycan::Tr(L"main.dialogs"), lycan::Tr(L"main.ok_cancel"), L"Enter  /  Esc", VirtualKey::None, N, nullptr});

        // Register the shortcuts
        auto weak = get_weak();
        for (auto const& d : defs)
        {
            if (d.key == VirtualKey::None || !d.act) continue;
            Input::KeyboardAccelerator ka;
            ka.Key(d.key);
            ka.Modifiers(d.mods);
            ka.Invoked([weak, act = d.act](Input::KeyboardAccelerator const&, Input::KeyboardAcceleratorInvokedEventArgs const& args) {
                if (auto s = weak.get()) args.Handled(act(*s));
            });
            RootGrid().KeyboardAccelerators().Append(ka);
        }

        // The table of the settings page: one card for each group, the action on the left, the keys on the right
        auto panel = ShortcutPanel();
        const wchar_t* lastGroup = nullptr;
        StackPanel rows{nullptr};
        for (auto const& d : defs)
        {
            if (!lastGroup || wcscmp(lastGroup, d.group) != 0)
            {
                lastGroup = d.group;
                panel.Children().Append(Caption(d.group, 0.75));
                rows = StackPanel{};
                rows.Spacing(10);
                Border card;
                card.Style(RootGrid().Resources().Lookup(box_value(L"SettingCard")).as<Style>());
                card.Child(rows);
                panel.Children().Append(card);
            }
            Grid row;
            row.ColumnSpacing(16);
            ColumnDefinition c0, c1;
            c0.Width(GridLength{1, GridUnitType::Star});
            c1.Width(GridLength{0, GridUnitType::Auto});
            row.ColumnDefinitions().Append(c0);
            row.ColumnDefinitions().Append(c1);
            TextBlock label;
            label.Text(d.label);
            label.TextWrapping(TextWrapping::Wrap);
            label.VerticalAlignment(VerticalAlignment::Center);
            row.Children().Append(label);
            Border cap;
            cap.CornerRadius(CornerRadiusHelper::FromUniformRadius(5));
            cap.Padding(ThicknessHelper::FromLengths(10, 3, 10, 3));
            cap.BorderThickness(ThicknessHelper::FromUniformLength(1));
            cap.BorderBrush(Microsoft::UI::Xaml::Media::SolidColorBrush{Windows::UI::Color{0x50, 0x80, 0x80, 0x80}});
            cap.Background(Microsoft::UI::Xaml::Media::SolidColorBrush{Windows::UI::Color{0x20, 0x80, 0x80, 0x80}});
            cap.VerticalAlignment(VerticalAlignment::Center);
            TextBlock keys;
            keys.Text(d.keys);
            keys.FontFamily(Microsoft::UI::Xaml::Media::FontFamily{L"Consolas, Segoe UI"});
            keys.FontSize(13);
            cap.Child(keys);
            Grid::SetColumn(cap, 1);
            row.Children().Append(cap);
            rows.Children().Append(row);
        }

        // The tooltips of buttons show the shortcut
        auto tip = [](FrameworkElement const& el, const wchar_t* text) { ToolTipService::SetToolTip(el, box_value(hstring{text})); };
        tip(OpenButton(), lycan::Tr(L"main.open_archive_ctrl_o"));
        tip(NewButton(), lycan::Tr(L"main.new_archive_ctrl_n"));
        tip(SettingsButton(), lycan::Tr(L"main.settings_ctrl"));
        tip(BackButton(), lycan::Tr(L"main.back_home_ctrl_w"));
        tip(ExtractToButton(), lycan::Tr(L"main.extract_ctrl_shift_e"));
        tip(UpButton(), lycan::Tr(L"main.up_one_level_backspace_alt"));
        tip(TaskCollapseButton(), lycan::Tr(L"main.collapse_expand_ctrl_j"));
        tip(CStartButton(), lycan::Tr(L"main.start_compressing_ctrl_enter"));
        tip(CCancelButton(), lycan::Tr(L"main.cancel_esc"));
        tip(CBackButton(), lycan::Tr(L"main.back_esc"));
        tip(SettingsBackButton(), lycan::Tr(L"main.back_esc"));
    }

    // ---- Paths dragged in / opened

    // A single archive -> open. A single .exe: try to open it as an archive; if the format is not an ordinary PE (a self-extracting archive or an installer
    // that contains 7z / zip data) ask "open the content" or "add to an archive" (asked every time); an ordinary program .exe is simply a file to compress. Anything else -> add to an archive.
    fire_and_forget MainWindow::HandleDroppedPaths(std::vector<fs::path> paths)
    {
        auto strong = get_strong();
        if (paths.size() == 1 && IsArchivePath(paths[0])) { OpenFromUi(paths[0]); co_return; }
        // an archive that can be changed is on screen: what is dropped goes into it (into the folder that is shown)
        if (view_ == View::Archive && current_ && tree_ && !compact_ && current_->Caps().canModify)
        {
            AddPathsToArchive(std::move(paths), CurrentFolderPath());
            co_return;
        }
        // checksum files: check them (against the archive that is on screen, when there is one)
        if (!paths.empty() && std::all_of(paths.begin(), paths.end(), [](fs::path const& p) { return lycan::IsSumFile(p); }))
        {
            if (view_ == View::Archive && current_ && !compact_) ShowVerifyDialogAsync(std::move(paths), currentPath_);
            else QueueVerify(std::move(paths), false);
            co_return;
        }
        if (paths.size() == 1 && IsExePath(paths[0]))
        {
            const fs::path file = paths[0];
            co_await resume_background();
            std::wstring format;
            try { format = lycan::ArchiveSource::Open(file)->FormatName(); } catch (...) {}
            co_await resume_ui{dispatcher_};
            if (!format.empty() && format != L"PE")
            {
                auto dlg = MakeDialog(lycan::Tr(L"main.self_extracting_archive"), L"\uE7B8");
                StackPanel sp;
                sp.Spacing(10);
                sp.MinWidth(420);
                TextBlock tb;
                tb.Text(hstring{lycan::Tf(L"main.contains_compressed_data_format", file.filename().wstring(), format)});
                tb.TextWrapping(TextWrapping::Wrap);
                sp.Children().Append(tb);
                dlg.Content(sp);
                dlg.PrimaryButtonText(lycan::Tr(L"main.open_contents"));
                dlg.SecondaryButtonText(lycan::Tr(L"main.add_to_archive_title"));
                dlg.CloseButtonText(lycan::Tr(L"main.cancel"));
                dlg.DefaultButton(ContentDialogButton::Primary);
                auto r = co_await ShowDialogAsync(dlg);
                if (r == ContentDialogResult::Primary) OpenFromUi(file);
                else if (r == ContentDialogResult::Secondary) StartCompress({file});
                co_return;
            }
        }
        StartCompress(std::move(paths));
    }

    // ---- Picture preview

    // Toolbar "Preview > Images" / Alt+P: the selected image, otherwise the first one in the current list. Nothing to
    // preview is only reported after the user asked (no detection beforehand).
    void MainWindow::PreviewImages()
    {
        if (!current_ || !tree_ || !entries_) return;
        uint32_t pick = lycan::kNoNode, fallback = lycan::kNoNode;
        auto nodes = SelectedNodes();
        if (nodes.size() == 1)
        {
            auto const& n = tree_->Node(nodes[0]);
            if (!n.isDir && n.entryIndex != lycan::kNoEntry && lycan_app::IsPreviewableImageName(n.name)) pick = nodes[0];
        }
        if (pick == lycan::kNoNode)
            for (auto const& it : entries_)
            {
                const uint32_t id = it.as<LycanZip::EntryRow>().NodeId();
                auto const& n = tree_->Node(id);
                if (n.isDir || n.entryIndex == lycan::kNoEntry || lycan::IsMacMetadataPath(n.path)) continue;
                if (lycan_app::CanPreviewImage(n.name)) { pick = id; break; }
                if (fallback == lycan::kNoNode && lycan_app::IsPreviewableImageName(n.name)) fallback = id;  // known format, but no decoder
            }
        if (pick == lycan::kNoNode) pick = fallback;  // PreviewNode explains what is missing
        if (pick == lycan::kNoNode)
        {
            ShowMessage(lycan::Tr(L"main.no_images_preview"), lycan::Tr(L"main.there_no_images_current_folder"));
            return;
        }
        PreviewNode(pick);
    }

    // Toolbar "Preview > Audio" / Alt+Shift+P: the selected audio file, otherwise the first one in the current list.
    fire_and_forget MainWindow::PreviewAudio(uint32_t pick)
    {
        auto strong = get_strong();
        if (!current_ || !tree_ || !entries_) co_return;
        lycan_app::AudioRequest req;
        uint32_t picked = lycan::kNoNode;
        auto nodes = SelectedNodes();
        if (pick != lycan::kNoNode) nodes = {pick};
        if (nodes.size() == 1)
        {
            auto const& n = tree_->Node(nodes[0]);
            if (!n.isDir && n.entryIndex != lycan::kNoEntry && lycan_app::IsPreviewableAudioName(n.name)) picked = nodes[0];
        }
        for (auto const& it : entries_)
        {
            const uint32_t id = it.as<LycanZip::EntryRow>().NodeId();
            auto const& n = tree_->Node(id);
            if (n.isDir || n.entryIndex == lycan::kNoEntry || !lycan_app::IsPreviewableAudioName(n.name) || lycan::IsMacMetadataPath(n.path)) continue;
            if (id == picked) req.start = req.items.size();
            req.items.push_back({n.entryIndex, n.name, n.path, n.size});
        }
        if (req.items.empty())
        {
            ShowMessage(lycan::Tr(L"main.no_audio_preview"), lycan::Tr(L"main.there_no_audio_files_current"));
            co_return;
        }
        if (!co_await EnsurePasswordAsync()) co_return;  // an encrypted archive needs its password first
        if (!current_ || !tree_) co_return;
        req.owner = this;
        req.archive = currentPath_;
        req.open = MakeOpenOptions();
        req.loopMode = static_cast<int>(settings_.audioLoop);
        req.onLoopMode = [weak = get_weak()](int m) { if (auto s = weak.get()) { s->settings_.audioLoop = static_cast<lycan::AudioLoop>(m); s->SaveSettings(); } };
        lycan_app::ShowAudioPreview(std::move(req));
    }

    void MainWindow::OnPreviewImagesClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) { PreviewImages(); }
    void MainWindow::OnPreviewAudioClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) { PreviewAudio(); }

    fire_and_forget MainWindow::PreviewNode(uint32_t node)
    {
        auto strong = get_strong();
        if (!current_ || !tree_ || node >= tree_->NodeCount()) co_return;
        {
            auto const& n = tree_->Node(node);
            if (n.isDir || n.entryIndex == lycan::kNoEntry || !lycan_app::IsPreviewableImageName(n.name)) co_return;
            if (!lycan_app::IsImageDecoderAvailable(n.name))
            {
                ShowStatusNote(lycan::Tf(L"main.cannot_preview_use_open_with", n.name, lycan_app::MissingDecoderHint(n.name)));
                co_return;
            }
        }
        if (!co_await EnsurePasswordAsync()) co_return;  // an encrypted archive needs its password verified first
        if (!current_ || !tree_) co_return;

        lycan_app::PreviewRequest req;
        req.owner = this;
        req.archive = currentPath_;
        req.open = MakeOpenOptions();
        req.prefetchBytes = static_cast<uint64_t>(settings_.prefetchMB) << 20;
        req.enlargeSmall = settings_.previewEnlargeSmall;
            req.animations = AnimOn();
        req.setEnlargeSmall = [weak = get_weak()](bool on) {
            if (auto s = weak.get()) {
                s->settings_.previewEnlargeSmall = on;
                s->loadingSettings_ = true;  // keep the settings page in step without re-reading every control
                s->EnlargeToggle().IsOn(on);
                s->loadingSettings_ = false;
                s->SaveSettings();
            }
        };
        req.openExternal = [weak = get_weak(), archive = currentPath_](uint32_t n) {
            if (auto s = weak.get()) if (s->current_ && s->currentPath_ == archive) s->OpenEntryFileAsync(n);  // the archive has been replaced: do not handle it
        };
        req.thumbReads = current_->Caps().randomAccess && !current_->Caps().solid;  // a solid archive is not read actively for thumbnails
        // All pictures in the current list (same folder / search result), in the order of the screen; the left and right keys page
        if (entries_)
        {
            for (auto const& it : entries_)
            {
                const uint32_t id = it.as<LycanZip::EntryRow>().NodeId();
                auto const& n = tree_->Node(id);
                if (n.isDir || n.entryIndex == lycan::kNoEntry || !lycan_app::CanPreviewImage(n.name) || lycan::IsMacMetadataPath(n.path)) continue;
                if (id == node) req.start = req.items.size();
                req.items.push_back({n.entryIndex, n.name, n.path, n.size, id, n.packedSize, n.mtimeFileTime});
            }
        }
        if (req.items.empty())
        {
            auto const& n = tree_->Node(node);
            req.items.push_back({n.entryIndex, n.name, n.path, n.size, node, n.packedSize, n.mtimeFileTime});
            req.start = 0;
        }
        // the reading mode of the preview reads the whole archive: every picture, in path order
        for (uint32_t id : tree_->Descendants(tree_->Root(), lycan::SortKey::Path, true, false))
        {
            auto const& n = tree_->Node(id);
            if (n.isDir || n.entryIndex == lycan::kNoEntry || !lycan_app::CanPreviewImage(n.name) || lycan::IsMacMetadataPath(n.path)) continue;
            req.book.push_back({n.entryIndex, n.name, n.path, n.size, id, n.packedSize, n.mtimeFileTime});
        }
        req.readingStore = ReadingStorePath();
        lycan_app::ShowImagePreview(std::move(req));
    }

    // ---- Dragging to Explorer

    // Only when the target (Explorer / desktop) asks for the files are they extracted to a temporary folder and the file list handed over (provided on demand, see DataPackage::SetDataProvider).
    // Small files have almost no delay; for large files the target waits. When cancelled / dropped where it is not accepted nothing is extracted.
    // This is the "extract to temporary first" way (a setting can choose it, and it is the fallback when streaming does not fit); when wrap is not empty everything goes under dir\wrap.
    static fire_and_forget ProvideDragItems(Windows::ApplicationModel::DataTransfer::DataProviderRequest request, fs::path archive,
                                            lycan::OpenOptions oo, lycan::ExtractOptions eo, std::vector<uint32_t> indices, fs::path dir,
                                            std::wstring wrap)
    {
        auto deferral = request.GetDeferral();
        co_await resume_background();
        try
        {
            std::error_code ec;
            fs::create_directories(dir, ec);
            auto src = lycan::ArchiveSource::Open(archive, oo);
            fs::path target = dir;
            if (!wrap.empty())
            {
                const lycan::SafePath w = lycan::SanitizeEntryPath(wrap);
                target = dir / (w.rejected ? fs::path(L"extracted") : w.relative);
            }
            auto rep = lycan::ExtractTo(*src, target, eo, indices);
            std::vector<Windows::Storage::IStorageItem> items;
            for (auto const& e : fs::directory_iterator(dir, ec))
            {
                if (e.is_directory(ec)) items.push_back(Windows::Storage::StorageFolder::GetFolderFromPathAsync(e.path().wstring()).get());
                else items.push_back(Windows::Storage::StorageFile::GetFileFromPathAsync(e.path().wstring()).get());
            }
            if (!items.empty()) request.SetData(single_threaded_vector<Windows::Storage::IStorageItem>(std::move(items)).GetView());
        }
        catch (...) {}
        deferral.Complete();
    }

    // Starts dragging `nodes` out of the archive. strip: like ExtractOptions::stripPrefix (what stays above the dragged
    // items); wrap: put everything in a folder of this name (the archive's root). Cancel = no drag; Done = the whole drag
    // already ran (virtual files); Provider = `data` carries the delayed-files provider and the caller lets WinUI go on.
    MainWindow::DragOutResult MainWindow::BeginDragOut(std::vector<uint32_t> const& nodes, std::wstring const& strip, std::wstring const& wrap,
                                                       Windows::ApplicationModel::DataTransfer::DataPackage const& data)
    {
        auto indices = nodes.empty() ? std::vector<uint32_t>{} : tree_->EntryIndices(nodes);
        if (indices.empty()) return DragOutResult::Cancel;
        if (current_->Caps().encrypted && !passwordVerified_)
        {
            EnsurePasswordAsync();  // verify the password first; after it succeeds just drag again
            return DragOutResult::Cancel;
        }

        // Where the files go when they have to be unpacked: a fresh folder in the temporary area
        static std::atomic<uint32_t> seq{0};
        if (lycan_app::OpenTempRoot().empty()) return DragOutResult::Cancel;
        fs::path dir = lycan_app::OpenTempRoot() /
                       (L"drag-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++seq));

        lycan::ExtractOptions eo;
        eo.skipMacMetadata = settings_.skipMacMetadata;
        eo.keepDamaged = settings_.keepDamaged;
        eo.overwrite = lycan::OverwriteMode::Overwrite;  // a new folder
        eo.stripPrefix = strip;
        if (!password_.empty()) eo.password = [pw = password_](std::wstring& out) { out = pw; return true; };
        auto oo = MakeOpenOptions();
        const fs::path archive = currentPath_;

        dragOutActive_ = true;  // when dragged onto our own window it must not count as files to compress
        SetDragGlow(false);

        // Virtual files (DragOut.h): nothing is unpacked beforehand, so the cursor never shows "no drop" while a big file
        // is being prepared. Only Explorer-like targets take virtual files (the other programs ask for real files), so the
        // setting can turn it off; selections that cannot be virtual (solid archives with several files, very long names)
        // always use the temporary folder.
        {
            lycan_app::VirtualDragRequest vr;
            const auto caps = current_->Caps();
            if (settings_.dragOutVirtual && !_wgetenv(L"LYCAN_NO_VIRTUAL_DRAG") &&
                lycan_app::PlanVirtualDrag(current_->Entries(), indices, strip, wrap, settings_.skipMacMetadata, caps.randomAccess && !caps.solid, vr))
            {
                vr.archive = archive;
                vr.open = oo;
                ActiveList().ReleasePointerCaptures();
                // Right here, inside the pointer event that started the drag: the OLE drag loop runs from this call
                lycan_app::RunVirtualDrag(std::move(vr));
                dragOutActive_ = false;
                return DragOutResult::Done;
            }
        }

        data.RequestedOperation(Windows::ApplicationModel::DataTransfer::DataPackageOperation::Copy);
        data.SetDataProvider(Windows::ApplicationModel::DataTransfer::StandardDataFormats::StorageItems(),
                             [archive, oo, eo, indices = std::move(indices), dir, wrap](Windows::ApplicationModel::DataTransfer::DataProviderRequest const& req) {
                                 ProvideDragItems(req, archive, oo, eo, indices, dir, wrap);
                             });
        return DragOutResult::Provider;
    }

    void MainWindow::OnEntryDragStarting(Windows::Foundation::IInspectable const&, DragItemsStartingEventArgs const& e)
    {
        if (!current_ || !tree_ || dragOutActive_ || bandPending_ || bandActive_) { e.Cancel(true); return; }
        std::vector<uint32_t> nodes;
        for (auto const& it : e.Items())
            if (auto row = it.try_as<LycanZip::EntryRow>()) nodes.push_back(row.NodeId());
        if (BeginDragOut(nodes, StripPrefixForView(), L"", e.Data()) != DragOutResult::Provider) e.Cancel(true);  // (virtual files: the drag already ran)
    }

    // The folder tree: a folder is dragged with its name on top; the root (the archive) is dragged like "extract to a
    // folder named after the archive"
    void MainWindow::OnTreeDragStarting(Windows::Foundation::IInspectable const&, TreeViewDragItemsStartingEventArgs const& e)
    {
        if (!current_ || !tree_ || dragOutActive_) { e.Cancel(true); return; }
        uint32_t node = lycan::kNoNode;
        for (auto const& it : e.Items())
        {
            if (auto tn = it.try_as<TreeViewNode>()) { node = NodeIdOf(tn); break; }
            if (auto row = it.try_as<LycanZip::EntryRow>()) { node = row.NodeId(); break; }
        }
        if (node == lycan::kNoNode || node >= tree_->NodeCount()) { e.Cancel(true); return; }
        std::wstring strip, wrap;
        if (node == tree_->Root()) wrap = lycan::ArchiveBaseName(currentPath_);
        else
        {
            const std::wstring parent = tree_->ParentPath(node);
            strip = parent.empty() ? std::wstring() : parent + L"/";
        }
        if (BeginDragOut({node}, strip, wrap, e.Data()) != DragOutResult::Provider) e.Cancel(true);
    }

    void MainWindow::OnEntryDragCompleted(ListViewBase const&, DragItemsCompletedEventArgs const&) { dragOutActive_ = false; }

    // At start remove temporary files older than one day (a file that is open is not deleted by mistake: only very old folders are cleared, and failures are skipped)
    void MainWindow::CleanupStaleTemp()
    {
        const fs::path root = lycan_app::OpenTempRoot();
        std::thread([root] {
            try
            {
                std::error_code ec;
                if (root.empty() || !fs::exists(root, ec)) return;
                auto now = fs::file_time_type::clock::now();
                for (auto const& d : fs::directory_iterator(root, ec))
                {
                    std::error_code e2;
                    auto t = fs::last_write_time(d.path(), e2);
                    if (!e2 && now - t > std::chrono::hours(24)) fs::remove_all(d.path(), e2);
                }
            }
            catch (...) {}
        }).detach();
    }

    // ---- Context menu

    void MainWindow::OnEntryContextRequested(UIElement const& sender, Input::ContextRequestedEventArgs const& args)
    {
        if (!current_ || !tree_ || !entries_) return;
        args.Handled(true);

        // Right click on an item that is not selected: select only that one first (as in Explorer)
        if (auto row = RowFromSource(args.OriginalSource()))
        {
            bool selected = false;
            for (auto const& it : ActiveList().SelectedItems())
                if (it.as<LycanZip::EntryRow>().NodeId() == row.NodeId()) { selected = true; break; }
            if (!selected)
            {
                ActiveList().SelectedItems().Clear();
                ActiveList().SelectedItems().Append(row);
            }
        }

        auto nodes = SelectedNodes();
        MenuFlyout fly;
        auto add = [&](hstring const& text, std::function<void()> fn, hstring const& accel = {}, const wchar_t* glyph = nullptr, IconTone tone = IconTone::None) {
            MenuFlyoutItem item;
            item.Text(text);
            if (!accel.empty()) item.KeyboardAcceleratorTextOverride(accel);
            if (glyph) item.Icon(MenuIcon(glyph, tone, RootGrid().ActualTheme() != ElementTheme::Light));
            item.Click([fn = std::move(fn)](auto&&, auto&&) { fn(); });
            fly.Items().Append(item);
        };
        auto sep = [&] { fly.Items().Append(MenuFlyoutSeparator{}); };

        if (!nodes.empty()) AddNodeMenuItems(fly, nodes, StripPrefixForView());
        else
        {
            // View / sort: the same options as the "View" menu of the toolbar (shared ApplyViewChoice)
            auto radio = [&](MenuFlyoutSubItem const& sub, hstring const& text, bool checked, wchar_t cat, int n, hstring const& group) {
                RadioMenuFlyoutItem it;
                it.Text(text);
                it.GroupName(group);
                it.IsChecked(checked);
                it.Click([weak = get_weak(), cat, n](auto&&, auto&&) { if (auto s = weak.get()) s->ApplyViewChoice(cat, n); });
                sub.Items().Append(it);
            };
            MenuFlyoutSubItem viewSub;
            viewSub.Text(lycan::Tr(L"main.view"));
            radio(viewSub, lycan::Tr(L"main.details"), viewMode_ == lycan::ViewMode::Details, L'v', 0, L"ctxview");
            radio(viewSub, lycan::Tr(L"main.list"), viewMode_ == lycan::ViewMode::List, L'v', 1, L"ctxview");
            const bool iconView = viewMode_ == lycan::ViewMode::Icons;
            radio(viewSub, lycan::Tr(L"main.view_small_icons"), iconView && settings_.thumbSize == 0, L'i', 0, L"ctxview");
            radio(viewSub, lycan::Tr(L"main.view_medium_icons"), iconView && settings_.thumbSize == 1, L'i', 1, L"ctxview");
            radio(viewSub, lycan::Tr(L"main.view_large_icons"), iconView && settings_.thumbSize == 2, L'i', 2, L"ctxview");
            radio(viewSub, lycan::Tr(L"main.view_huge_icons"), iconView && settings_.thumbSize == 3, L'i', 3, L"ctxview");
            viewSub.Items().Append(MenuFlyoutSeparator{});
            {
                ToggleMenuFlyoutItem th;
                th.Text(lycan::Tr(L"main.show_image_thumbnails_icon_view"));
                th.IsChecked(settings_.iconThumbnails);
                th.Click([weak = get_weak()](auto&&, auto&&) { if (auto s = weak.get()) s->ApplyViewChoice(L't', 1); });
                viewSub.Items().Append(th);
            }
            viewSub.Icon(MenuIcon(L"\uE890"));
            fly.Items().Append(viewSub);
            MenuFlyoutSubItem sortSub;
            sortSub.Text(lycan::Tr(L"main.sort_by"));
            sortSub.Icon(MenuIcon(L"\uE8CB"));
            radio(sortSub, lycan::Tr(L"main.name"), sortKey_ == lycan::SortKey::Name, L's', 0, L"ctxsort");
            radio(sortSub, lycan::Tr(L"main.size"), sortKey_ == lycan::SortKey::Size, L's', 1, L"ctxsort");
            radio(sortSub, lycan::Tr(L"main.packed_size"), sortKey_ == lycan::SortKey::PackedSize, L's', 2, L"ctxsort");
            radio(sortSub, lycan::Tr(L"main.modified"), sortKey_ == lycan::SortKey::Modified, L's', 3, L"ctxsort");
            radio(sortSub, lycan::Tr(L"main.type"), sortKey_ == lycan::SortKey::Type, L's', 4, L"ctxsort");
            sortSub.Items().Append(MenuFlyoutSeparator{});
            radio(sortSub, lycan::Tr(L"main.ascending"), sortAsc_, L'd', 1, L"ctxdir");
            radio(sortSub, lycan::Tr(L"main.descending"), !sortAsc_, L'd', 0, L"ctxdir");
            fly.Items().Append(sortSub);
            sep();
            add(lycan::Tr(L"main.add_files"), [this] { AddFromPickerAsync(false, CurrentFolderPath()); }, {}, L"\uE710", IconTone::Accent);
            add(lycan::Tr(L"main.add_folder"), [this] { AddFromPickerAsync(true, CurrentFolderPath()); }, {}, L"\uE8F4", IconTone::Accent);
            add(lycan::Tr(L"main.new_folder"), [this] { NewFolderAsync(CurrentFolderPath()); }, {}, L"\uE8F4", IconTone::Accent);
            if (current_->Caps().canModify && ClipboardHasFiles()) add(lycan::Tr(L"main.paste"), [this] { PasteFilesAsync(); }, L"Ctrl+V", L"\uE77F", IconTone::Accent);   // what was copied in Explorer goes into this folder
            sep();
            add(lycan::Tr(L"main.select_all"), [this] { ActiveList().SelectAll(); }, L"Ctrl+A", L"\uE8B3");
            add(flat_ ? lycan::Tr(L"main.switch_single_level_browsing") : lycan::Tr(L"main.list_all"), [this] {
                flat_ = !flat_;
                syncingUi_ = true;
                FlatToggle().IsChecked(flat_);
                syncingUi_ = false;
                RefreshList();
            });
            if (curNode_ != tree_->Root()) add(lycan::Tr(L"main.up_one_level"), [this] { OnUpClick(nullptr, nullptr); }, L"Backspace", L"\uE74A");
        }

        lycan_app::RefreshCursorWhenOpened(fly);
        Windows::Foundation::Point pt{};
        if (args.TryGetPosition(sender, pt))
        {
            Primitives::FlyoutShowOptions opt;
            opt.Position(pt);
            fly.ShowAt(sender, opt);
        }
        else fly.ShowAt(sender.as<FrameworkElement>());
    }

    // The items for selected folders / files, shared by the list and the folder tree. strip: what stays above them when
    // they are extracted (the folder the list shows; the parent of the node in the tree).
    void MainWindow::AddNodeMenuItems(MenuFlyout const& fly, std::vector<uint32_t> const& nodes, std::wstring const& strip)
    {
        auto add = [&](hstring const& text, std::function<void()> fn, hstring const& accel = {}, const wchar_t* glyph = nullptr, IconTone tone = IconTone::None) {
            MenuFlyoutItem item;
            item.Text(text);
            if (!accel.empty()) item.KeyboardAcceleratorTextOverride(accel);
            if (glyph) item.Icon(MenuIcon(glyph, tone, RootGrid().ActualTheme() != ElementTheme::Light));
            item.Click([fn = std::move(fn)](auto&&, auto&&) { fn(); });
            fly.Items().Append(item);
        };
        auto sep = [&] { fly.Items().Append(MenuFlyoutSeparator{}); };
        const bool single = nodes.size() == 1;
        if (single)
        {
            uint32_t n = nodes[0];
            const bool isImage = !tree_->Node(n).isDir && tree_->Node(n).entryIndex != lycan::kNoEntry && lycan_app::IsPreviewableImageName(tree_->Node(n).name);
            const bool enterPreviews = isImage && settings_.doubleClickPreview && lycan_app::IsImageDecoderAvailable(tree_->Node(n).name);
            if (tree_->Node(n).isDir) add(lycan::Tr(L"main.open_folder"), [this, n] { OpenNode(n); }, L"Enter", L"\uE838");
            else
            {
                if (isImage && lycan_app::IsImageDecoderAvailable(tree_->Node(n).name)) add(lycan::Tr(L"main.open_image_preview"), [this, n] { PreviewNode(n); }, enterPreviews ? L"Enter" : L"Alt+P", L"\uE91B");
                if (tree_->Node(n).entryIndex != lycan::kNoEntry && lycan_app::IsPreviewableAudioName(tree_->Node(n).name)) add(lycan::Tr(L"main.open_audio_preview"), [this] { PreviewAudio(); }, L"Alt+Shift+P", L"\uE8D6");
                add(lycan::Tr(L"main.open_with_associated_app"), [this, n] { OpenEntryFileAsync(n); }, enterPreviews ? L"" : L"Enter", L"\uE8A7");
                if (tree_->Node(n).entryIndex != lycan::kNoEntry) add(lycan::Tr(L"main.edit_put_back_into_archive"), [this, n] { EditEntryAsync(n); }, {}, L"\uE70F", IconTone::Accent);
            }
            sep();
        }
        add(lycan::Tr(L"main.extract_selected_items_here"), [this, nodes, strip] { ExtractNodes(nodes, strip, false); }, {}, L"\uE896");
        add(lycan::Tr(L"main.extract_selected_items"), [this, nodes, strip] { ExtractNodes(nodes, strip, true); }, {}, L"\uE896");
        sep();
        add(lycan::Tr(L"main.copy_path"), [this, nodes] { CopyPaths(nodes); }, {}, L"\uE8C8");
        add(lycan::Tr(L"main.calculate_hash_values"), [this, nodes] { HashNodes(nodes); }, {}, L"\uE7C3");
        if (single && !tree_->Node(nodes[0]).isDir && tree_->Node(nodes[0]).entryIndex != lycan::kNoEntry)
        {
            add(lycan::Tr(L"main.virustotal_lookup"), [this, n = nodes[0]] { VirusTotalNode(n); }, {}, L"\uE774", IconTone::Security);
            Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(fly.Items().GetAt(fly.Items().Size() - 1), box_value(lycan::Tr(L"main.virustotal_lookup_tip")));
        }
        if (single && (flat_ || !query_.empty())) add(lycan::Tr(L"main.go_containing_folder"), [this, n = nodes[0]] { ShowNodeInFolder(n); });
        if (single) add(lycan::Tr(L"main.properties"), [this, nodes] { ShowProperties(nodes); }, {}, L"\uE946");
        sep();
        if (single && tree_->Node(nodes[0]).isDir)
            add(lycan::Tr(L"main.add_files_folder"), [this, p = tree_->Node(nodes[0]).path] { AddFromPickerAsync(false, p); }, {}, L"\uE710", IconTone::Accent);
        if (single && tree_->Node(nodes[0]).isDir && current_->Caps().canModify && ClipboardHasFiles())
            add(lycan::Tr(L"main.paste"), [this, p = tree_->Node(nodes[0]).path] { PasteFilesAsync(p, true); }, {}, L"\uE77F", IconTone::Accent);
        if (single) add(lycan::Tr(L"main.rename"), [this, n = nodes[0]] { RenameNodeAsync(n); }, L"F2", L"\uE8AC", IconTone::Accent);
        add(lycan::Tr(L"main.delete"), [this, nodes] { DeleteNodesAsync(nodes); }, L"Del", L"\uE74D", IconTone::Danger);
    }

    // The root of the folder tree is the archive itself
    void MainWindow::AddArchiveMenuItems(MenuFlyout const& fly)
    {
        auto add = [&](hstring const& text, std::function<void()> fn, const wchar_t* glyph = nullptr, IconTone tone = IconTone::None) {
            MenuFlyoutItem item;
            item.Text(text);
            if (glyph) item.Icon(MenuIcon(glyph, tone, RootGrid().ActualTheme() != ElementTheme::Light));
            item.Click([fn = std::move(fn)](auto&&, auto&&) { fn(); });
            fly.Items().Append(item);
        };
        auto sep = [&] { fly.Items().Append(MenuFlyoutSeparator{}); };
        add(lycan::Tr(L"main.menu_extract_here"), [this] { StartExtract(currentPath_.parent_path(), false); }, L"\uE896");
        add(lycan::Tr(L"main.extract"), [this] { OnExtractToClick(nullptr, nullptr); }, L"\uE896");
        add(lycan::Tr(L"main.test_integrity"), [this] { OnTestClick(nullptr, nullptr); }, L"\uE73E", IconTone::Verify);
        add(lycan::Tr(L"main.hash_values_archive"), [this] { OnHashArchiveClick(nullptr, nullptr); }, L"\uE7C3");
        add(lycan::Tr(L"sums.verify_menu"), [this] { VerifySumsFromPickerAsync(); }, L"\uE73E", IconTone::Verify);
        add(lycan::Tr(L"main.virustotal_lookup"), [this] { OnVirusTotalArchiveClick(nullptr, nullptr); }, L"\uE774", IconTone::Security);
        Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(fly.Items().GetAt(fly.Items().Size() - 1), box_value(lycan::Tr(L"main.virustotal_lookup_tip")));
        sep();
        add(lycan::Tr(L"main.add_files"), [this] { AddFromPickerAsync(false, std::wstring()); }, L"\uE710", IconTone::Accent);
        add(lycan::Tr(L"main.new_folder"), [this] { NewFolderAsync(std::wstring()); }, L"\uE8F4", IconTone::Accent);
        if (current_ && current_->Caps().canModify && ClipboardHasFiles()) add(lycan::Tr(L"main.paste"), [this] { PasteFilesAsync(std::wstring(), true); }, L"\uE77F", IconTone::Accent);
        sep();
        add(lycan::Tr(L"main.archive_information"), [this] { ShowArchiveInfoAsync(); }, L"\uE946");
        add(lycan::Tr(L"main.properties"), [this] { SHObjectProperties(Hwnd(), SHOP_FILEPATH, currentPath_.c_str(), nullptr); });  // Windows' own Properties window
        add(lycan::Tr(L"main.show_explorer"), [this] {
            if (PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(currentPath_.c_str()))
            {
                SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
                ILFree(pidl);
            }
        });
        add(lycan::Tr(L"main.copy_file_path"), [this] {
            Windows::ApplicationModel::DataTransfer::DataPackage dp;
            dp.SetText(hstring{currentPath_.wstring()});
            Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(dp);
        }, L"\uE8C8");
        if (!IsVolumePath(currentPath_))  // a split archive is several files: deleting one of them would leave the rest behind
        {
            sep();
            add(lycan::Tr(L"main.delete_archive"), [this] { DeleteCurrentArchiveAsync(); }, L"\uE74D", IconTone::Danger);
        }
    }

    void MainWindow::OnTreeContextRequested(UIElement const& sender, Input::ContextRequestedEventArgs const& args)
    {
        if (!current_ || !tree_) return;
        uint32_t node = lycan::kNoNode;
        for (auto d = args.OriginalSource().try_as<DependencyObject>(); d; d = Microsoft::UI::Xaml::Media::VisualTreeHelper::GetParent(d))
        {
            if (auto tvi = d.try_as<TreeViewItem>())
            {
                if (auto tn = FolderTree().NodeFromContainer(tvi)) node = NodeIdOf(tn);
                break;
            }
        }
        if (node == lycan::kNoNode || node >= tree_->NodeCount()) return;
        args.Handled(true);
        MenuFlyout fly;
        if (node == tree_->Root()) AddArchiveMenuItems(fly);
        else
        {
            const std::wstring parent = tree_->ParentPath(node);
            AddNodeMenuItems(fly, {node}, parent.empty() ? std::wstring() : parent + L"/");
        }
        lycan_app::RefreshCursorWhenOpened(fly);
        Windows::Foundation::Point pt{};
        if (args.TryGetPosition(sender, pt))
        {
            Primitives::FlyoutShowOptions opt;
            opt.Position(pt);
            fly.ShowAt(sender, opt);
        }
        else fly.ShowAt(sender.as<FrameworkElement>());
    }

    // "Delete the archive": to the Recycle Bin, then back to the home page. The question can be switched off (settings,
    // or the check box in the question itself).
    fire_and_forget MainWindow::DeleteCurrentArchiveAsync()
    {
        auto strong = get_strong();
        if (!current_) co_return;
        const fs::path file = currentPath_;
        const bool recycle = CanRecycle(file);
        if (settings_.confirmDeleteArchive)
        {
            auto dlg = MakeDialog(lycan::Tr(L"main.delete_archive_title"), L"\uE74D");
            StackPanel sp;
            sp.Spacing(10);
            sp.MaxWidth(440);
            TextBlock t1;
            t1.Text(hstring{recycle ? lycan::Tr(L"main.move_archive_recycle_bin_you_can")
                                    : lycan::Tr(L"main.drive_archive_has_no_recycle_bin")});
            t1.TextWrapping(TextWrapping::Wrap);
            if (!recycle) t1.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
            sp.Children().Append(t1);
            TextBlock t2;
            t2.Text(hstring{file.wstring()});
            t2.Opacity(0.8);
            t2.TextWrapping(TextWrapping::Wrap);
            sp.Children().Append(t2);
            CheckBox again;
            again.Content(box_value(hstring{lycan::Tr(L"main.delete_without_asking_from_now_on")}));
            sp.Children().Append(again);
            dlg.Content(sp);
            dlg.PrimaryButtonText(recycle ? lycan::Tr(L"main.move_recycle_bin") : lycan::Tr(L"main.delete_permanently"));
            dlg.CloseButtonText(lycan::Tr(L"main.cancel"));
            dlg.DefaultButton(ContentDialogButton::Close);
            if (co_await ShowDialogAsync(dlg) != ContentDialogResult::Primary) co_return;
            if (again.IsChecked().Value())
            {
                settings_.confirmDeleteArchive = false;
                loadingSettings_ = true;
                ConfirmDeleteToggle().IsOn(false);
                loadingSettings_ = false;
                SaveSettings();
            }
        }
        if (!current_ || currentPath_ != file) co_return;  // something else was opened while the question was open
        int deleted = 0;
        UnlockArchiveFile();
        const int failed = ShellDelete(Hwnd(), {file}, recycle, deleted);
        if (failed != 0) LockArchiveFile(currentPath_);
        if (failed == 0) ShowHome();
        else ShowMessage(lycan::Tr(L"main.archive_could_not_be_deleted"), lycan::Tr(L"main.file_may_be_use_by_another"));
    }
}
