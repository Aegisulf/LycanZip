#pragma once
// File and folder choosers: the pickers of Windows App SDK 2.x (Microsoft.Windows.Storage.Pickers).
//
// Unlike the WinRT pickers of Windows.Storage.Pickers (whose "Select Folder" button stays disabled until an item is
// selected, so the folder that is shown cannot be chosen), these take a window id instead of an HWND, return plain paths,
// remember the last folder per purpose (SettingsIdentifier) and are asynchronous (the UI thread is not blocked).
// A picker that throws (a bad argument, no window) counts as "cancelled": an exception must not end the program.
#include <winrt/Microsoft.UI.h>
#include <winrt/Microsoft.Windows.Storage.Pickers.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <filesystem>
#include <string>

#include "lycan/diag.h"

namespace lycan_app {

// Each purpose has its own remembered "last folder" (kept by Windows).
inline constexpr const wchar_t* kPurposeExtractDest = L"LycanZip.ExtractDest";
inline constexpr const wchar_t* kPurposeCompressOut = L"LycanZip.CompressOut";
inline constexpr const wchar_t* kPurposeSourceFolder = L"LycanZip.SourceFolder";
inline constexpr const wchar_t* kPurposeSaveImage = L"LycanZip.SaveImage";
inline constexpr const wchar_t* kPurposeTempDir = L"LycanZip.TempDir";
inline constexpr const wchar_t* kPurposeOpenArchive = L"LycanZip.OpenArchive";
inline constexpr const wchar_t* kPurposeSourceFiles = L"LycanZip.SourceFiles";
inline constexpr const wchar_t* kPurposeChecksum = L"LycanZip.Checksum";

namespace pickers = winrt::Microsoft::Windows::Storage::Pickers;

namespace detail {
inline void LogPickerError(const wchar_t* what, winrt::hresult_error const& e) {
    const std::wstring m = std::wstring(L"LycanZip: ") + what + L" failed: " + std::wstring(e.message()) + L"\n";
    OutputDebugStringW(m.c_str());
    lycan::diag::Warn(L"{} failed: 0x{:08X} {}", what, static_cast<unsigned long>(static_cast<int32_t>(e.code())), std::wstring(e.message()));
}
}  // namespace detail

// The chosen folder, or an empty string when the dialog was cancelled. defaultFolder only counts the first time for this
// purpose (afterwards the last used folder wins).
inline winrt::Windows::Foundation::IAsyncOperation<winrt::hstring> PickFolderAsync(winrt::Microsoft::UI::WindowId owner, winrt::hstring purpose,
                                                                                   std::filesystem::path defaultFolder, winrt::hstring title) {
    try {
        pickers::FolderPicker picker(owner);
        picker.SettingsIdentifier(purpose);
        if (!title.empty()) picker.Title(title);
        std::error_code ec;
        if (!defaultFolder.empty() && std::filesystem::is_directory(defaultFolder, ec)) picker.SuggestedStartFolder(winrt::hstring{defaultFolder.wstring()});
        auto result = co_await picker.PickSingleFolderAsync();
        co_return result ? result.Path() : winrt::hstring{};
    } catch (winrt::hresult_error const& e) {
        detail::LogPickerError(L"folder picker", e);
        co_return winrt::hstring{};
    }
}

// The chosen files (empty when cancelled). inDownloads: open in the Downloads folder the first time.
inline winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::Foundation::Collections::IVector<winrt::hstring>> PickFilesAsync(
    winrt::Microsoft::UI::WindowId owner, winrt::hstring purpose, bool multiple, bool inDownloads) {
    auto out = winrt::single_threaded_vector<winrt::hstring>();
    try {
        pickers::FileOpenPicker picker(owner);
        picker.SettingsIdentifier(purpose);
        if (inDownloads) picker.SuggestedStartLocation(pickers::PickerLocationId::Downloads);
        picker.FileTypeFilter().Append(L"*");
        if (multiple) {
            auto files = co_await picker.PickMultipleFilesAsync();
            if (files)
                for (auto const& f : files) out.Append(f.Path());
        } else {
            auto file = co_await picker.PickSingleFileAsync();
            if (file) out.Append(file.Path());
        }
    } catch (winrt::hresult_error const& e) {
        detail::LogPickerError(L"file picker", e);
    }
    co_return out;
}

// "Save as" for one file: the user can change the name (and folder); asks before overwriting. `types` are the file types offered:
// (label, extension without the dot), the first one is the selected one. Returns an empty string on cancel. (The file is not created.)
inline winrt::Windows::Foundation::IAsyncOperation<winrt::hstring> PickSaveFileAsync(winrt::Microsoft::UI::WindowId owner, winrt::hstring purpose,
                                                                                     std::filesystem::path defaultFolder, std::wstring fileName,
                                                                                     std::vector<std::pair<std::wstring, std::wstring>> types, winrt::hstring title) {
    try {
        pickers::FileSavePicker picker(owner);
        picker.SettingsIdentifier(purpose);
        if (!title.empty()) picker.Title(title);
        picker.SuggestedFileName(winrt::hstring{fileName});
        std::error_code ec;
        if (!defaultFolder.empty() && std::filesystem::is_directory(defaultFolder, ec)) picker.SuggestedStartFolder(winrt::hstring{defaultFolder.wstring()});
        // a type needs a real extension (no wildcards); without one the dialog offers whatever the name has
        for (auto const& t : types)
            if (!t.second.empty() && t.second.find_first_of(L"*?") == std::wstring::npos)
                picker.FileTypeChoices().Insert(winrt::hstring{t.first}, winrt::single_threaded_vector<winrt::hstring>({winrt::hstring{L"." + t.second}}));
        auto result = co_await picker.PickSaveFileAsync();
        co_return result ? result.Path() : winrt::hstring{};
    } catch (winrt::hresult_error const& e) {
        detail::LogPickerError(L"save picker", e);
        co_return winrt::hstring{};
    }
}

}  // namespace lycan_app
