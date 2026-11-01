#include "pch.h"
// The main window: the password dialog, the master password and the Passwords tab of the settings.
#include "MainWindowShared.h"

namespace winrt::LycanZip::implementation
{
    // ---------------------------------------------------------------- Password

    // The dialog opens in the middle of a double click (the second press and release come after it): the list takes the focus back. So the
    // focus is put on the password box a moment after the dialog is there.
    static fire_and_forget FocusSoon(Microsoft::UI::Dispatching::DispatcherQueue dq, TextBox plain, PasswordBox hidden, CheckBox showPw)
    {
        co_await resume_after(150ms);
        co_await resume_ui{dq};
        if (showPw.IsChecked().Value()) { plain.Focus(FocusState::Programmatic); plain.SelectAll(); }
        else hidden.Focus(FocusState::Programmatic);
    }

    // Asks for the password of an archive. When the saved list is locked by a master password, the dialog can unlock it: then the saved
    // passwords are tried (`works`), and the dialog comes back only when none of them fits.
    Windows::Foundation::IAsyncOperation<hstring> MainWindow::AskPasswordAsync(hstring name, bool previousWasWrong, std::function<bool(std::wstring const&)> works)
    {
        auto strong = get_strong();
        bool noneFits = false;
        for (;;)
        {
            askUnlocked_ = false;
            const bool canUnlock = works && settings_.autoTryPasswords;
            auto pw = co_await AskPasswordDialogAsync(name, previousWasWrong, canUnlock, noneFits);
            if (!askUnlocked_) co_return pw;
            askUnlocked_ = false;
            if (auto found = co_await TryKnownPasswordsAsync(works); !found.empty())
            {
                ShowStatusNote(lycan::Tr(L"main.used_saved_password"));
                co_return found;
            }
            noneFits = true;
            previousWasWrong = false;
        }
    }

