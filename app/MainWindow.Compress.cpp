#include "pch.h"
// The main window: the "New archive" page and repacking (convert the format, remove the password, upgrade an archive).
#include "MainWindowShared.h"

namespace winrt::LycanZip::implementation
{
    // ---------------------------------------------------------------- Creating an archive

    fire_and_forget MainWindow::OnNewFromFilesClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        auto strong = get_strong();
        auto files = co_await lycan_app::PickFilesAsync(AppWindow().Id(), lycan_app::kPurposeSourceFiles, true, false);
        std::vector<fs::path> paths;
        for (auto const& f : files) paths.emplace_back(std::wstring(f));
        if (!paths.empty()) StartCompress(std::move(paths));
    }

    fire_and_forget MainWindow::OnNewFromFolderClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        auto strong = get_strong();
        const fs::path dir{std::wstring(co_await lycan_app::PickFolderAsync(AppWindow().Id(), lycan_app::kPurposeSourceFolder, {}, lycan::Tr(L"main.choose_folder_compress")))};
        if (!dir.empty()) StartCompress({dir});
    }

    // ---------------------------------------------------------------- "New archive" page

    namespace
    {
        struct ValueItem { const wchar_t* text; int64_t value; };
        constexpr const wchar_t* kFormatNames[] = {L"ZIP", L"7z", L"TAR.GZ", L"TAR.XZ", L"TAR.BZ2", L"TAR", L"ISO"};
        constexpr const wchar_t* kProfileNames[] = {L"main.balanced_recommended", L"main.compatibility_first", L"main.smallest_size", L"main.fastest"};
        constexpr const wchar_t* kProfileShort[] = {L"main.balanced", L"main.compatibility_first", L"main.smallest_size", L"main.fastest"};

        ComboBoxItem MakeItem(hstring const& text, Windows::Foundation::IInspectable const& tag)
        {
            ComboBoxItem it;
            it.Content(box_value(text));
            it.Tag(tag);
            return it;
        }

        void FillValues(ComboBox const& c, std::initializer_list<ValueItem> items)
        {
            c.Items().Clear();
            for (auto const& v : items) c.Items().Append(MakeItem(v.text, box_value<int64_t>(v.value)));
            c.SelectedIndex(0);
        }

        int64_t ComboValue(ComboBox const& c, int64_t dflt)
        {
            auto it = c.SelectedItem().try_as<ComboBoxItem>();
            return it ? unbox_value_or<int64_t>(it.Tag(), dflt) : dflt;
        }

        // Select the given value; if the list does not have it (for example a preset stored a custom size) add an item
        void SelectValue(ComboBox const& c, int64_t v, std::wstring const& customText)
        {
            for (uint32_t i = 0; i < c.Items().Size(); ++i)
            {
                auto it = c.Items().GetAt(i).try_as<ComboBoxItem>();
                if (it && unbox_value_or<int64_t>(it.Tag(), INT64_MIN) == v) { c.SelectedIndex(static_cast<int32_t>(i)); return; }
            }
            c.Items().Append(MakeItem(hstring{customText}, box_value<int64_t>(v)));
            c.SelectedIndex(static_cast<int32_t>(c.Items().Size()) - 1);
        }

        std::wstring EffectiveMethod(lycan::CompressPreset const& p)
        {
            if (!p.method.empty()) return p.method;
            return p.type == lycan::ArchiveType::SevenZip ? L"LZMA2" : p.type == lycan::ArchiveType::Zip ? L"Deflate" : L"";
        }

        // Zero the fields that are not used: a disabled control may still hold an old value, which must not affect the result or the judgment of "modified"
        void NormalizePreset(lycan::CompressPreset& p)
        {
            const bool zip = p.type == lycan::ArchiveType::Zip, sz = p.type == lycan::ArchiveType::SevenZip;
            if (!sz) { p.solidSize = lycan::kSolidAuto; p.encryptFileNames = false; }
            if (!zip) p.zipEncryption.clear();
            if (p.type == lycan::ArchiveType::Iso) { p.profile = lycan::Profile::Balanced; p.level = -1; p.threads = 0; p.pathMode = lycan::PathMode::Relative; p.customParams.clear(); }
            if (!zip && !sz) { p.method.clear(); p.volumeSize = 0; p.dictSize = 0; p.wordSize = 0; p.storeCompressed = true; }
            else
            {
                auto m = EffectiveMethod(p);
                if (!lycan::MethodHasDict(m)) p.dictSize = 0;
                if (!lycan::MethodHasWordSize(m)) p.wordSize = 0;
            }
        }

        std::wstring TrimW(std::wstring s)
        {
            while (!s.empty() && iswspace(s.back())) s.pop_back();
            size_t i = 0;
            while (i < s.size() && iswspace(s[i])) ++i;
            return s.substr(i);
        }

        constexpr int64_t kVolCustom = -2;  // the value of the "Custom..." item

        // A size typed by the user: "50 MB", "1.5gb", "700" (no unit = MB). At least 64 KB (the lower limit of the Core), at most 1 TB.
        std::optional<uint64_t> ParseSizeInput(std::wstring s)
        {
            s = TrimW(s);
            size_t i = 0;
            bool dot = false;
            while (i < s.size() && (iswdigit(s[i]) || (!dot && s[i] == L'.'))) { dot |= s[i] == L'.'; ++i; }
            if (i == 0) return std::nullopt;
            double num = 0;
            try { num = std::stod(s.substr(0, i)); } catch (...) { return std::nullopt; }
            std::wstring unit = TrimW(s.substr(i));
            for (auto& ch : unit) ch = static_cast<wchar_t>(towlower(ch));
            double mult = 1048576.0;
            if (unit == L"b") mult = 1;
            else if (unit == L"k" || unit == L"kb") mult = 1024.0;
            else if (unit == L"m" || unit == L"mb" || unit.empty()) mult = 1048576.0;
            else if (unit == L"g" || unit == L"gb") mult = 1073741824.0;
            else return std::nullopt;
            const double bytes = num * mult;
            if (!(bytes >= 65536.0) || bytes > 1099511627776.0) return std::nullopt;
            return static_cast<uint64_t>(std::llround(bytes));
        }

        std::wstring FormatSizeInput(uint64_t b)
        {
            if (b % (1ull << 30) == 0) return std::to_wstring(b >> 30) + L" GB";
            if (b % (1ull << 20) == 0) return std::to_wstring(b >> 20) + L" MB";
            if (b % 1024 == 0) return std::to_wstring(b >> 10) + L" KB";
            return std::to_wstring(b) + L" B";
        }

        std::wstring DescribePreset(lycan::CompressPreset const& p, bool hasPassword)
        {
            std::vector<std::wstring> parts;
            parts.push_back(kFormatNames[IndexOf(kFormatOrder, p.type)]);
            if (!p.method.empty()) parts.push_back(p.method);
            if (p.level == 0) parts.push_back(lycan::Tr(L"main.store_only"));
            else if (p.level > 0) parts.push_back(lycan::Tf(L"main.level", p.level));
            else parts.push_back(lycan::Tr(kProfileShort[IndexOf(kProfileOrder, p.profile)]));
            if (p.type == lycan::ArchiveType::SevenZip)
            {
                if (p.solidSize == lycan::kSolidAll) parts.push_back(lycan::Tr(L"main.single_solid_block"));
                else if (p.solidSize == lycan::kSolidOff) parts.push_back(lycan::Tr(L"main.non_solid"));
            }
            if (hasPassword) parts.push_back(p.encryptFileNames ? lycan::Tr(L"main.password_protected_incl_file_names") : lycan::Tr(L"main.password_protected"));
            if (p.volumeSize) parts.push_back(lycan::Tf(L"main.split", FormatSize(p.volumeSize)));
            if (p.pathMode == lycan::PathMode::Full) parts.push_back(lycan::Tr(L"main.full_path"));
            std::wstring out;
            for (size_t i = 0; i < parts.size(); ++i) out += (i ? L" · " : L"") + parts[i];
            return out;
        }
    }

    void MainWindow::InitCompressPage()
    {
        auto weak = get_weak();
        // Static option lists
        for (auto n : kFormatNames) CFormatCombo().Items().Append(box_value(hstring{n}));
        for (auto n : kProfileNames) CProfileCombo().Items().Append(box_value(hstring{lycan::Tr(n)}));
        FillValues(CLevelCombo(), {{lycan::Tr(L"main.by_mode_default"), -1}, {lycan::Tr(L"main.store_only_no_compression"), 0}, {lycan::Tr(L"main.1_fastest"), 1}, {lycan::Tr(L"main.3_fast"), 3}, {lycan::Tr(L"main.5_normal"), 5}, {lycan::Tr(L"main.7_maximum"), 7}, {lycan::Tr(L"main.9_ultra"), 9}});
        FillValues(CDictCombo(), {{lycan::Tr(L"main.auto"), 0}, {L"64 KB", 64ll << 10}, {L"256 KB", 256ll << 10}, {L"1 MB", 1ll << 20}, {L"4 MB", 4ll << 20},
                                  {L"16 MB", 16ll << 20}, {L"32 MB", 32ll << 20}, {L"64 MB", 64ll << 20}, {L"128 MB", 128ll << 20},
                                  {L"256 MB", 256ll << 20}, {L"512 MB", 512ll << 20}, {L"1 GB", 1ll << 30}});
        FillValues(CSolidCombo(), {{lycan::Tr(L"main.auto"), lycan::kSolidAuto}, {lycan::Tr(L"main.non_solid_each_file_separately"), lycan::kSolidOff}, {L"1 MB", 1ll << 20}, {L"16 MB", 16ll << 20},
                                   {L"64 MB", 64ll << 20}, {L"256 MB", 256ll << 20}, {L"1 GB", 1ll << 30}, {L"4 GB", 4ll << 30},
                                   {lycan::Tr(L"main.all_single_block_smallest"), lycan::kSolidAll}});
        FillValues(CThreadsCombo(), {{lycan::Tr(L"main.auto"), 0}, {L"1", 1}, {L"2", 2}, {L"4", 4}, {L"8", 8}, {L"16", 16}, {L"32", 32}});
        FillValues(CZipEncCombo(), {{lycan::Tr(L"main.by_mode_aes_256"), 0}, {L"AES-256", 1}, {lycan::Tr(L"main.zipcrypto_compatible_with_old"), 2}});
        FillValues(CPathModeCombo(), {{lycan::Tr(L"main.relative_path"), 0}, {lycan::Tr(L"main.full_path_without_drive_letter"), 1}});
        FillValues(CConvertPwCombo(), {{lycan::Tr(L"main.keep_original_password"), 0}, {lycan::Tr(L"main.no_password"), 1}, {lycan::Tr(L"main.new_password"), 2}});
        CConvertPwCombo().SelectionChanged([weak](auto&&, auto&&) {
            auto s = weak.get();
            if (!s || s->cLoading_) return;
            if (s->CConvertPwCombo().SelectedIndex() != 2) s->SetCompressPassword(L"");
            s->RefreshCompressUi();
        });
        FillValues(CVolCombo(), {{lycan::Tr(L"main.do_not_split"), 0}, {lycan::Tr(L"main.25_mb_e_mail_attachment"), 25ll << 20}, {L"100 MB", 100ll << 20}, {L"700 MB (CD)", 700ll << 20},
                                 {L"1 GB", 1ll << 30}, {L"2 GB", 2ll << 30}, {lycan::Tr(L"main.4_gb_fat32_limit"), 4ll << 30},
                                 {lycan::Tr(L"main.custom"), kVolCustom}});
        RebuildMethodItems(lycan::ArchiveType::Zip);
        RebuildWordItems(false);

        auto changed = [weak](auto&&, auto&&) { if (auto s = weak.get()) s->RefreshCompressUi(); };
        for (auto c : {CProfileCombo(), CLevelCombo(), CDictCombo(), CWordCombo(), CSolidCombo(), CThreadsCombo(), CZipEncCombo(), CPathModeCombo(), CVolCombo()})
            c.SelectionChanged(changed);
        CFormatCombo().SelectionChanged([weak](auto&&, auto&&) {
            auto s = weak.get();
            if (!s || s->cLoading_) return;
            s->cLoading_ = true;
            s->RebuildMethodItems(kFormatOrder[std::clamp(s->CFormatCombo().SelectedIndex(), 0, 6)]);
            s->RebuildWordItems(false);
            s->cLoading_ = false;
            s->RefreshCompressUi();
        });
        CMethodCombo().SelectionChanged([weak](auto&&, auto&&) {
            auto s = weak.get();
            if (!s || s->cLoading_) return;
            s->cLoading_ = true;
            auto it = s->CMethodCombo().SelectedItem().try_as<ComboBoxItem>();
            s->RebuildWordItems(it && _wcsicmp(unbox_value_or<hstring>(it.Tag(), L"").c_str(), L"PPMd") == 0);
            s->cLoading_ = false;
            s->RefreshCompressUi();
        });
        CShowPw().IsChecked(true);
        CPwText().TextChanged(changed);
        CPwBox().PasswordChanged(changed);
        CPwConfirm().PasswordChanged(changed);
        CVolCustom().TextChanged(changed);
        CShowPw().Click([weak](auto&&, auto&&) { if (auto s = weak.get()) s->ApplyPasswordMode(); });
        CParamsBox().TextChanged(changed);
        CEncNames().Toggled(changed);
        CStoreCompressed().Toggled(changed);
        CTestAfter().Toggled([weak](auto&&, auto&&) {
            auto s = weak.get();
            if (!s || s->cLoading_) return;
            s->settings_.testAfterCompress = s->CTestAfter().IsOn();  // remembered (the same setting as the compact window and the settings page)
            s->SaveSettings();
            s->LoadSettingsToUi();
        });
        CSumsArchive().Toggled(changed);   // part of the template (the settings page has the defaults for the right-click compress without a template)
        CSumsFiles().Toggled(changed);
        CNameBox().TextChanged([weak](auto&&, auto&&) {
            auto s = weak.get();
            if (!s || s->cLoading_) return;
            s->cNameEdited_ = true;
            s->RefreshCompressUi();
        });
        CPresetCombo().SelectionChanged([weak](auto&&, auto&&) {
            auto s = weak.get();
            if (!s || s->cLoading_) return;
            auto it = s->CPresetCombo().SelectedItem().try_as<ComboBoxItem>();
            if (!it) return;
            std::wstring name{unbox_value_or<hstring>(it.Tag(), L"")};
            for (auto const* list : {&s->cBuiltin_, &s->cUser_})
                for (auto const& p : *list)
                    if (p.name == name) { s->cBase_ = p; s->LoadPresetToUi(p); return; }
        });
        CChangeDirButton().Click([weak](auto&&, auto&&) -> fire_and_forget {
            auto s = weak.get();
            if (!s) co_return;
            const fs::path dir{std::wstring(co_await lycan_app::PickFolderAsync(s->AppWindow().Id(), lycan_app::kPurposeCompressOut, s->cOutDir_, lycan::Tr(L"main.choose_output_folder_archive")))};
            if (!dir.empty()) { s->cOutDir_ = dir; s->RefreshCompressUi(); }
        });
        CAddFiles().Click([weak](auto&&, auto&&) { if (auto s = weak.get()) s->AddSourcesFromPicker(false); });
        CAddFolder().Click([weak](auto&&, auto&&) { if (auto s = weak.get()) s->AddSourcesFromPicker(true); });
        CBackButton().Click([weak](auto&&, auto&&) { if (auto s = weak.get()) s->LeaveCompressPage(); });
        CCancelButton().Click([weak](auto&&, auto&&) { if (auto s = weak.get()) s->LeaveCompressPage(); });
        CStartButton().Click([weak](auto&&, auto&&) { if (auto s = weak.get()) s->OnCompressStart(); });
        CResetAdvanced().Click([weak](auto&&, auto&&) { if (auto s = weak.get()) s->LoadPresetToUi(s->cBase_); });
        CAdvanced().Expanding([weak](auto&&, auto&&) {
            auto s = weak.get();
            if (!s || s->cLoading_) return;
            s->settings_.compressAdvancedOpen = true;
            s->SaveSettings();
        });
        CAdvanced().Collapsed([weak](auto&&, auto&&) {
            auto s = weak.get();
            if (!s || s->cLoading_) return;
            s->settings_.compressAdvancedOpen = false;
            s->SaveSettings();
        });

        // Preset menu
        MenuFlyout menu;
        auto add = [&](const wchar_t* text, auto&& handler) {
            MenuFlyoutItem item;
            item.Text(text);
            item.Click([weak, handler](auto&&, auto&&) { if (auto s = weak.get()) handler(*s); });
            menu.Items().Append(item);
            return item;
        };
        add(lycan::Tr(L"main.menu_save_as_new_template"), [](MainWindow& s) { s.OnPresetSaveNew(); });
        cMenuUpdate_ = add(lycan::Tr(L"main.update_template_with_current"), [](MainWindow& s) { s.OnPresetUpdate(); });
        cMenuRename_ = add(lycan::Tr(L"main.menu_rename_template"), [](MainWindow& s) { s.OnPresetRename(); });
        cMenuDelete_ = add(lycan::Tr(L"main.delete_template"), [](MainWindow& s) { s.OnPresetDelete(); });
        menu.Items().Append(MenuFlyoutSeparator{});
        add(lycan::Tr(L"main.restore_template_s_settings"), [](MainWindow& s) { s.LoadPresetToUi(s.cBase_); });
        menu.Items().Append(MenuFlyoutSeparator{});
        cMenuInMenu_ = ToggleMenuFlyoutItem{};
        cMenuInMenu_.Text(lycan::Tr(L"main.show_right_click_menu_compress"));
        cMenuInMenu_.Click([weak](auto&&, auto&&) {
            auto s = weak.get();
            if (!s) return;
            s->SetPresetInMenu(s->cBase_.name, s->cBase_.builtin, s->cMenuInMenu_.IsChecked());
        });
        menu.Items().Append(cMenuInMenu_);
        CPresetMenu().Flyout(menu);
    }

    std::wstring MainWindow::CompressPassword()
    {
        return CShowPw().IsChecked().Value() ? std::wstring{CPwText().Text()} : std::wstring{CPwBox().Password()};
    }

    void MainWindow::SetCompressPassword(std::wstring const& pw)
    {
        const bool was = cLoading_;
        cLoading_ = true;
        CPwText().Text(hstring{pw});
        CPwBox().Password(hstring{pw});
        CPwConfirm().Password(L"");
        cLoading_ = was;
    }

    // Toggle "show password": carries what was typed over to the other kind of box
    void MainWindow::ApplyPasswordMode()
    {
        const bool show = CShowPw().IsChecked().Value();
        const bool was = cLoading_;
        cLoading_ = true;
        if (show) CPwText().Text(CPwBox().Password());
        else { CPwBox().Password(CPwText().Text()); CPwConfirm().Password(L""); }
        cLoading_ = was;
        RefreshCompressUi();
    }

    void MainWindow::RebuildMethodItems(lycan::ArchiveType type)
    {
        auto c = CMethodCombo();
        c.Items().Clear();
        auto methods = lycan::MethodsFor(type);
        if (methods.empty()) c.Items().Append(MakeItem(lycan::Tr(L"main.fixed_format"), box_value(hstring{})));
        else
        {
            c.Items().Append(MakeItem(lycan::Tr(L"main.auto"), box_value(hstring{})));
            for (auto const& m : methods) c.Items().Append(MakeItem(hstring{m}, box_value(hstring{m})));
        }
        c.SelectedIndex(0);
    }

    // For PPMd the "word size" is the model order (2-32), for other methods it is the word length
    void MainWindow::RebuildWordItems(bool ppmd)
    {
        auto c = CWordCombo();
        const int64_t keep = ComboValue(c, 0);
        c.Items().Clear();
        c.Items().Append(MakeItem(lycan::Tr(L"main.auto"), box_value<int64_t>(0)));
        bool found = keep == 0;
        if (ppmd) for (int64_t v : {4, 6, 8, 12, 16, 24, 32}) { c.Items().Append(MakeItem(hstring{std::to_wstring(v)}, box_value<int64_t>(v))); found |= v == keep; }
        else for (int64_t v : {8, 12, 16, 24, 32, 48, 64, 96, 128, 192, 256}) { c.Items().Append(MakeItem(hstring{std::to_wstring(v)}, box_value<int64_t>(v))); found |= v == keep; }
        if (found) SelectValue(c, keep, std::to_wstring(keep));
        else c.SelectedIndex(0);
    }

    lycan::CompressPreset MainWindow::ReadCompressUi()
    {
        lycan::CompressPreset p;
        p.type = kFormatOrder[std::clamp(CFormatCombo().SelectedIndex(), 0, 6)];
        p.profile = kProfileOrder[std::clamp(CProfileCombo().SelectedIndex(), 0, 3)];
        p.level = static_cast<int>(ComboValue(CLevelCombo(), -1));
        if (auto it = CMethodCombo().SelectedItem().try_as<ComboBoxItem>()) p.method = std::wstring{unbox_value_or<hstring>(it.Tag(), L"")};
        p.dictSize = static_cast<uint64_t>(std::max<int64_t>(0, ComboValue(CDictCombo(), 0)));
        p.wordSize = static_cast<unsigned>(std::max<int64_t>(0, ComboValue(CWordCombo(), 0)));
        p.solidSize = ComboValue(CSolidCombo(), lycan::kSolidAuto);
        const int ze = static_cast<int>(ComboValue(CZipEncCombo(), 0));
        p.zipEncryption = ze == 1 ? L"AES256" : ze == 2 ? L"ZipCrypto" : L"";
        p.encryptFileNames = CEncNames().IsOn();
        p.storeCompressed = CStoreCompressed().IsOn();
        p.sumsArchive = CSumsArchive().IsOn();
        p.sumsFiles = CSumsFiles().IsOn();
        const int64_t vol = ComboValue(CVolCombo(), 0);
        cVolInvalid_ = false;
        if (vol == kVolCustom)
        {
            auto bytes = ParseSizeInput(std::wstring{CVolCustom().Text()});
            cVolInvalid_ = !bytes;
            p.volumeSize = bytes.value_or(0);
        }
        else p.volumeSize = static_cast<uint64_t>(std::max<int64_t>(0, vol));
        p.threads = static_cast<unsigned>(std::max<int64_t>(0, ComboValue(CThreadsCombo(), 0)));
        p.pathMode = ComboValue(CPathModeCombo(), 0) == 1 ? lycan::PathMode::Full : lycan::PathMode::Relative;
        p.customParams = TrimW(std::wstring{CParamsBox().Text()});
        NormalizePreset(p);
        return p;
    }

    void MainWindow::LoadPresetToUi(lycan::CompressPreset const& p)
    {
        cLoading_ = true;
        CFormatCombo().SelectedIndex(IndexOf(kFormatOrder, p.type));
        CProfileCombo().SelectedIndex(IndexOf(kProfileOrder, p.profile));
        RebuildMethodItems(p.type);
        for (uint32_t i = 0; i < CMethodCombo().Items().Size(); ++i)
        {
            auto it = CMethodCombo().Items().GetAt(i).try_as<ComboBoxItem>();
            if (it && _wcsicmp(unbox_value_or<hstring>(it.Tag(), L"").c_str(), p.method.c_str()) == 0) { CMethodCombo().SelectedIndex(static_cast<int32_t>(i)); break; }
        }
        RebuildWordItems(_wcsicmp(EffectiveMethod(p).c_str(), L"PPMd") == 0);
        SelectValue(CLevelCombo(), p.level, lycan::Tf(L"main.level", p.level));
        SelectValue(CDictCombo(), static_cast<int64_t>(p.dictSize), FormatSize(p.dictSize));
        SelectValue(CWordCombo(), p.wordSize, std::to_wstring(p.wordSize));
        SelectValue(CSolidCombo(), p.solidSize, FormatSize(static_cast<uint64_t>(p.solidSize)));
        SelectValue(CThreadsCombo(), p.threads, std::to_wstring(p.threads));
        CZipEncCombo().SelectedIndex(p.zipEncryption == L"AES256" ? 1 : p.zipEncryption == L"ZipCrypto" ? 2 : 0);
        CEncNames().IsOn(p.encryptFileNames);
        CStoreCompressed().IsOn(p.storeCompressed);
        {
            bool found = false;
            for (uint32_t i = 0; i < CVolCombo().Items().Size() && !found; ++i)
            {
                auto it = CVolCombo().Items().GetAt(i).try_as<ComboBoxItem>();
                if (it && unbox_value_or<int64_t>(it.Tag(), INT64_MIN) == static_cast<int64_t>(p.volumeSize)) { CVolCombo().SelectedIndex(static_cast<int32_t>(i)); found = true; }
            }
            if (!found)  // the preset stores a size that is not in the list: select "Custom..." and fill it in
            {
                SelectValue(CVolCombo(), kVolCustom, lycan::Tr(L"main.custom"));
                CVolCustom().Text(hstring{FormatSizeInput(p.volumeSize)});
            }
        }
        CPathModeCombo().SelectedIndex(p.pathMode == lycan::PathMode::Full ? 1 : 0);
        CParamsBox().Text(hstring{p.customParams});
        CTestAfter().IsOn(settings_.testAfterCompress);
        CSumsArchive().IsOn(p.sumsArchive);
        CSumsFiles().IsOn(p.sumsFiles);
        cLoading_ = false;
        cBaseNorm_ = ReadCompressUi();
        RefreshCompressUi();
    }

    void MainWindow::FillPresetCombo(std::wstring const& select)
    {
        const bool was = cLoading_;
        cLoading_ = true;
        CPresetCombo().Items().Clear();
        int sel = 0, i = 0;
        for (auto const* list : {&cBuiltin_, &cUser_})
            for (auto const& p : *list)
            {
                // The names of built-in presets are data (Tag identifies them), only the displayed text is translated; a name the user chose is shown as it is
                CPresetCombo().Items().Append(MakeItem(hstring{list == &cBuiltin_ ? std::wstring(lycan::Tr(lycan::BuiltinPresetNameId(p.name))) : p.name}, box_value(hstring{p.name})));
                if (p.name == select) sel = i;
                ++i;
            }
        CPresetCombo().SelectedIndex(sel);
        cLoading_ = was;
    }

    // Control state, output file name, summary and the "modified" hint; called whenever any setting changes
    void MainWindow::RefreshCompressUi()
    {
        if (cLoading_ || !cPageActive_) return;
        auto p = ReadCompressUi();
        const bool zip = p.type == lycan::ArchiveType::Zip, sz = p.type == lycan::ArchiveType::SevenZip, arch = zip || sz;
        const bool iso = p.type == lycan::ArchiveType::Iso;
        const auto method = EffectiveMethod(p);
        // an ISO image holds the files as they are: nothing of the compression is used
        CIsoCard().Visibility(iso ? Visibility::Visible : Visibility::Collapsed);
        for (auto c : {CProfileCombo(), CLevelCombo(), CThreadsCombo(), CPathModeCombo()}) c.IsEnabled(!iso);
        CParamsBox().IsEnabled(!iso);
        CPwText().IsEnabled(arch);
        CPwBox().IsEnabled(arch);
        CPwConfirm().IsEnabled(arch);
        CShowPw().IsEnabled(arch);
        {
            // "Use a saved password": when there are saved ones (or they are locked by the master password)
            auto& vault = lycan_app::PasswordVault::Get();
            const bool any = vault.Locked() || (vault.State() == lycan::PasswordFileState::Ok && !vault.Saved().empty());
            CPwPick().Visibility(any ? Visibility::Visible : Visibility::Collapsed);
            CPwPick().IsEnabled(arch);
        }
        CVolCombo().IsEnabled(arch);
        const bool customVol = ComboValue(CVolCombo(), 0) == kVolCustom;
        CVolCustom().Visibility(customVol ? Visibility::Visible : Visibility::Collapsed);
        CVolCustom().IsEnabled(arch);
        CMethodCombo().IsEnabled(arch);
        CDictCombo().IsEnabled(arch && lycan::MethodHasDict(method));
        CWordCombo().IsEnabled(arch && lycan::MethodHasWordSize(method));
        CSolidCombo().IsEnabled(sz);
        CZipEncCombo().IsEnabled(zip);
        CEncNames().IsEnabled(sz);
        CStoreCompressed().IsEnabled(arch);
        if (!arch && !CompressPassword().empty()) SetCompressPassword(L"");

        const bool showPw = CShowPw().IsChecked().Value();
        CPwText().Visibility(showPw ? Visibility::Visible : Visibility::Collapsed);
        CPwBox().Visibility(showPw ? Visibility::Collapsed : Visibility::Visible);
        const std::wstring pw = CompressPassword();
        const bool hasPw = !pw.empty() && arch;
        // when the password is visible there is no need to type it again; when hidden, ask for confirmation
        CPwConfirm().Visibility(hasPw && !showPw ? Visibility::Visible : Visibility::Collapsed);
        if (cConvert_)
        {
            // converting: the password fields only for "a new password" (keep / none need nothing typed)
            CConvertPwCombo().IsEnabled(arch);
            if (CConvertPwCombo().SelectedIndex() != 2)
                for (UIElement e : {UIElement{CPwText()}, UIElement{CPwBox()}, UIElement{CPwConfirm()}, UIElement{CShowPw()}, UIElement{CPwPick()}})
                    e.Visibility(Visibility::Collapsed);
        }

        // Reasons that block the start (shown in the password area or the status bar)
        std::wstring pwNote, blocker, warning;
        if (hasPw && !showPw && std::wstring{CPwConfirm().Password()} != pw) { pwNote = lycan::Tr(L"main.two_passwords_do_not_match"); blocker = pwNote; }
        else if (zip && std::any_of(pw.begin(), pw.end(), [](wchar_t c) { return c > 0x7F; }))
        { pwNote = lycan::Tr(L"main.zip_passwords_can_only_use_letters"); blocker = pwNote; }
        else if (hasPw && zip && (p.zipEncryption == L"ZipCrypto" || (p.zipEncryption.empty() && p.profile == lycan::Profile::Compatible)))
            pwNote = lycan::Tr(L"main.zipcrypto_weakly_encrypted");
        else if (iso) pwNote = lycan::Tr(L"main.iso_images_not_compressed_do_not");
        else if (!arch) pwNote = lycan::Tr(L"main.tar_family_does_not_support");
        else if (sz && p.encryptFileNames && pw.empty()) pwNote = lycan::Tr(L"main.no_password_entered_yet_so_file");
        CPwNote().Text(hstring{pwNote});
        CPwNote().Visibility(pwNote.empty() ? Visibility::Collapsed : Visibility::Visible);

        // Output
        std::wstring name = TrimW(std::wstring{CNameBox().Text()});
        const bool eachOwnName = cConvert_ && cSources_.size() > 1;   // converting several archives: each keeps its name
        const bool badName = !eachOwnName && (name.empty() || name.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos || name.back() == L'.');
        const int fi = IndexOf(kFormatOrder, p.type);
        COutDirText().Text(hstring{lycan::Tf(L"main.save", cOutDir_.wstring())});
        COutNameText().Text(hstring{badName ? std::wstring{lycan::Tr(L"main.invalid_file_name_chars")}
                                    : eachOwnName ? lycan::Tf(L"main.convert_each_keeps_name", kFormatExt[fi])
                                    : lycan::Tf(L"main.output_file", name + kFormatExt[fi]) + (p.volumeSize ? lycan::Tr(L"main.split_into_001_002") : L"")});
        if (badName && blocker.empty()) blocker = lycan::Tr(L"main.invalid_file_name");
        if (cSources_.empty() && blocker.empty()) blocker = lycan::Tr(L"main.please_add_files_folders_compress");
        if (arch && customVol && cVolInvalid_ && blocker.empty()) blocker = lycan::Tr(L"main.invalid_split_size_enter_size_like");

        // Preset state
        const bool modified = !(p.SameSettings(cBaseNorm_));
        CPresetModified().Visibility(modified ? Visibility::Visible : Visibility::Collapsed);
        cMenuUpdate_.IsEnabled(!cBase_.builtin && modified);
        cMenuRename_.IsEnabled(!cBase_.builtin);
        cMenuDelete_.IsEnabled(!cBase_.builtin);
        if (cMenuInMenu_) cMenuInMenu_.IsChecked(PresetInMenu(cBase_.name, cBase_.builtin));

        if (p.customParams.empty() == false) warning = lycan::Tr(L"main.custom_parameters_set_if_they_do");
        if (cConvert_ && !cConvertNote_.empty() && warning.empty()) warning = cConvertNote_;
        std::wstring status = DescribePreset(p, hasPw) + lycan::Tf(L"fmt.n_sources", cSources_.size());
        if (!blocker.empty()) status += L"\n⚠ " + blocker;
        else if (!warning.empty()) status += L"\n" + warning;
        CStatusText().Text(hstring{status});
        CStartButton().IsEnabled(blocker.empty());
    }

    std::wstring MainWindow::DefaultArchiveBaseName() const
    {
        if (cSources_.empty()) return L"Archive";
        if (cConvert_) return lycan::ArchiveBaseName(cSources_[0]);
        std::wstring baseName = cSources_.size() == 1 ? cSources_[0].stem().wstring() : cSources_[0].parent_path().filename().wstring();
        std::error_code ec;
        if (cSources_.size() == 1 && fs::is_directory(cSources_[0], ec)) baseName = cSources_[0].filename().wstring();
        return baseName.empty() ? L"Archive" : baseName;
    }

    void MainWindow::RebuildSources()
    {
        auto list = CSourcesList();
        list.Children().Clear();
        CSourcesTitle().Text(hstring{lycan::Tf(L"main.sources", cSources_.size())});
        constexpr size_t kMaxShown = 100;
        auto weak = get_weak();
        for (size_t i = 0; i < cSources_.size() && i < kMaxShown; ++i)
        {
            const fs::path p = cSources_[i];
            std::error_code ec;
            const bool dir = fs::is_directory(p, ec);
            Grid row;
            row.ColumnSpacing(8);
            ColumnDefinition c0, c1, c2;
            c0.Width(GridLength{0, GridUnitType::Auto});
            c1.Width(GridLength{1, GridUnitType::Star});
            c2.Width(GridLength{0, GridUnitType::Auto});
            row.ColumnDefinitions().Append(c0);
            row.ColumnDefinitions().Append(c1);
            row.ColumnDefinitions().Append(c2);
            auto glyph = MakeGlyph(dir ? L"" : L"", 18);
            row.Children().Append(glyph);
            StackPanel names;
            TextBlock n;
            n.Text(hstring{p.filename().empty() ? p.wstring() : p.filename().wstring()});
            n.TextTrimming(TextTrimming::CharacterEllipsis);
            names.Children().Append(n);
            auto sub = Caption(p.parent_path().wstring(), 0.55);
            sub.TextTrimming(TextTrimming::CharacterEllipsis);
            sub.TextWrapping(TextWrapping::NoWrap);
            names.Children().Append(sub);
            Grid::SetColumn(names, 1);
            row.Children().Append(names);
            Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(row, box_value(hstring{p.wstring()}));
            Button rm;
            rm.Content(MakeGlyph(L"", 11));
            rm.Padding(ThicknessHelper::FromUniformLength(6));
            rm.Background(Microsoft::UI::Xaml::Media::SolidColorBrush{Windows::UI::Color{0, 0, 0, 0}});
            rm.BorderThickness(ThicknessHelper::FromUniformLength(0));
            Automation::AutomationProperties::SetName(rm, lycan::Tr(L"main.remove"));
            Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(rm, box_value(lycan::Tr(L"main.remove")));
            rm.Click([weak, p](auto&&, auto&&) {
                auto s = weak.get();
                if (!s) return;
                s->cSources_.erase(std::remove(s->cSources_.begin(), s->cSources_.end(), p), s->cSources_.end());
                s->OnSourcesChanged();
            });
            Grid::SetColumn(rm, 2);
            row.Children().Append(rm);
            list.Children().Append(row);
        }
        if (cSources_.size() > kMaxShown) list.Children().Append(Caption(lycan::Tf(L"main.and_n_more_sources", cSources_.size() - kMaxShown), 0.65));
        CSourcesInfo().Text(lycan::Tr(L"main.calculating"));
        ScanSources(cSources_, CSourcesInfo(), dispatcher_);
    }

    void MainWindow::OnSourcesChanged()
    {
        RebuildSources();
        if (!cNameEdited_)
        {
            cLoading_ = true;
            CNameBox().Text(hstring{DefaultArchiveBaseName()});
            cLoading_ = false;
        }
        RefreshCompressUi();
    }

    void MainWindow::UpdateCompressLayout()
    {
        const bool wide = RootGrid().ActualWidth() >= 900;
        auto card = CSourcesCard();
        CSourcesNarrowHost().Visibility(wide ? Visibility::Collapsed : Visibility::Visible);  // an empty container still takes one gap
        auto from = wide ? CSourcesNarrowHost() : CSide();
        auto to = wide ? CSide() : CSourcesNarrowHost();
        uint32_t idx;
        if (from.Children().IndexOf(card, idx))
        {
            from.Children().RemoveAt(idx);
            to.Children().Append(card);
        }
        CSideCol().Width(wide ? GridLength{360, GridUnitType::Pixel} : GridLength{0, GridUnitType::Pixel});
    }

    fire_and_forget MainWindow::AddSourcesFromPicker(bool folder)
    {
        auto strong = get_strong();
        std::vector<fs::path> add;
        if (folder)
        {
            const fs::path dir{std::wstring(co_await lycan_app::PickFolderAsync(AppWindow().Id(), lycan_app::kPurposeSourceFolder, {}, lycan::Tr(L"main.choose_folder_add")))};
            if (!dir.empty()) add.push_back(dir);
        }
        else
        {
            auto files = co_await lycan_app::PickFilesAsync(AppWindow().Id(), lycan_app::kPurposeSourceFiles, true, false);
            for (auto const& f : files) add.emplace_back(std::wstring(f));
        }
        for (auto& p : add)
            if (std::find(cSources_.begin(), cSources_.end(), p) == cSources_.end()) cSources_.push_back(std::move(p));
        OnSourcesChanged();
    }

    // Ctrl+V: files and folders copied in Explorer go into the sources of the new-archive page, or into the archive that is
    // shown (as a drop would). While a text box has the focus the paste is the text box's own, never taken over.
    bool MainWindow::TryPasteFiles()
    {
        using Windows::ApplicationModel::DataTransfer::StandardDataFormats;
        const bool compress = view_ == View::Compress;
        const bool archive = view_ == View::Archive && current_ && tree_ && !compact_ && current_->Caps().canModify;
        if (dialogOpen_ || (!compress && !archive) || FocusInTextInput()) return false;
        try
        {
            auto view = Windows::ApplicationModel::DataTransfer::Clipboard::GetContent();
            if (!view.Contains(StandardDataFormats::StorageItems())) return false;
        }
        catch (...) { return false; }
        PasteFilesAsync();
        return true;
    }

    bool MainWindow::ClipboardHasFiles()
    {
        try { return Windows::ApplicationModel::DataTransfer::Clipboard::GetContent().Contains(Windows::ApplicationModel::DataTransfer::StandardDataFormats::StorageItems()); }
        catch (...) { return false; }
    }

    fire_and_forget MainWindow::PasteFilesAsync(std::wstring into, bool targeted)
    {
        auto strong = get_strong();
        std::vector<fs::path> paths;
        try
        {
            auto items = co_await Windows::ApplicationModel::DataTransfer::Clipboard::GetContent().GetStorageItemsAsync();
            for (auto const& it : items)
                if (!it.Path().empty()) paths.emplace_back(std::wstring(it.Path()));
        }
        catch (...) {}
        if (paths.empty()) co_return;
        PlaySfx(L"drop");
        if (view_ == View::Compress)
        {
            for (auto& p : paths)
                if (std::find(cSources_.begin(), cSources_.end(), p) == cSources_.end()) cSources_.push_back(std::move(p));
            OnSourcesChanged();
        }
        else if (view_ == View::Archive && current_ && tree_ && !compact_ && current_->Caps().canModify)
            AddPathsToArchive(std::move(paths), targeted ? into : CurrentFolderPath());
    }

    // Entered from the right click "Add to archive", drag and drop, and "New archive" on the start page; when the page is open already new sources are merged into the list
    void MainWindow::StartCompress(std::vector<fs::path> inputs)
    {
        if (inputs.empty()) return;
        if (cPageActive_ && cConvert_)   // converting: only archives join the list
            inputs.erase(std::remove_if(inputs.begin(), inputs.end(), [](fs::path const& p) { return !IsOpenableArchive(p); }), inputs.end());
        if (inputs.empty()) return;
        if (cPageActive_)
        {
            for (auto& p : inputs)
                if (std::find(cSources_.begin(), cSources_.end(), p) == cSources_.end()) cSources_.push_back(std::move(p));
            if (view_ != View::Compress) ShowView(View::Compress);
            OnSourcesChanged();
            return;
        }
        const bool wasCompact = compact_;
        ExitCompact();
        cFromCompact_ = cFromCompact_ || wasCompact;
        cReturnView_ = view_ == View::Settings ? viewBeforeSettings_ : view_;
        if (cReturnView_ == View::Compress) cReturnView_ = View::Home;

        cSources_ = std::move(inputs);
        cOutDir_ = cSources_[0].parent_path();
        cNameEdited_ = false;
        cBuiltin_ = lycan::BuiltinPresets();
        cUser_ = lycan::LoadPresets(lycan::DefaultPresetsPath());

        std::wstring want = settings_.lastPreset;
        auto exists = [&](std::wstring const& n) {
            for (auto const* l : {&cBuiltin_, &cUser_}) for (auto const& p : *l) if (p.name == n) return true;
            return false;
        };
        if (want.empty() || !exists(want)) want = settings_.defaultFormat == lycan::ArchiveType::SevenZip ? L"General 7z" : L"General ZIP";
        for (auto const* l : {&cBuiltin_, &cUser_}) for (auto const& p : *l) if (p.name == want) cBase_ = p;

        SetCompressPassword(L"");
        CShowPw().IsChecked(true);  // show the password by default: a hidden password box cannot type Chinese and other characters
        cPageActive_ = true;
        FillPresetCombo(want);
        LoadPresetToUi(cBase_);
        cLoading_ = true;
        CAdvanced().IsExpanded(settings_.compressAdvancedOpen);
        CNameBox().Text(hstring{DefaultArchiveBaseName()});
        cLoading_ = false;
        RebuildSources();
        ShowView(View::Compress);
        UpdateCompressLayout();
        RefreshCompressUi();
    }

    void MainWindow::LeaveCompressPage()
    {
        cPageActive_ = false;
        SetCompressPassword(L"");
        if (cConvert_)
        {
            cConvert_ = false;
            cConvertPw_.clear();
            cConvertCp_.clear();
            cConvertNote_.clear();
            ApplyConvertMode();
        }
        if (cFromCompact_)
        {
            cFromCompact_ = false;
            ShowView(cReturnView_ == View::Archive && !current_ ? View::Home : cReturnView_);
            EnterCompact();
            CheckCompactAutoClose();
            return;
        }
        ShowView(cReturnView_ == View::Archive && !current_ ? View::Home : cReturnView_);
    }

    void MainWindow::OnCompressStart() { StartCompressAsync(); }

    fire_and_forget MainWindow::StartCompressAsync()
    {
        auto strong = get_strong();
        if (cSources_.empty() || !CStartButton().IsEnabled()) co_return;
        if (cConvert_) { StartConvertAsync(); co_return; }
        auto p = ReadCompressUi();
        std::wstring name = TrimW(std::wstring{CNameBox().Text()});
        const int fi = IndexOf(kFormatOrder, p.type);
        fs::path output = cOutDir_ / (name + kFormatExt[fi]);

        lycan::CompressOptions co;
        co.tempDir = lycan_app::ScratchOverride();
        lycan::ApplyPreset(p, co);
        co.skipThumbsDb = settings_.skipThumbsDb;
        co.password = CompressPassword();
        if (p.type != lycan::ArchiveType::Zip && p.type != lycan::ArchiveType::SevenZip) co.password.clear();
        co.encryptFileNames = p.encryptFileNames && !co.password.empty();
        co.isoLabel = TrimW(std::wstring{CIsoLabel().Text()});
        co.isoKeepTimes = CIsoKeepTimes().IsOn();

        // the file exists already: a new archive under another name, replace it, or add to it (the update modes)
        {
            std::error_code ec;
            if (fs::exists(output, ec))
            {
                const bool canUpdate = p.type == lycan::ArchiveType::Zip || p.type == lycan::ArchiveType::SevenZip || p.type == lycan::ArchiveType::Tar;
                const std::vector<fs::path> sources = cSources_;
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
                    std::vector<lycan::UpdateOp> ops = {lycan::UpdateOp::Add(L"", sources)};
                    if (uo.mode == lycan::UpdateMode::Synchronize && !co_await ConfirmSyncAsync(output, ops, uo)) co_return;
                    {
                        std::lock_guard lk(metaM_);
                        queue_.EnqueueUpdate(output, std::move(ops), uo);
                    }
                    settings_.lastPreset = cBase_.name;
                    SaveSettings();
                    LeaveCompressPage();
                    co_return;
                }
            }
        }

        {
            std::lock_guard lk(metaM_);
            uint64_t id = queue_.EnqueueCompress(output, cSources_, co);
            CompressMeta m;
            m.password = co.password;
            m.sources = cSources_;
            m.testAfter = CTestAfter().IsOn();
            m.sumsArchive = CSumsArchive().IsOn();
            m.sumsFiles = CSumsFiles().IsOn();
            compressMeta_[id] = std::move(m);
        }
        settings_.testAfterCompress = CTestAfter().IsOn();
        settings_.lastPreset = cBase_.name;
        SaveSettings();
        LeaveCompressPage();
    }

    // After a successful compression with nothing skipped, move the sources to the recycle bin (when the output file is inside a source folder, that source is not touched)
    void MainWindow::FinishCompressTask(lycan::TaskInfo const& info)
    {
        if (info.state == lycan::TaskState::Completed && info.compress.ok() && settings_.revealAfterCompress && !info.compress.outputs.empty())
        {
            // show the new file selected in Explorer
            if (PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(info.compress.outputs.front().c_str()))
            {
                SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
                ILFree(pidl);
            }
        }
        if (info.state != lycan::TaskState::Completed || !info.compress.ok() || info.compress.outputs.empty()) return;
        auto row = taskById_.find(info.id);
        auto self = row != taskById_.end() ? get_self<implementation::TaskRow>(row->second) : nullptr;
        bool haveSources = false, testAfter = false, sumsArchive = false, sumsFiles = false;
        std::vector<fs::path> sumSources;
        {
            std::lock_guard lk(metaM_);
            auto it = compressMeta_.find(info.id);
            if (it != compressMeta_.end())
            {
                it->second.output = info.compress.outputs.front();
                haveSources = !it->second.sources.empty();
                testAfter = it->second.testAfter;
                sumsArchive = it->second.sumsArchive;
                sumsFiles = it->second.sumsFiles;
                if (sumsArchive || sumsFiles) sumSources = it->second.sources;
            }
        }
        if (self)
        {
            self->SetTestable(true);
            // nothing is offered for deleting when something was skipped: the archive is not a complete copy
            const bool complete = info.compress.skipped.empty();
            self->SetDeletable(haveSources && complete, hstring{lycan::Tr(L"main.delete_originals")});
            if (haveSources && !complete) self->SetNote(lycan::Tr(L"main.some_items_were_skipped_so"));
        }
        if (testAfter) StartFollowupTest(info.id);
        if ((sumsArchive || sumsFiles) && !sumSources.empty())
            WriteSumsAfterCompressAsync(info.id, info.compress.outputs, std::move(sumSources), sumsArchive, sumsFiles);
    }

    // The checksum files next to a new archive (settings "write a checksum file ..."): read after the archive is complete, on a thread of their own
    fire_and_forget MainWindow::WriteSumsAfterCompressAsync(uint64_t cardId, std::vector<fs::path> outputs, std::vector<fs::path> sources, bool archiveSum, bool filesSum)
    {
        auto strong = get_strong();
        OpGuard guard{this};   // the compact window stays until the files are written
        lycan::SumWriteResult res;
        bool failed = false;
        co_await resume_background();
        try { res = lycan::WriteArchiveSums(outputs.front(), sources, archiveSum, filesSum, {}, outputs); }
        catch (...) { failed = true; }
        co_await resume_ui{dispatcher_};
        auto row = taskById_.find(cardId);
        if (row == taskById_.end()) co_return;
        auto self = get_self<implementation::TaskRow>(row->second);
        std::wstring note;
        if (failed) note = lycan::Tr(L"sums.card_note_failed");
        else
        {
            std::wstring names;
            for (auto const& w : res.written) names += (names.empty() ? L"" : L", ") + w.filename().wstring();
            if (!names.empty()) note = lycan::Tf(L"sums.card_note_written", names);
        }
        if (!note.empty()) self->SetNote(hstring{note});
    }

    // The integrity test of a finished compression runs as its own queue task, but is shown inside the card of the
    // compression: the card shows the test's progress and returns to its finished state (with the buttons) afterwards.
    void MainWindow::StartFollowupTest(uint64_t cardId)
    {
        auto row = taskById_.find(cardId);
        if (row == taskById_.end() || testOf_.count(cardId)) return;
        fs::path output;
        std::wstring password;
        {
            std::lock_guard lk(metaM_);
            auto it = compressMeta_.find(cardId);
            if (it == compressMeta_.end() || it->second.output.empty()) return;
            output = it->second.output;
            password = it->second.password;
        }
        lycan::OpenOptions oo;
        oo.tempDir = lycan_app::ScratchOverride();
        if (!password.empty()) oo.password = [password](std::wstring& out) { out = password; return true; };
        uint64_t testId;
        {
            std::lock_guard lk(metaM_);
            testId = queue_.EnqueueTest(output, oo);
            testFor_[testId] = cardId;
            testOf_[cardId] = testId;
        }
        get_self<implementation::TaskRow>(row->second)->BeginTest();
    }

    void MainWindow::OnFollowupTestUpdate(uint64_t cardId, lycan::TaskInfo const& info)
    {
        auto it = taskById_.find(cardId);
        if (it == taskById_.end()) { testFor_.erase(info.id); testOf_.erase(cardId); return; }
        auto* row = get_self<implementation::TaskRow>(it->second);
        if (!info.Finished())
        {
            row->Apply(info);
            NotifyStateChanged(false);
            UpdateTaskbar(nullptr);
            return;
        }
        testFor_.erase(info.id);
        testOf_.erase(cardId);
        std::wstring note;
        int state = 0;
        if (info.state == lycan::TaskState::Completed)
        {
            state = 1;
            note = lycan::Tr(L"main.integrity_test_passed");
            if (!info.extract.warnings.empty()) note += lycan::Tf(L"main.data_after_end_compressed_stream", info.extract.warnings.size());
        }
        else if (info.state == lycan::TaskState::Failed)
        {
            state = 2;
            note = lycan::Tf(L"main.integrity_test_failed", (info.extract.errors.empty() ? info.error : info.extract.errors.front()).Text());
        }
        else note = lycan::Tr(L"main.integrity_test_cancelled");
        row->EndTest(hstring{note});
        if (state)
        {
            std::lock_guard lk(metaM_);
            auto m = compressMeta_.find(cardId);
            if (m != compressMeta_.end()) m->second.testState = state;
        }
        if (state == 2) PlaySfx(L"error"); else if (state == 1) PlaySfx(L"done");
        UpdateTaskbar(&info);
        NotifyStateChanged(true);
        UpdateTaskSummary();
        CheckCompactAutoClose();
    }

    void MainWindow::OnTaskTestClick(Windows::Foundation::IInspectable const& s, RoutedEventArgs const&) { StartFollowupTest(TagId(s)); }
    // ---------------------------------------------------------------- Repacking (DESIGN.md "重新封裝")

    // "Convert the archive format": the new archive page with the archives as its sources. The format, the template, the level
    // and the volumes are chosen as for a new archive; the password is kept by default.
    void MainWindow::OpenConvertPage(std::vector<fs::path> archives)
    {
        if (archives.empty()) return;
        if (cPageActive_ && cConvert_)
        {
            StartCompress(std::move(archives));   // merged into the list
            ApplyConvertMode();
            return;
        }
        if (cPageActive_) LeaveCompressPage();
        cConvert_ = true;
        StartCompress(std::move(archives));
        CConvertPwCombo().SelectedIndex(0);
        ApplyConvertMode();
    }

    void MainWindow::ApplyConvertMode()
    {
        CTitle().Text(lycan::Tr(cConvert_ ? L"main.convert_archive_title" : L"main.new_archive"));
        const hstring start{lycan::Tr(cConvert_ ? L"main.start_converting" : L"main.start_compressing")};
        CStartButton().Content(box_value(start));
        Automation::AutomationProperties::SetName(CStartButton(), start);
        const auto vis = [](bool v) { return v ? Visibility::Visible : Visibility::Collapsed; };
        CConvertPwCombo().Visibility(vis(cConvert_));
        CAddFiles().Visibility(vis(!cConvert_));
        CAddFolder().Visibility(vis(!cConvert_));
        CPasteHint().Visibility(vis(!cConvert_));
        // several archives: each keeps its own name
        CNameBox().Visibility(vis(!cConvert_ || cSources_.size() <= 1));
        if (cConvert_ && cSources_.size() <= 1 && !cNameEdited_)
        {
            cLoading_ = true;
            CNameBox().Text(hstring{DefaultArchiveBaseName()});
            cLoading_ = false;
        }
        RefreshCompressUi();
    }

    // The password of an archive to convert: none needed (empty), this run's / a saved one, or asked. Cancelled: convertPwCancelled_
    Windows::Foundation::IAsyncOperation<hstring> MainWindow::SourcePasswordAsync(fs::path archive)
    {
        auto strong = get_strong();
        convertPwCancelled_ = false;
        int need = 0;   // 0 none, 1 the names are encrypted, 2 the content is
        std::shared_ptr<lycan::ArchiveSource> src;
        co_await resume_background();
        try
        {
            lycan::OpenOptions oo;
            oo.tempDir = lycan_app::ScratchOverride();
            src = lycan::ArchiveSource::Open(archive, oo);
            need = src->Caps().encrypted ? 2 : 0;
        }
        catch (lycan::Error const& e) { need = e.cancelled() || e.wrongPassword() ? 1 : 0; }
        catch (...) {}
        co_await resume_ui{dispatcher_};
        if (need == 0) co_return hstring{};
        std::function<bool(std::wstring const&)> works;
        if (need == 2) works = [src](std::wstring const& pw) { return src->CheckPassword(pw); };
        else works = [archive](std::wstring const& pw) {
            lycan::OpenOptions oo;
            oo.tempDir = lycan_app::ScratchOverride();
            oo.password = [pw](std::wstring& out) { out = pw; return true; };
            try { lycan::ArchiveSource::Open(archive, oo); return true; } catch (...) { return false; }
        };
        if (auto known = co_await TryKnownPasswordsAsync(works); !known.empty()) co_return known;
        bool wrong = false;
        for (;;)
        {
            auto pw = co_await AskPasswordAsync(hstring{archive.filename().wstring()}, wrong, works);
            if (pw.empty()) { convertPwCancelled_ = true; co_return hstring{}; }
            co_await resume_background();
            const bool ok = works(std::wstring(pw));
            co_await resume_ui{dispatcher_};
            if (ok) { PasswordWorked(std::wstring(pw)); co_return pw; }
            wrong = true;
        }
    }

    fire_and_forget MainWindow::StartConvertAsync()
    {
        auto strong = get_strong();
        OpGuard guard{this};
        auto p = ReadCompressUi();
        const int fi = IndexOf(kFormatOrder, p.type);
        const bool canEncrypt = p.type == lycan::ArchiveType::Zip || p.type == lycan::ArchiveType::SevenZip;
        const int pwMode = CConvertPwCombo().SelectedIndex();   // 0 keep, 1 none, 2 new
        const std::wstring newPw = CompressPassword();
        lycan::CompressOptions base;
        base.tempDir = lycan_app::ScratchOverride();
        lycan::ApplyPreset(p, base);
        base.isoLabel = TrimW(std::wstring{CIsoLabel().Text()});
        base.isoKeepTimes = CIsoKeepTimes().IsOn();
        const auto sources = cSources_;
        const bool single = sources.size() == 1;
        const std::wstring name = TrimW(std::wstring{CNameBox().Text()});
        const fs::path chosenDir = cOutDir_;
        const bool dirChanged = !sources.empty() && cOutDir_ != sources[0].parent_path();   // otherwise each one goes next to its archive
        const auto known = cConvertPw_;
        const auto codepages = cConvertCp_;
        settings_.lastPreset = cBase_.name;
        SaveSettings();
        LeaveCompressPage();
        for (auto const& archive : sources)
        {
            std::wstring sourcePw;
            if (auto k = known.find(archive); k != known.end()) sourcePw = k->second;
            else
            {
                sourcePw = std::wstring(co_await SourcePasswordAsync(archive));
                if (convertPwCancelled_) continue;   // this one is left out
            }
            lycan::CompressOptions co = base;
            co.password = !canEncrypt ? L"" : pwMode == 0 ? sourcePw : pwMode == 2 ? newPw : L"";
            co.encryptFileNames = p.encryptFileNames && !co.password.empty();
            const fs::path dir = single || dirChanged ? chosenDir : archive.parent_path();
            fs::path output = dir / ((single ? name : lycan::ArchiveBaseName(archive)) + kFormatExt[fi]);
            std::error_code ec;
            if (fs::exists(output, ec)) output = UniquePath(output);
            lycan::OpenOptions oo;
            oo.tempDir = lycan_app::ScratchOverride();
            if (auto c = codepages.find(archive); c != codepages.end()) oo.nameCodepage = c->second;
            if (!sourcePw.empty()) oo.password = [sourcePw](std::wstring& out) { out = sourcePw; return true; };
            lycan::diag::Info(L"convert {} to {}", lycan::diag::Name(archive), kFormatExt[fi]);
            std::lock_guard lk(metaM_);
            const uint64_t id = queue_.EnqueueRepack(archive, output, co, oo, false, false, true);
            CompressMeta m;
            m.password = co.password;
            m.sources = {archive};
            m.output = output;
            m.convert = true;
            compressMeta_[id] = std::move(m);
        }
    }

    // A conversion ended: the card offers to delete the original (with a warning when the new archive was not tested or failed);
    // the removal of the password (in place) opens the archive again
    void MainWindow::FinishRepackTask(lycan::TaskInfo const& info)
    {
        TaskMeta meta;
        bool inPlace = false;
        {
            std::lock_guard lk(metaM_);
            auto m = taskMeta_.find(info.id);
            if (m != taskMeta_.end() && !m->second.handled) { m->second.handled = true; meta = m->second; inPlace = true; }
        }
        if (inPlace)
        {
            if (updateTask_ == info.id) updateTask_ = 0;
            if (current_ && archiveLock_ == INVALID_HANDLE_VALUE) LockArchiveFile(currentPath_);
            if (info.state == lycan::TaskState::Cancelled) return;
            if (info.state != lycan::TaskState::Completed)
            {
                ShowMessage(lycan::Tr(L"main.archive_could_not_be_changed"), hstring{info.error.Text()});
                return;
            }
            if (!current_ || currentPath_ != meta.archive) return;
            password_.clear();
            passwordVerified_ = false;
            openNote_ = lycan::Tr(L"main.password_removed");
            restoreFor_ = meta.archive;
            restoreFolder_ = meta.restoreFolder;
            restoreSelect_ = meta.selectPaths;
            keptView_ = meta.keep;
            OpenArchiveAsync(currentPath_, currentCp_, L"");
            return;
        }
        if (info.state == lycan::TaskState::Cancelled) return;
        if (info.repack.outputs.empty() || !(info.state == lycan::TaskState::Completed || info.repack.tested)) return;
        const int testState = !info.repack.tested ? 0 : info.repack.ok() ? 1 : 2;
        bool have = false;
        {
            std::lock_guard lk(metaM_);
            auto it = compressMeta_.find(info.id);
            if (it != compressMeta_.end()) { it->second.output = info.repack.output; it->second.testState = testState; have = true; }
        }
        auto row = taskById_.find(info.id);
        auto self = row != taskById_.end() ? get_self<implementation::TaskRow>(row->second) : nullptr;
        if (self && have)
        {
            // items left out: the new archive is not a complete copy, so the original stays
            const bool complete = info.repack.skipped.empty();
            self->SetDeletable(complete, hstring{lycan::Tr(L"main.delete_original_archive")});
            if (!complete) self->SetNote(lycan::Tr(L"main.items_left_out_original_kept"));
        }
        if (info.state == lycan::TaskState::Completed && settings_.revealAfterCompress)
        {
            if (PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(info.repack.output.c_str()))
            {
                SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
                ILFree(pidl);
            }
        }
    }

    fire_and_forget MainWindow::OnConvertClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        auto strong = get_strong();
        if (!current_) co_return;
        if (current_->Caps().encrypted && !co_await EnsurePasswordAsync()) co_return;
        if (!current_) co_return;
        const fs::path archive = currentPath_;
        cConvertPw_.clear();
        cConvertCp_.clear();
        cConvertPw_[archive] = password_;   // empty: none needed (encrypted names were asked while opening)
        cConvertCp_[archive] = currentCp_;
        const auto ne = current_->NameEncoding();
        const std::wstring note = ne.applicable ? lycan::Tf(L"main.convert_names_note", lycan::CodepageName(ne.codepage)) : std::wstring();
        OpenConvertPage({archive});
        cConvertNote_ = note;
        RefreshCompressUi();
    }

    // Removes the password: packed again without it, tested, and only then swapped in (no question: the archive is kept when anything fails)
    fire_and_forget MainWindow::OnRemovePasswordClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        auto strong = get_strong();
        if (!CanModifyNow(true)) co_return;
        if (!co_await EnsurePasswordAsync()) co_return;
        if (!CanModifyNow(false)) co_return;
        lycan::ArchiveType type;
        if (!lycan::WritableTypeOf(*current_, type)) co_return;
        const fs::path archive = currentPath_;
        lycan::CompressOptions co;
        co.type = type;
        co.tempDir = lycan_app::ScratchOverride();
        const lycan::OpenOptions oo = MakeOpenOptions();
        lycan_app::CloseImagePreview(this);
        lycan_app::CloseAudioPreview(this);
        TaskMeta meta;
        meta.archive = archive;
        meta.isUpdate = true;
        meta.restoreFolder = CurrentFolderPath();
        meta.keep.valid = true;
        meta.keep.flat = flat_;
        meta.keep.mode = viewMode_;
        meta.keep.sort = sortKey_;
        meta.keep.asc = sortAsc_;
        meta.keep.tree = treeWanted_;
        meta.keep.query = query_;
        lycan::diag::Info(L"remove the password of {}", lycan::diag::Name(archive));
        std::lock_guard lk(metaM_);
        UnlockArchiveFile();   // the archive is replaced by a new file
        const uint64_t id = queue_.EnqueueRepack(archive, {}, co, oo, true, false, true);
        updateTask_ = id;
        taskMeta_[id] = std::move(meta);
    }

    // Changing a tar whose names have no encoding mark stores all of them in UTF-8 as they are shown now: say so, with some of them
    Windows::Foundation::IAsyncOperation<bool> MainWindow::ConfirmTarNamesAsync()
    {
        auto strong = get_strong();
        if (!current_) co_return false;
        auto dlg = MakeDialog(lycan::Tr(L"main.tar_names_title"), L"");
        StackPanel sp;
        sp.Spacing(12);
        sp.Width(460);
        TextBlock intro;
        intro.Text(lycan::Tr(L"main.tar_names_text"));
        intro.TextWrapping(TextWrapping::Wrap);
        sp.Children().Append(intro);
        StackPanel names;
        names.Spacing(2);
        int shown = 0;
        for (auto const& e : current_->Entries())
        {
            if (std::all_of(e.path.begin(), e.path.end(), [](wchar_t c) { return c < 0x80; })) continue;
            TextBlock t;
            t.Text(hstring{e.path});
            t.TextTrimming(TextTrimming::CharacterEllipsis);
            names.Children().Append(t);
            if (++shown == 8) break;
        }
        sp.Children().Append(MakeCardBox(names));
        dlg.Content(sp);
        dlg.PrimaryButtonText(lycan::Tr(L"main.resume"));
        dlg.CloseButtonText(lycan::Tr(L"main.cancel"));
        dlg.DefaultButton(ContentDialogButton::Close);
        co_return co_await ShowDialogAsync(dlg) == ContentDialogResult::Primary;
    }

    // Upgrades an old zip / tar: the names are written again in UTF-8 (the data is copied as it is), tested, then swapped in.
    // The names are stored as they are decoded now, for good: a dialog shows them first, with the encoding to choose there.
    fire_and_forget MainWindow::OnUpgradeClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        auto strong = get_strong();
        if (!current_ || !CanModifyNow(true)) co_return;
        const fs::path archive = currentPath_;
        const auto ne = current_->NameEncoding();
        // a second copy of the archive for the preview: its names are decoded again without touching the view
        std::shared_ptr<lycan::ArchiveSource> preview;
        lycan::OpenOptions oo = MakeOpenOptions();
        oo.nameCodepage = 0;
        SetBusy(true);
        co_await resume_background();
        try { preview = lycan::ArchiveSource::Open(archive, oo); } catch (...) {}
        co_await resume_ui{dispatcher_};
        SetBusy(false);
        if (!preview || !current_ || currentPath_ != archive) co_return;
        const unsigned detected = preview->NameEncoding().codepage;

        auto dlg = MakeDialog(lycan::Tr(L"main.upgrade_archive"), L"");
        StackPanel sp;
        sp.Spacing(12);
        sp.Width(460);
        TextBlock intro;
        intro.Text(lycan::Tr(L"main.upgrade_confirm_text"));
        intro.TextWrapping(TextWrapping::Wrap);
        sp.Children().Append(intro);
        if (ne.autoDetected && ne.ambiguous)
        {
            TextBlock warn;
            warn.Text(lycan::Tr(L"main.upgrade_encoding_uncertain"));
            warn.TextWrapping(TextWrapping::Wrap);
            warn.Foreground(Microsoft::UI::Xaml::Media::SolidColorBrush{lycan_app::StatusColor(Windows::UI::Color{255, 255, 200, 90})});
            sp.Children().Append(warn);
        }
        ComboBox combo;
        combo.Header(box_value(lycan::Tr(L"main.filename_encoding")));
        combo.MinWidth(240);
        Automation::AutomationProperties::SetName(combo, lycan::Tr(L"main.filename_encoding"));
        for (auto const& o : EncodingCombo().Items())
        {
            auto src = o.try_as<ComboBoxItem>();
            if (!src) continue;
            ComboBoxItem it;
            it.Content(src.Content());
            it.Tag(src.Tag());
            combo.Items().Append(it);
        }
        combo.SelectedIndex(EncodingCombo().SelectedIndex());
        sp.Children().Append(combo);
        StackPanel names;
        names.Spacing(2);
        sp.Children().Append(MakeCardBox(names));
        // the names with characters outside ASCII (the ones the encoding decides), at most 8, decoded with the chosen code page
        auto fill = [preview, names, detected](unsigned cp) {
            preview->SetNameCodepage(cp ? cp : detected);
            names.Children().Clear();
            int shown = 0;
            for (auto const& e : preview->Entries())
            {
                if (std::all_of(e.path.begin(), e.path.end(), [](wchar_t c) { return c < 0x80; })) continue;
                TextBlock t;
                t.Text(hstring{e.path});
                t.TextTrimming(TextTrimming::CharacterEllipsis);
                names.Children().Append(t);
                if (++shown == 8) break;
            }
        };
        auto tagOf = [](ComboBox const& c) {
            auto it = c.SelectedItem().try_as<ComboBoxItem>();
            return it ? static_cast<unsigned>(unbox_value<int32_t>(it.Tag())) : 0u;
        };
        fill(tagOf(combo));
        combo.SelectionChanged([fill, tagOf](Windows::Foundation::IInspectable const& s, auto&&) { fill(tagOf(s.as<ComboBox>())); });
        dlg.Content(sp);
        dlg.PrimaryButtonText(lycan::Tr(L"main.upgrade_button"));
        dlg.CloseButtonText(lycan::Tr(L"main.cancel"));
        dlg.DefaultButton(ContentDialogButton::Close);   // it cannot be undone
        if (co_await ShowDialogAsync(dlg) != ContentDialogResult::Primary) co_return;
        if (!current_ || currentPath_ != archive) co_return;
        // the encoding chosen in the dialog is the one of the window too (the names are written with it)
        const unsigned cp = tagOf(combo);
        if (cp != currentCp_)
        {
            currentCp_ = cp;
            updatingEncoding_ = true;
            EncodingCombo().SelectedIndex(combo.SelectedIndex());
            updatingEncoding_ = false;
        }
        TaskMeta meta;
        meta.upgrade = true;
        meta.restoreFolder = CurrentFolderPath();
        RunUpdateAsync({}, std::move(meta), lycan::UpdateMode::AddReplace, true);
    }

    void MainWindow::OnTaskDeleteClick(Windows::Foundation::IInspectable const& s, RoutedEventArgs const&) { DeleteOriginalsAsync(TagId(s)); }

    // "Delete the originals" (after a compression) / "delete the archive" (after an extraction): always an explicit
    // click and a warning that says what will happen; the deletion itself is done by the shell.
    fire_and_forget MainWindow::DeleteOriginalsAsync(uint64_t cardId)
    {
        auto strong = get_strong();
        std::vector<fs::path> items;
        fs::path output;
        int testState = 0;
        bool isCompress = false, convert = false;
        {
            std::lock_guard lk(metaM_);
            auto c = compressMeta_.find(cardId);
            if (c != compressMeta_.end())
            {
                items = c->second.sources;
                output = c->second.output;
                testState = c->second.testState;
                convert = c->second.convert;
                isCompress = true;
            }
            else
            {
                auto m = taskMeta_.find(cardId);
                if (m != taskMeta_.end() && m->second.deletable) items = {m->second.archive};
            }
        }
        // only what still exists; a source that contains the new archive stays (it is not a separate original)
        std::vector<fs::path> todo;
        bool keptOne = false;
        for (auto const& p : items)
        {
            std::error_code ec;
            if (!fs::exists(p, ec)) continue;
            if (isCompress && !output.empty())
            {
                auto rel = output.lexically_relative(p);
                if (!rel.empty() && *rel.begin() != L"..") { keptOne = true; continue; }
            }
            todo.push_back(p);
        }
        auto rowIt = taskById_.find(cardId);
        auto* row = rowIt != taskById_.end() ? get_self<implementation::TaskRow>(rowIt->second) : nullptr;
        if (todo.empty())
        {
            ShowMessage(lycan::Tr(L"main.nothing_delete"), lycan::Tr(L"main.originals_no_longer_exist_they"));
            if (row) row->SetDeletable(false, {});
            co_return;
        }
        if (isCompress)
        {
            std::error_code ec;
            if (output.empty() || !fs::exists(output, ec) || fs::file_size(output, ec) == 0)
            {
                ShowMessage(lycan::Tr(L"main.new_archive_was_not_found"), lycan::Tr(L"main.new_archive_gone_so_originals_will"));
                co_return;
            }
        }
        bool recycle = true;
        for (auto const& p : todo) recycle = recycle && CanRecycle(p);

        auto dlg = MakeDialog(isCompress && !convert ? lycan::Tr(L"main.delete_originals_title") : lycan::Tr(L"main.delete_original_archive_title"), L"\uE74D");
        StackPanel sp;
        sp.Spacing(10);
        sp.MaxWidth(440);
        auto add = [&](std::wstring const& text, bool strong_) {
            TextBlock t;
            t.Text(hstring{text});
            t.TextWrapping(TextWrapping::Wrap);
            if (strong_) t.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
            sp.Children().Append(t);
        };
        if (recycle)
            add(lycan::Tf(L"main.move_recycle_bin_you_can_restore", todo.size()), false);
        else
            add(lycan::Tf(L"main.drive_cannot_use_recycle_bin", todo.size()), true);
        std::wstring list;
        for (size_t i = 0; i < todo.size() && i < 5; ++i) list += L"•  " + todo[i].filename().wstring() + L"\n";
        if (todo.size() > 5) list += lycan::Tf(L"main.more", todo.size() - 5);
        {
            TextBlock t;
            t.Text(hstring{list});
            t.Opacity(0.8);
            t.TextWrapping(TextWrapping::Wrap);
            sp.Children().Append(t);
        }
        if (isCompress)
        {
            if (testState == 1) add(lycan::Tr(L"main.new_archive_passed_integrity_test"), false);
            else if (testState == 2) add(lycan::Tr(L"main.integrity_test_new_archive_failed"), true);
            else add(lycan::Tr(L"main.new_archive_has_not_been_tested"), true);
            if (keptOne) add(lycan::Tr(L"main.new_archive_inside_one_source"), false);
        }
        dlg.Content(sp);
        dlg.PrimaryButtonText(recycle ? lycan::Tr(L"main.move_recycle_bin") : lycan::Tr(L"main.delete_permanently"));
        dlg.CloseButtonText(lycan::Tr(L"main.cancel"));
        dlg.DefaultButton(ContentDialogButton::Close);
        if (co_await ShowDialogAsync(dlg) != ContentDialogResult::Primary) co_return;

        int deleted = 0;
        const bool shown = current_ && !todo.empty() && currentPath_ == todo.front();
        if (shown) UnlockArchiveFile();
        const int failed = ShellDelete(Hwnd(), todo, recycle, deleted);
        if (shown && failed != 0 && current_) LockArchiveFile(currentPath_);
        if (row)
        {
            if (failed == 0)
            {
                row->SetNote(hstring{recycle ? lycan::Tf(L"main.moved_recycle_bin", deleted) : lycan::Tf(L"main.status_deleted_items", deleted)});
                row->SetDeletable(false, {});
            }
            else
                row->SetNote(hstring{lycan::Tf(L"main.not_deleted", failed)});
        }
        if (!isCompress)
        {
            std::lock_guard lk(metaM_);
            auto m = taskMeta_.find(cardId);
            if (m != taskMeta_.end() && failed == 0) m->second.deletable = false;
        }
        if ((!isCompress || convert) && failed == 0 && current_ && !todo.empty() && currentPath_ == todo.front()) ShowHome();
    }

    Windows::Foundation::IAsyncOperation<hstring> MainWindow::AskNameAsync(hstring title, hstring initial)
    {
        auto strong = get_strong();
        auto dlg = MakeDialog(title, L"");
        TextBox box;
        box.Text(initial);
        box.MaxLength(40);
        box.SelectAll();
        Automation::AutomationProperties::SetName(box, lycan::Tr(L"main.template_name"));
        StackPanel sp;
        sp.Spacing(8);
        sp.MinWidth(360);
        sp.Children().Append(box);
        sp.Children().Append(Caption(lycan::Tr(L"main.up_40_characters_no_allowed"), 0.65));
        dlg.Content(sp);
        dlg.PrimaryButtonText(lycan::Tr(L"main.ok"));
        dlg.CloseButtonText(lycan::Tr(L"main.cancel"));
        dlg.DefaultButton(ContentDialogButton::Primary);
        auto r = co_await ShowDialogAsync(dlg);
        co_return r == ContentDialogResult::Primary ? hstring{TrimW(std::wstring{box.Text()})} : hstring{};
    }

    bool MainWindow::PresetNameTaken(std::wstring const& name) const
    {
        for (auto const* l : {&cBuiltin_, &cUser_}) for (auto const& p : *l) if (p.name == name) return true;
        return false;
    }

    fire_and_forget MainWindow::OnPresetSaveNew()
    {
        auto strong = get_strong();
        std::wstring name{co_await AskNameAsync(lycan::Tr(L"main.save_as_new_template"), lycan::Tr(L"main.my_template"))};
        if (name.empty()) co_return;
        if (!lycan::IsValidPresetName(name)) { ShowMessage(lycan::Tr(L"main.invalid_name"), lycan::Tr(L"main.name_cannot_be_empty_contain_line")); co_return; }
        if (PresetNameTaken(name)) { ShowMessage(lycan::Tr(L"main.name_already_exists"), lycan::Tr(L"main.template_with_name_already_exists")); co_return; }
        auto cur = ReadCompressUi();
        cur.name = name;
        cUser_.push_back(cur);
        lycan::SavePresets(lycan::DefaultPresetsPath(), cUser_);
        cBase_ = cur;
        FillPresetCombo(name);
        cBaseNorm_ = cur;
        RefreshCompressUi();
    }

    void MainWindow::OnPresetUpdate()
    {
        if (cBase_.builtin) return;
        auto cur = ReadCompressUi();
        cur.name = cBase_.name;
        cur.inMenu = cBase_.inMenu;  // not part of what the page shows
        for (auto& u : cUser_) if (u.name == cur.name) u = cur;
        lycan::SavePresets(lycan::DefaultPresetsPath(), cUser_);
        cBase_ = cur;
        cBaseNorm_ = cur;
        RefreshCompressUi();
    }

    fire_and_forget MainWindow::OnPresetRename()
    {
        auto strong = get_strong();
        if (cBase_.builtin) co_return;
        std::wstring oldName = cBase_.name;
        std::wstring name{co_await AskNameAsync(lycan::Tr(L"main.rename_template"), hstring{oldName})};
        if (name.empty() || name == oldName) co_return;
        if (!lycan::IsValidPresetName(name)) { ShowMessage(lycan::Tr(L"main.invalid_name"), lycan::Tr(L"main.name_cannot_be_empty_contain_line")); co_return; }
        if (PresetNameTaken(name)) { ShowMessage(lycan::Tr(L"main.name_already_exists"), lycan::Tr(L"main.template_with_name_already_exists")); co_return; }
        for (auto& u : cUser_) if (u.name == oldName) u.name = name;
        lycan::SavePresets(lycan::DefaultPresetsPath(), cUser_);
        cBase_.name = name;
        FillPresetCombo(name);
        RefreshCompressUi();
    }

    fire_and_forget MainWindow::OnPresetDelete()
    {
        auto strong = get_strong();
        if (cBase_.builtin) co_return;
        auto dlg = MakeDialog(lycan::Tr(L"main.delete_template_title"), L"");
        TextBlock tb;
        tb.Text(hstring{lycan::Tf(L"main.delete_template_cannot_be_undone", cBase_.name)});
        tb.TextWrapping(TextWrapping::Wrap);
        tb.MaxWidth(420);
        dlg.Content(tb);
        dlg.PrimaryButtonText(lycan::Tr(L"main.delete"));
        dlg.CloseButtonText(lycan::Tr(L"main.cancel"));
        dlg.DefaultButton(ContentDialogButton::Close);
        if (co_await ShowDialogAsync(dlg) != ContentDialogResult::Primary) co_return;
        const std::wstring gone = cBase_.name;
        cUser_.erase(std::remove_if(cUser_.begin(), cUser_.end(), [&](auto const& u) { return u.name == gone; }), cUser_.end());
        lycan::SavePresets(lycan::DefaultPresetsPath(), cUser_);
        cBase_ = cBuiltin_.front();
        FillPresetCombo(cBase_.name);
        LoadPresetToUi(cBase_);
    }
}