    Windows::Foundation::IAsyncOperation<hstring> MainWindow::AskPasswordDialogAsync(hstring name, bool previousWasWrong, bool canUnlock, bool noneFits)
    {
        auto strong = get_strong();
        co_await resume_ui{dispatcher_};
        // Show the password in plain text by default: a hidden password box disables the input method, so Chinese and other characters could not be typed
        TextBox plain;
        plain.Header(box_value(lycan::Tr(L"main.password")));
        plain.MinWidth(320);
        Automation::AutomationProperties::SetName(plain, lycan::Tr(L"main.password"));
        PasswordBox hidden;
        hidden.Header(box_value(lycan::Tr(L"main.password")));
        hidden.MinWidth(320);
        hidden.PasswordRevealMode(PasswordRevealMode::Peek);
        hidden.Visibility(Visibility::Collapsed);
        Automation::AutomationProperties::SetName(hidden, lycan::Tr(L"main.password"));
        CheckBox showPw;
        showPw.Content(box_value(lycan::Tr(L"main.show_password_keep_checked_when")));
        showPw.IsChecked(true);
        Automation::AutomationProperties::SetName(showPw, lycan::Tr(L"main.show_password"));
        showPw.Click([plain, hidden, showPw](auto&&, auto&&) {
            const bool show = showPw.IsChecked().Value();
            if (show) plain.Text(hidden.Password()); else hidden.Password(plain.Text());
            plain.Visibility(show ? Visibility::Visible : Visibility::Collapsed);
            hidden.Visibility(show ? Visibility::Collapsed : Visibility::Visible);
        });

        auto dlg = MakeDialog(lycan::Tr(L"main.password_required"), L"\uE72E");
        StackPanel sp;
        sp.Spacing(12);
        sp.MinWidth(380);
        {
            StackPanel row;
            row.Orientation(Orientation::Horizontal);
            row.Spacing(10);
            row.Children().Append(MakeGlyph(L"\uE7B8", 20, BrushForCategory(FileCategory::Archive)));
            TextBlock tb;
            tb.Text(name);
            tb.VerticalAlignment(VerticalAlignment::Center);
            tb.TextTrimming(TextTrimming::CharacterEllipsis);
            row.Children().Append(tb);
            sp.Children().Append(MakeCardBox(row));
        }
        if (previousWasWrong || noneFits)
        {
            StackPanel warnRow;
            warnRow.Orientation(Orientation::Horizontal);
            warnRow.Spacing(8);
            auto red = Microsoft::UI::Xaml::Media::SolidColorBrush{lycan_app::StatusColor(Windows::UI::Color{255, 0xFF, 0x6B, 0x6B})};
            warnRow.Children().Append(MakeGlyph(L"\uE783", 15, red));
            TextBlock warn;
            warn.Text(lycan::Tr(previousWasWrong ? L"main.wrong_password_please_try_again" : L"main.no_saved_password_fits"));
            warn.Foreground(red);
            warnRow.Children().Append(warn);
            sp.Children().Append(warnRow);
        }
        sp.Children().Append(plain);
        sp.Children().Append(hidden);
        sp.Children().Append(showPw);
        // the password manager: keep it in passwords.dat (encrypted for this Windows account) once it proves right
        CheckBox remember;
        remember.Content(box_value(lycan::Tr(L"main.remember_this_password")));
        remember.IsChecked(settings_.rememberPasswordDefault);
        Automation::AutomationProperties::SetName(remember, lycan::Tr(L"main.remember_this_password"));
        if (lycan_app::PasswordVault::Get().State() == lycan::PasswordFileState::Unreadable) remember.IsEnabled(false);   // the saved list cannot be read: nothing can be added
        sp.Children().Append(remember);
        // The saved list is locked by the master password: it can be unlocked here, to try the saved passwords, or to add this one
        const bool locked = lycan_app::PasswordVault::Get().Locked();
        PasswordBox master;
        TextBlock masterNote;
        if (locked)
        {
            StackPanel area;
            area.Spacing(8);
            TextBlock hint;
            hint.Text(lycan::Tr(canUnlock ? L"main.unlock_to_try_saved" : L"main.master_to_remember"));
            hint.TextWrapping(TextWrapping::Wrap);
            hint.MaxWidth(420);
            hint.Opacity(0.8);
            area.Children().Append(hint);
            StackPanel row;
            row.Orientation(Orientation::Horizontal);
            row.Spacing(8);
            master.Header(box_value(lycan::Tr(L"main.master_password")));
            master.MinWidth(240);
            master.PasswordRevealMode(PasswordRevealMode::Peek);
            Automation::AutomationProperties::SetName(master, lycan::Tr(L"main.master_password"));
            row.Children().Append(master);
            if (canUnlock)
            {
                Button unlock;
                unlock.Content(box_value(lycan::Tr(L"main.unlock_and_try")));
                unlock.VerticalAlignment(VerticalAlignment::Bottom);
                Automation::AutomationProperties::SetName(unlock, lycan::Tr(L"main.unlock_and_try"));
                unlock.Click([weak = get_weak(), dlg, master, masterNote, unlock](auto&&, auto&&) {
                    if (auto s = weak.get()) s->UnlockFromPasswordDialogAsync(dlg, master, masterNote, unlock);
                });
                row.Children().Append(unlock);
            }
            area.Children().Append(row);
            masterNote.Foreground(Microsoft::UI::Xaml::Media::SolidColorBrush{lycan_app::StatusColor(Windows::UI::Color{255, 0xFF, 0x6B, 0x6B})});
            masterNote.TextWrapping(TextWrapping::Wrap);
            masterNote.Visibility(Visibility::Collapsed);
            area.Children().Append(masterNote);
            sp.Children().Append(MakeCardBox(area));
        }
        dlg.Content(sp);
        dlg.PrimaryButtonText(lycan::Tr(L"main.ok"));
        dlg.CloseButtonText(lycan::Tr(L"main.cancel"));
        dlg.DefaultButton(ContentDialogButton::Primary);
        if (locked)
        {
            // "Remember this password" while locked: the list must be opened first (the new password is encrypted with the master key)
            dlg.PrimaryButtonClick([weak = get_weak(), remember, master, masterNote](ContentDialog const&, ContentDialogButtonClickEventArgs const& args) {
                auto s = weak.get();
                if (!s || !remember.IsChecked().Value() || !lycan_app::PasswordVault::Get().Locked()) return;
                if (master.Password().empty())
                {
                    args.Cancel(true);
                    masterNote.Text(lycan::Tr(L"main.enter_master_to_remember"));
                    masterNote.Visibility(Visibility::Visible);
                    master.Focus(FocusState::Programmatic);
                    return;
                }
                s->UnlockToRememberAsync(args, args.GetDeferral(), master.Password(), masterNote);
            });
        }
        dlg.Opened([dq = dispatcher_, plain, hidden, showPw](auto&&, auto&&) { FocusSoon(dq, plain, hidden, showPw); });
        auto result = co_await ShowDialogAsync(dlg);
        if (result != ContentDialogResult::Primary) co_return hstring{};
        hstring pw = showPw.IsChecked().Value() ? plain.Text() : hidden.Password();
        askedPassword_ = std::wstring(pw);
        askedRemember_ = remember.IsChecked().Value();
        co_return pw;
    }

    // Before asking: the passwords of this run and the saved ones (settings), the most likely first, at most 50 or 5 seconds, tried in
    // the background. `works` tells whether a password is right (it is called on a background thread). Empty = none of them.
    Windows::Foundation::IAsyncOperation<hstring> MainWindow::TryKnownPasswordsAsync(std::function<bool(std::wstring const&)> works)
    {
        auto strong = get_strong();
        auto list = lycan_app::PasswordVault::Get().Candidates(settings_.rememberRunPasswords, settings_.autoTryPasswords, 50);
        if (list.empty()) co_return hstring{};
        SetBusy(true);
        co_await resume_background();
        std::wstring found;
        size_t tried = 0;
        const auto start = std::chrono::steady_clock::now();
        for (auto const& pw : list)
        {
            if (std::chrono::steady_clock::now() - start > std::chrono::seconds(5)) break;
            ++tried;
            bool ok = false;
            try { ok = works(pw); } catch (...) {}
            if (ok) { found = pw; break; }
        }
        co_await resume_ui{dispatcher_};
        SetBusy(false);
        lycan::diag::Info(L"known passwords: {} of {} tried, {}", tried, list.size(), found.empty() ? L"none fits" : L"one fits");   // (never the password itself)
        if (!found.empty()) lycan_app::PasswordVault::Get().Worked(found, settings_.rememberRunPasswords, false);
        co_return hstring{found};
    }

    // ---- The master password (DESIGN.md "密碼管理員", the third stage). Deriving a key takes about a quarter of a second, so it is
    // done in the background; the vault itself is used on the UI thread only.

    Windows::Foundation::IAsyncOperation<bool> MainWindow::UnlockVaultAsync(hstring master)
    {
        auto strong = get_strong();
        if (!lycan_app::PasswordVault::Get().Locked()) co_return true;
        std::string bytes = lycan_app::PasswordVault::Get().LockedBytes();
        std::wstring pw{master};
        SetBusy(true);
        co_await resume_background();
        std::vector<lycan::SavedPassword> items;
        lycan::MasterKey key;
        const bool ok = lycan::UnlockPasswords(bytes, pw, items, key);
        SecureZeroMemory(pw.data(), pw.size() * sizeof(wchar_t));
        co_await resume_ui{dispatcher_};
        SetBusy(false);
        lycan::diag::Info(L"saved passwords: {}", ok ? L"unlocked" : L"the master password was wrong");
        if (ok) lycan_app::PasswordVault::Get().Unlocked(std::move(items), std::move(key));
        co_return ok;
    }

    fire_and_forget MainWindow::UnlockFromPasswordDialogAsync(ContentDialog dlg, PasswordBox master, TextBlock note, Button button)
    {
        auto strong = get_strong();
        button.IsEnabled(false);
        const bool ok = co_await UnlockVaultAsync(master.Password());
        button.IsEnabled(true);
        if (!ok)
        {
            note.Text(lycan::Tr(L"main.wrong_master_password"));
            note.Visibility(Visibility::Visible);
            master.SelectAll();
            master.Focus(FocusState::Programmatic);
            co_return;
        }
        askUnlocked_ = true;   // AskPasswordAsync tries the saved passwords now
        dlg.Hide();
    }

    fire_and_forget MainWindow::UnlockToRememberAsync(ContentDialogButtonClickEventArgs args, ContentDialogButtonClickDeferral deferral, hstring master, TextBlock note)
    {
        auto strong = get_strong();
        if (!co_await UnlockVaultAsync(master))
        {
            args.Cancel(true);
            note.Text(lycan::Tr(L"main.wrong_master_password"));
            note.Visibility(Visibility::Visible);
        }
        deferral.Complete();
    }

    // A dialog with password boxes (`headers` are their string ids). newPassword: the last two are a new master password and its
    // repetition, and OK is enabled only when they match and are long enough.
    ContentDialog MainWindow::MakeMasterDialog(hstring title, std::vector<const wchar_t*> headers, bool newPassword, std::vector<PasswordBox>& boxes)
    {
        auto dlg = MakeDialog(title, L"");
        StackPanel sp;
        sp.Spacing(12);
        sp.Width(400);   // a fixed width and a line kept for the mismatch note: the dialog does not change its size while typing
        boxes.clear();
        for (auto id : headers)
        {
            PasswordBox b;
            b.Header(box_value(lycan::Tr(id)));
            b.PasswordRevealMode(PasswordRevealMode::Peek);
            Automation::AutomationProperties::SetName(b, lycan::Tr(id));
            sp.Children().Append(b);
            boxes.push_back(b);
        }
        TextBlock mismatchNote;
        mismatchNote.TextWrapping(TextWrapping::Wrap);
        mismatchNote.MinHeight(20);
        mismatchNote.Foreground(Microsoft::UI::Xaml::Media::SolidColorBrush{lycan_app::StatusColor(Windows::UI::Color{255, 0xFF, 0x6B, 0x6B})});
        TextBlock note;
        note.TextWrapping(TextWrapping::Wrap);
        note.Opacity(0.8);
        note.Text(lycan::Tr(L"main.master_password_rules"));
        if (newPassword)
        {
            sp.Children().Append(mismatchNote);
            sp.Children().Append(note);
        }
        dlg.Content(sp);
        dlg.PrimaryButtonText(lycan::Tr(L"main.ok"));
        dlg.CloseButtonText(lycan::Tr(L"main.cancel"));
        dlg.DefaultButton(ContentDialogButton::Primary);
        auto check = [dlg, boxes, newPassword, mismatchNote]() {
            bool ok = std::all_of(boxes.begin(), boxes.end(), [](auto const& b) { return !b.Password().empty(); });
            if (newPassword && boxes.size() >= 2)
            {
                const std::wstring a{boxes[boxes.size() - 2].Password()}, b{boxes.back().Password()};
                const bool mismatch = !b.empty() && a != b;
                ok = ok && a.size() >= lycan::kMinMasterLength && !mismatch;
                mismatchNote.Text(mismatch ? hstring{lycan::Tr(L"main.two_passwords_do_not_match")} : hstring{});
            }
            dlg.IsPrimaryButtonEnabled(ok);
        };
        for (auto& b : boxes) b.PasswordChanged([check](auto&&, auto&&) { check(); });
        check();
        if (!boxes.empty()) dlg.Opened([first = boxes.front()](auto&&, auto&&) { first.Focus(FocusState::Programmatic); });
        return dlg;
    }

    Windows::Foundation::IAsyncOperation<bool> MainWindow::AskUnlockVaultAsync()
    {
        auto strong = get_strong();
        bool wrong = false;
        while (lycan_app::PasswordVault::Get().Locked())
        {
            std::vector<PasswordBox> boxes;
            auto dlg = MakeMasterDialog(lycan::Tr(L"main.unlock_saved_passwords"), {L"main.master_password"}, false, boxes);
            dlg.PrimaryButtonText(lycan::Tr(L"main.unlock"));
            if (wrong)
            {
                TextBlock warn;
                warn.Text(lycan::Tr(L"main.wrong_master_password"));
                warn.Foreground(Microsoft::UI::Xaml::Media::SolidColorBrush{lycan_app::StatusColor(Windows::UI::Color{255, 0xFF, 0x6B, 0x6B})});
                dlg.Content().as<StackPanel>().Children().InsertAt(0, warn);
            }
            if (co_await ShowDialogAsync(dlg) != ContentDialogResult::Primary) co_return false;
            if (co_await UnlockVaultAsync(boxes[0].Password())) break;
            wrong = true;
        }
        RebuildPasswordList();
        co_return true;
    }

    fire_and_forget MainWindow::OnSetMasterClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        auto strong = get_strong();
        std::vector<PasswordBox> boxes;
        auto dlg = MakeMasterDialog(lycan::Tr(L"main.set_master_password"), {L"main.new_master_password", L"main.re_enter_password"}, true, boxes);
        if (co_await ShowDialogAsync(dlg) != ContentDialogResult::Primary) co_return;
        std::wstring pw{boxes[0].Password()};
        SetBusy(true);
        co_await resume_background();
        auto key = lycan::MakeMasterKey(pw);
        SecureZeroMemory(pw.data(), pw.size() * sizeof(wchar_t));
        co_await resume_ui{dispatcher_};
        SetBusy(false);
        if (!lycan_app::PasswordVault::Get().SetMaster(std::move(key))) ShowMessage(lycan::Tr(L"main.master_password"), lycan::Tr(L"main.could_not_save_passwords"));
        else ShowStatusNote(lycan::Tr(L"main.master_password_set"));
        RebuildPasswordList();
    }

    fire_and_forget MainWindow::OnChangeMasterClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        auto strong = get_strong();
        std::vector<PasswordBox> boxes;
        auto dlg = MakeMasterDialog(lycan::Tr(L"main.change_master_password"), {L"main.current_master_password", L"main.new_master_password", L"main.re_enter_password"}, true, boxes);
        if (co_await ShowDialogAsync(dlg) != ContentDialogResult::Primary) co_return;
        std::wstring current{boxes[0].Password()}, pw{boxes[1].Password()};
        auto old = lycan_app::PasswordVault::Get().Key();
        SetBusy(true);
        co_await resume_background();
        const bool right = lycan::CheckMasterKey(old, current);
        lycan::MasterKey key;
        if (right) key = lycan::MakeMasterKey(pw);
        old.Clear();
        SecureZeroMemory(current.data(), current.size() * sizeof(wchar_t));
        SecureZeroMemory(pw.data(), pw.size() * sizeof(wchar_t));
        co_await resume_ui{dispatcher_};
        SetBusy(false);
        if (!right) { ShowMessage(lycan::Tr(L"main.change_master_password"), lycan::Tr(L"main.wrong_master_password")); co_return; }
        if (!lycan_app::PasswordVault::Get().SetMaster(std::move(key))) ShowMessage(lycan::Tr(L"main.master_password"), lycan::Tr(L"main.could_not_save_passwords"));
        else ShowStatusNote(lycan::Tr(L"main.master_password_changed"));
        RebuildPasswordList();
    }

    fire_and_forget MainWindow::OnRemoveMasterClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        auto strong = get_strong();
        std::vector<PasswordBox> boxes;
        auto dlg = MakeMasterDialog(lycan::Tr(L"main.turn_off_master_password"), {L"main.current_master_password"}, false, boxes);
        if (co_await ShowDialogAsync(dlg) != ContentDialogResult::Primary) co_return;
        std::wstring current{boxes[0].Password()};
        auto old = lycan_app::PasswordVault::Get().Key();
        SetBusy(true);
        co_await resume_background();
        const bool right = lycan::CheckMasterKey(old, current);
        old.Clear();
        SecureZeroMemory(current.data(), current.size() * sizeof(wchar_t));
        co_await resume_ui{dispatcher_};
        SetBusy(false);
        if (!right) { ShowMessage(lycan::Tr(L"main.turn_off_master_password"), lycan::Tr(L"main.wrong_master_password")); co_return; }
        if (!lycan_app::PasswordVault::Get().RemoveMaster()) ShowMessage(lycan::Tr(L"main.master_password"), lycan::Tr(L"main.could_not_save_passwords"));
        else ShowStatusNote(lycan::Tr(L"main.master_password_turned_off"));
        RebuildPasswordList();
    }

    void MainWindow::OnLockPasswordsClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        lycan_app::PasswordVault::Get().Lock();
        lycan::diag::Info(L"saved passwords: locked");
        ShowStatusNote(lycan::Tr(L"main.saved_passwords_locked_now"));
        RebuildPasswordList();
    }

    fire_and_forget MainWindow::OnUnlockPasswordsClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        auto strong = get_strong();
        co_await AskUnlockVaultAsync();
    }

    // The master password is forgotten: the saved passwords cannot be read any more, so they go with it
    fire_and_forget MainWindow::OnResetMasterClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        auto strong = get_strong();
        if (!co_await ConfirmAsync(lycan::Tr(L"main.forgot_master_password"), lycan::Tr(L"main.reset_saved_passwords_question"), lycan::Tr(L"main.delete_all"))) co_return;
        if (!lycan_app::PasswordVault::Get().Reset()) ShowMessage(lycan::Tr(L"main.master_password"), lycan::Tr(L"main.could_not_save_passwords"));
        lycan::diag::Info(L"saved passwords: deleted with the master password (forgotten)");
        RebuildPasswordList();
    }

    // The first and the last character of a password, the rest (and the length) hidden; a surrogate pair counts as one character
    static std::wstring MaskEnds(std::wstring const& pw)
    {
        const size_t first = pw.size() > 1 && IS_HIGH_SURROGATE(pw[0]) ? 2 : 1;
        const size_t last = pw.size() > 1 && IS_LOW_SURROGATE(pw.back()) ? 2 : 1;
        if (pw.size() <= first + last) return L"••••";
        return pw.substr(0, first) + L"••••" + pw.substr(pw.size() - last);
    }

    // New archive (and adding to an encrypted one): fill the password from the saved list. Locked: the master password first.
    fire_and_forget MainWindow::OnPickSavedPasswordClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        auto strong = get_strong();
        auto& vault = lycan_app::PasswordVault::Get();
        if (vault.Locked() && !co_await AskUnlockVaultAsync()) co_return;
        const auto items = vault.Saved();
        Flyout fly;
        StackPanel sp;
        sp.Spacing(6);
        sp.MinWidth(320);
        sp.MaxWidth(480);
        if (items.empty())
        {
            TextBlock none;
            none.Text(lycan::Tr(L"main.no_saved_passwords"));
            none.Opacity(0.65);
            sp.Children().Append(none);
        }
        else
        {
            CheckBox show;
            show.Content(box_value(lycan::Tr(L"main.show_passwords")));
            Automation::AutomationProperties::SetName(show, lycan::Tr(L"main.show_passwords"));
            sp.Children().Append(show);
            StackPanel list;
            list.Spacing(2);
            const auto mono = Microsoft::UI::Xaml::Media::FontFamily{L"Cascadia Mono, Consolas"};
            auto shown = std::make_shared<std::vector<std::pair<TextBlock, std::wstring>>>();
            for (auto const& it : items)
            {
                Grid g;
                g.ColumnSpacing(16);
                for (auto w : {GridLength{0, GridUnitType::Auto}, GridLength{1, GridUnitType::Star}})
                {
                    ColumnDefinition cd;
                    cd.Width(w);
                    g.ColumnDefinitions().Append(cd);
                }
                TextBlock pw;
                pw.Text(hstring{MaskEnds(it.password)});
                pw.FontFamily(mono);
                pw.MinWidth(90);
                TextBlock note;
                note.Text(hstring{it.note});
                note.Opacity(0.75);
                note.TextTrimming(TextTrimming::CharacterEllipsis);
                Grid::SetColumn(note, 1);
                g.Children().Append(pw);
                g.Children().Append(note);
                Button b;
                b.Content(g);
                b.HorizontalAlignment(HorizontalAlignment::Stretch);
                b.HorizontalContentAlignment(HorizontalAlignment::Left);
                b.Background(Microsoft::UI::Xaml::Media::SolidColorBrush{Windows::UI::Color{0, 0, 0, 0}});
                b.BorderThickness(ThicknessHelper::FromUniformLength(0));
                Automation::AutomationProperties::SetName(b, hstring{it.note.empty() ? std::wstring(lycan::Tr(L"main.hidden_password")) : it.note});
                const std::wstring value = it.password;
                b.Click([weak = get_weak(), value, fly](auto&&, auto&&) {
                    auto s = weak.get();
                    if (!s) return;
                    s->SetCompressPassword(value);
                    s->CPwConfirm().Password(hstring{value});   // chosen, not typed: no need to type it again
                    s->RefreshCompressUi();
                    fly.Hide();
                });
                shown->emplace_back(pw, it.password);
                list.Children().Append(b);
            }
            show.Click([show, shown](auto&&, auto&&) {
                const bool all = show.IsChecked().Value();
                for (auto& [tb, value] : *shown) tb.Text(hstring{all ? value : MaskEnds(value)});
            });
            ScrollViewer sv;
            sv.MaxHeight(360);
            sv.Content(list);
            sp.Children().Append(sv);
        }
        fly.Content(sp);
        if (auto tag = lycan::i18n::UiLanguageTag(); !tag.empty()) sp.Language(hstring{tag});
        fly.ShowAt(CPwPick());
    }

    // ---- The "Passwords" tab of the settings: the saved list (masked; show, copy, edit, delete), add, import, export

    Windows::Foundation::IAsyncOperation<bool> MainWindow::ConfirmAsync(hstring title, hstring body, hstring yes)
    {
        auto strong = get_strong();
        auto dlg = MakeDialog(title, L"");
        TextBlock tb;
        tb.Text(body);
        tb.TextWrapping(TextWrapping::Wrap);
        tb.MaxWidth(460);
        dlg.Content(tb);
        dlg.PrimaryButtonText(yes);
        dlg.CloseButtonText(lycan::Tr(L"main.cancel"));
        dlg.DefaultButton(ContentDialogButton::Close);
        co_return co_await ShowDialogAsync(dlg) == ContentDialogResult::Primary;
    }

    void MainWindow::RebuildPasswordList()
    {
        auto& vault = lycan_app::PasswordVault::Get();
        const bool unreadable = vault.State() == lycan::PasswordFileState::Unreadable;
        PasswordsStatus().Text(hstring{lycan::Tr(L"main.saved_passwords_unreadable")});
        PasswordsStatus().Visibility(unreadable ? Visibility::Visible : Visibility::Collapsed);
        ClearPasswordsButton().Visibility(unreadable ? Visibility::Visible : Visibility::Collapsed);
        // the master password card: what it is now, and the buttons for that state
        const bool locked = vault.Locked(), master = vault.HasMaster();
        MasterPasswordState().Text(lycan::Tr(locked ? L"main.master_password_locked" : master ? L"main.master_password_unlocked" : L"main.master_password_off"));
        auto shown = [](auto const& e, bool v) { e.Visibility(v ? Visibility::Visible : Visibility::Collapsed); };
        shown(SetMasterButton(), !master);
        SetMasterButton().IsEnabled(!unreadable);
        shown(UnlockPasswordsButton(), locked);
        shown(LockPasswordsButton(), master && !locked);
        shown(ChangeMasterButton(), master && !locked);
        shown(RemoveMasterButton(), master && !locked);
        shown(ResetMasterButton(), locked);
        auto list = PasswordList();
        list.Children().Clear();
        const auto items = vault.Saved();
        AddPasswordButton().IsEnabled(!unreadable && !locked);
        ImportPasswordsButton().IsEnabled(!unreadable && !locked);
        ExportPasswordsButton().IsEnabled(!unreadable && !locked && !items.empty());
        if (locked)
        {
            TextBlock note;
            note.Text(hstring{lycan::Tr(L"main.saved_passwords_are_locked")});
            note.Opacity(0.65);
            note.TextWrapping(TextWrapping::Wrap);
            list.Children().Append(note);
            return;
        }
        if (items.empty() && !unreadable)
        {
            TextBlock none;
            none.Text(hstring{lycan::Tr(L"main.no_saved_passwords")});
            none.Opacity(0.65);
            list.Children().Append(none);
            return;
        }
        const auto mono = Microsoft::UI::Xaml::Media::FontFamily{L"Cascadia Mono, Consolas"};
        for (auto const& it : items)
        {
            Grid row;
            row.ColumnSpacing(12);
            row.Padding(ThicknessHelper::FromLengths(0, 2, 0, 2));
            for (auto w : {GridLength{2, GridUnitType::Star}, GridLength{2, GridUnitType::Star}, GridLength{0, GridUnitType::Auto}, GridLength{0, GridUnitType::Auto}})
            {
                ColumnDefinition cd;
                cd.Width(w);
                row.ColumnDefinitions().Append(cd);
            }
            const std::wstring masked(std::min<size_t>(std::max<size_t>(it.password.size(), 6), 16), L'•');   // the length is not shown either
            TextBlock pw;
            pw.Text(hstring{masked});
            pw.FontFamily(mono);
            pw.VerticalAlignment(VerticalAlignment::Center);
            pw.TextTrimming(TextTrimming::CharacterEllipsis);
            Automation::AutomationProperties::SetName(pw, hstring{lycan::Tr(L"main.hidden_password")});
            TextBlock note;
            note.Text(hstring{it.note});
            note.Opacity(0.75);
            note.VerticalAlignment(VerticalAlignment::Center);
            note.TextTrimming(TextTrimming::CharacterEllipsis);
            Grid::SetColumn(note, 1);
            TextBlock used;
            used.Text(hstring{it.lastUsed ? FormatTime(it.lastUsed) : lycan::Tr(L"main.never_used")});
            used.Opacity(0.6);
            used.FontSize(12);
            used.VerticalAlignment(VerticalAlignment::Center);
            Grid::SetColumn(used, 2);
            StackPanel buttons;
            buttons.Orientation(Orientation::Horizontal);
            buttons.Spacing(4);
            Grid::SetColumn(buttons, 3);
            auto mk = [&](const wchar_t* glyph, const wchar_t* tip) {
                Button b;
                b.Content(MakeGlyph(glyph, 14));
                b.Padding(ThicknessHelper::FromLengths(8, 5, 8, 5));
                ToolTipService::SetToolTip(b, box_value(hstring{lycan::Tr(tip)}));
                Automation::AutomationProperties::SetName(b, hstring{lycan::Tr(tip)});
                buttons.Children().Append(b);
                return b;
            };
            auto show = mk(L"", L"main.show_password");
            auto copy = mk(L"", L"main.copy_password");
            auto edit = mk(L"", L"main.edit_password");
            auto del = mk(L"", L"main.delete_password");
            const std::wstring value = it.password;
            show.Click([pw, value, masked](auto&&, auto&&) {
                const bool hidden = pw.Text() == hstring{masked};
                pw.Text(hstring{hidden ? value : masked});
                Automation::AutomationProperties::SetName(pw, hstring{hidden ? value : std::wstring(lycan::Tr(L"main.hidden_password"))});
            });
            copy.Click([weak = get_weak(), value](auto&&, auto&&) {
                Windows::ApplicationModel::DataTransfer::DataPackage dp;
                dp.SetText(hstring{value});
                Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(dp);
                if (auto s = weak.get()) s->ShowStatusNote(lycan::Tr(L"main.password_copied"));
            });
            edit.Click([weak = get_weak(), value](auto&&, auto&&) { if (auto s = weak.get()) s->EditPasswordAsync(value); });
            del.Click([weak = get_weak(), value](auto&&, auto&&) { if (auto s = weak.get()) s->DeletePasswordAsync(value); });
            for (UIElement e : {UIElement{pw}, UIElement{note}, UIElement{used}, UIElement{buttons}}) row.Children().Append(e);
            list.Children().Append(row);
        }
    }

    // Add (original empty) or edit a saved password and its note
    fire_and_forget MainWindow::EditPasswordAsync(std::wstring original)
    {
        auto strong = get_strong();
        auto& vault = lycan_app::PasswordVault::Get();
        auto items = vault.Saved();
        std::wstring oldNote;
        for (auto const& it : items) if (it.password == original) oldNote = it.note;
        auto dlg = MakeDialog(lycan::Tr(original.empty() ? L"main.add_password" : L"main.edit_password"), L"");
        StackPanel sp;
        sp.Spacing(12);
        sp.MinWidth(380);
        TextBox pw;   // plain text, like the password dialog: a hidden box would turn the input method off
        pw.Header(box_value(lycan::Tr(L"main.password")));
        pw.Text(hstring{original});
        Automation::AutomationProperties::SetName(pw, lycan::Tr(L"main.password"));
        TextBox note;
        note.Header(box_value(lycan::Tr(L"main.password_note")));
        note.PlaceholderText(lycan::Tr(L"main.password_note_hint"));
        note.Text(hstring{oldNote});
        Automation::AutomationProperties::SetName(note, lycan::Tr(L"main.password_note"));
        sp.Children().Append(pw);
        sp.Children().Append(note);
        dlg.Content(sp);
        dlg.PrimaryButtonText(lycan::Tr(L"main.save_button"));
        dlg.CloseButtonText(lycan::Tr(L"main.cancel"));
        dlg.DefaultButton(ContentDialogButton::Primary);
        pw.TextChanged([dlg, pw](auto&&, auto&&) { dlg.IsPrimaryButtonEnabled(!pw.Text().empty()); });
        dlg.IsPrimaryButtonEnabled(!original.empty());
        if (co_await ShowDialogAsync(dlg) != ContentDialogResult::Primary) co_return;
        const std::wstring value{pw.Text()};
        if (value.empty()) co_return;
        items = vault.Saved();   // (it may have changed while the dialog was open)
        auto at = std::find_if(items.begin(), items.end(), [&](auto const& s) { return s.password == (original.empty() ? value : original); });
        if (at == items.end()) items.push_back({value, std::wstring(note.Text()), lycan::FileTimeNow(), 0});
        else { at->password = value; at->note = std::wstring(note.Text()); }
        // the same password twice (edited into one that exists): one entry
        std::vector<lycan::SavedPassword> merged;
        for (auto& s : items)
            if (std::none_of(merged.begin(), merged.end(), [&](auto const& m) { return m.password == s.password; })) merged.push_back(s);
        if (!vault.SetSaved(std::move(merged))) ShowMessage(lycan::Tr(L"main.passwords"), lycan::Tr(L"main.could_not_save_passwords"));
        RebuildPasswordList();
    }

    fire_and_forget MainWindow::DeletePasswordAsync(std::wstring password)
    {
        auto strong = get_strong();
        if (!co_await ConfirmAsync(lycan::Tr(L"main.delete_password"), lycan::Tr(L"main.delete_password_question"), lycan::Tr(L"main.delete"))) co_return;
        auto& vault = lycan_app::PasswordVault::Get();
        auto items = vault.Saved();
        items.erase(std::remove_if(items.begin(), items.end(), [&](auto const& s) { return s.password == password; }), items.end());
        if (!vault.SetSaved(std::move(items))) ShowMessage(lycan::Tr(L"main.passwords"), lycan::Tr(L"main.could_not_save_passwords"));
        RebuildPasswordList();
    }

    void MainWindow::OnAddPasswordClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) { EditPasswordAsync(L""); }

    // Import: a text file with one password per line; the ones already saved are skipped
    fire_and_forget MainWindow::OnImportPasswordsClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        auto strong = get_strong();
        auto files = co_await lycan_app::PickFilesAsync(AppWindow().Id(), L"LycanZip.Passwords", false, false);
        if (!files || files.Size() == 0) co_return;
        std::ifstream in(fs::path{std::wstring(files.GetAt(0))}, std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(in)), {});
        if (bytes.size() > (8u << 20)) { ShowMessage(lycan::Tr(L"main.import_passwords"), lycan::Tr(L"main.password_file_too_large")); co_return; }
        auto& vault = lycan_app::PasswordVault::Get();
        auto items = vault.Saved();
        size_t added = 0;
        const uint64_t now = lycan::FileTimeNow();
        for (auto const& line : lycan::ParsePasswordLines(bytes))
            if (std::none_of(items.begin(), items.end(), [&](auto const& s) { return s.password == line; })) { items.push_back({line, L"", now, 0}); ++added; }
        if (added && !vault.SetSaved(std::move(items))) { ShowMessage(lycan::Tr(L"main.import_passwords"), lycan::Tr(L"main.could_not_save_passwords")); co_return; }
        RebuildPasswordList();
        ShowMessage(lycan::Tr(L"main.import_passwords"), hstring{lycan::Tf(L"main.passwords_imported", added)});
    }

    // Export: a plain text file (not encrypted), after a warning
    fire_and_forget MainWindow::OnExportPasswordsClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        auto strong = get_strong();
        if (!co_await ConfirmAsync(lycan::Tr(L"main.export_passwords"), lycan::Tr(L"main.export_passwords_warning"), lycan::Tr(L"main.export_passwords"))) co_return;
        auto out = co_await lycan_app::PickSaveFileAsync(AppWindow().Id(), L"LycanZip.Passwords", {}, L"passwords.txt", {{lycan::Tr(L"main.text_file"), L"txt"}}, lycan::Tr(L"main.export_passwords"));
        if (out.empty()) co_return;
        const std::string text = lycan::PasswordLinesText(lycan_app::PasswordVault::Get().Saved());
        std::ofstream f(fs::path{std::wstring(out)}, std::ios::binary | std::ios::trunc);
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!f.good()) { ShowMessage(lycan::Tr(L"main.export_passwords"), lycan::Tr(L"main.could_not_save_passwords")); co_return; }
        ShowStatusNote(lycan::Tr(L"main.passwords_exported"));
    }

    fire_and_forget MainWindow::OnClearUnreadablePasswordsClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        auto strong = get_strong();
        if (!co_await ConfirmAsync(lycan::Tr(L"main.clear_unreadable_passwords"), lycan::Tr(L"main.clear_unreadable_passwords_question"), lycan::Tr(L"main.delete"))) co_return;
        lycan_app::PasswordVault::Get().ClearUnreadable();
        RebuildPasswordList();
    }

    // A password proved right: kept for this run, and saved when the dialog that asked for it had "Remember this password" ticked
    void MainWindow::PasswordWorked(std::wstring const& pw)
    {
        if (pw.empty()) return;
        const bool save = askedRemember_ && pw == askedPassword_;
        lycan_app::PasswordVault::Get().Worked(pw, settings_.rememberRunPasswords, save);
        askedPassword_.clear();
        askedRemember_ = false;
    }
}
