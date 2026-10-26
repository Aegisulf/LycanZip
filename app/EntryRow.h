#pragma once
#include "EntryRow.g.h"
#include "CrumbItem.g.h"

#include <algorithm>
#include <limits>
#include <string>
#include <vector>

#include "FileIconImages.h"
#include "HighContrast.h"
#include "lycan/fileicons.h"
#include "lycan/i18n.h"

namespace winrt::LycanZip::implementation
{
    // File category: decides the icon and the colour. Only broad categories, no call to the shell: fast, no lag with 100,000 items.
    enum class FileCategory { Folder, Locked, Image, Audio, Video, Archive, Code, Document, Executable, Other };

    inline FileCategory CategoryOfName(std::wstring const& name, bool isDir, bool isEncrypted)
    {
        if (isDir) return FileCategory::Folder;
        if (isEncrypted) return FileCategory::Locked;
        auto dot = name.rfind(L'.');
        if (dot == std::wstring::npos) return FileCategory::Other;
        std::wstring ext = name.substr(dot + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
        auto in = [&](std::initializer_list<const wchar_t*> list) {
            for (auto e : list) if (ext == e) return true;
            return false;
        };
        if (in({L"png", L"jpg", L"jpeg", L"gif", L"bmp", L"webp", L"tif", L"tiff", L"ico", L"svg", L"heic", L"avif"})) return FileCategory::Image;
        if (in({L"mp3", L"wav", L"flac", L"ogg", L"m4a", L"aac", L"wma", L"opus"})) return FileCategory::Audio;
        if (in({L"mp4", L"mkv", L"avi", L"mov", L"wmv", L"webm", L"flv", L"m4v"})) return FileCategory::Video;
        if (in({L"zip", L"7z", L"rar", L"tar", L"gz", L"tgz", L"xz", L"bz2", L"zst", L"cab", L"iso"})) return FileCategory::Archive;
        if (in({L"exe", L"dll", L"msi", L"bat", L"cmd", L"com", L"scr"})) return FileCategory::Executable;
        if (in({L"c", L"h", L"cpp", L"hpp", L"cs", L"py", L"js", L"ts", L"json", L"xml", L"html", L"htm", L"css", L"yml", L"yaml",
                L"ini", L"cfg", L"ps1", L"sh", L"java", L"go", L"rs", L"php", L"sql"})) return FileCategory::Code;
        if (in({L"txt", L"md", L"log", L"pdf", L"doc", L"docx", L"xls", L"xlsx", L"ppt", L"pptx", L"rtf", L"csv"})) return FileCategory::Document;
        return FileCategory::Other;
    }

    // Icons (Segoe Fluent Icons)
    inline const wchar_t* GlyphForCategory(FileCategory c)
    {
        switch (c)
        {
            case FileCategory::Folder: return L"\uE8B7";
            case FileCategory::Locked: return L"\uE72E";
            case FileCategory::Image: return L"\uE91B";
            case FileCategory::Audio: return L"\uE8D6";
            case FileCategory::Video: return L"\uE714";
            case FileCategory::Archive: return L"\uE7B8";
            case FileCategory::Code: return L"\uE943";
            case FileCategory::Document: return L"\uE8A5";
            default: return L"\uE7C3";
        }
    }

    // Category colours (clear in the dark and the light theme): folder amber, picture green, audio pink, video purple, archive teal, code blue, document grey-blue, executable orange-red, encrypted orange
    inline Microsoft::UI::Xaml::Media::Brush BrushForCategory(FileCategory c)
    {
        // Shared by all: 100,000 items still need only 10. Note: default-constructing the elements of a C++/WinRT array "creates new objects", so fill it with nullptr explicitly
        // a contrast theme: one colour for everything (the colours below can disappear on it)
        if (lycan_app::HighContrast())
        {
            static Microsoft::UI::Xaml::Media::SolidColorBrush hc{nullptr};
            if (!hc) hc = Microsoft::UI::Xaml::Media::SolidColorBrush{lycan_app::HighContrastForeground()};
            else hc.Color(lycan_app::HighContrastForeground());
            return hc;
        }
        static std::vector<Microsoft::UI::Xaml::Media::SolidColorBrush> brushes(10, nullptr);
        static constexpr uint32_t rgb[10] = {0xF2B33D, 0xFF8A4C, 0x4CC38A, 0xF06A9B, 0xA07BEF, 0x3FC1C9, 0x5B9DFF, 0x9AA9C4, 0xEF6B57, 0x9AA9C4};
        auto i = static_cast<size_t>(c);
        if (!brushes[i])
        {
            uint32_t v = rgb[i];
            brushes[i] = Microsoft::UI::Xaml::Media::SolidColorBrush{
                Windows::UI::Color{255, static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v)}};
        }
        return brushes[i];
    }

    struct EntryRow : EntryRowT<EntryRow>
    {
        EntryRow(uint32_t nodeId, hstring const& name, hstring const& folder, hstring const& sizeText, hstring const& packedText,
                 hstring const& modifiedText, bool isDir, bool isEncrypted, bool showPath)
            : nodeId_(nodeId), name_(name), folder_(folder), sizeText_(sizeText), packedText_(packedText), modifiedText_(modifiedText),
              isDir_(isDir), isEncrypted_(isEncrypted), showPath_(showPath) {}

        uint32_t NodeId() const { return nodeId_; }
        hstring Name() const { return name_; }
        hstring Folder() const { return folder_; }
        hstring SizeText() const { return sizeText_; }
        hstring PackedText() const { return packedText_; }
        hstring ModifiedText() const { return modifiedText_; }
        hstring RatioText() const { return ratioText_; }
        hstring CrcText() const { return crcText_; }
        hstring MethodText() const { return methodText_; }
        Microsoft::UI::Xaml::Visibility LockVisibility() const { return lock_ ? Microsoft::UI::Xaml::Visibility::Visible : Microsoft::UI::Xaml::Visibility::Collapsed; }
        void SetExtra(hstring const& ratioText, hstring const& crcText, hstring const& methodText, bool encrypted)
        {
            ratioText_ = ratioText;
            crcText_ = crcText;
            methodText_ = methodText;
            lock_ = encrypted;
        }
        // Type column: folder, the extension in capitals (PNG), and an em dash for no extension
        hstring TypeText() const
        {
            if (isDir_) return lycan::Tr(L"drag.folder");
            std::wstring n{name_};
            auto dot = n.rfind(L'.');
            if (dot == std::wstring::npos || dot == 0 || dot + 1 >= n.size()) return L"—";
            std::wstring ext = n.substr(dot + 1);
            std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return static_cast<wchar_t>(towupper(c)); });
            return hstring{ext};
        }
        bool IsDir() const { return isDir_; }
        hstring Glyph() const { return hstring{GlyphForCategory(category())}; }
        Microsoft::UI::Xaml::Media::Brush GlyphBrush() const { return BrushForCategory(category()); }
        bool IsSelected() const { return selected_; }
        void IsSelected(bool v) { if (selected_ != v) { selected_ = v; Raise(L"IsSelected"); RaiseCheck(); } }
        bool IsHover() const { return hover_; }
        void IsHover(bool v) { if (hover_ != v) { hover_ = v; RaiseCheck(); } }
        double CheckOpacity() const { return selected_ || hover_ ? 1.0 : 0.0; }
        double GlyphOpacity() const { return selected_ || hover_ ? 0.0 : 1.0; }
        // the second tone of the icons of the list and the tree: a soft plate of the colour of the icon behind the line drawing (none in a contrast theme)
        double ChipOpacity() const { return lycan_app::HighContrast() || selected_ || hover_ ? 0.0 : 0.2; }
        bool CheckHitTest() const { return selected_ || hover_; }
        double ItemWidth() const { return itemWidth_; }
        double BoxHeight() const { return boxHeight_; }
        double ItemHeight() const { return boxHeight_ + 22.0 + 20.0 * nameLines_; }  // the picture area + the lines of the name + the padding: the same for every cell of a folder
        int NameLines() const { return nameLines_; }
        // Name in the icon view: add a no-break character after ! and ? so that "!" does not get a line of its own (a line break is normally allowed after these marks)
        hstring DisplayName() const
        {
            std::wstring out;
            for (wchar_t c : name_) { out += c; if (c == L'!' || c == L'?') out += static_cast<wchar_t>(0x2060); }  // add a WORD JOINER after it
            return hstring{out};
        }
        Microsoft::UI::Xaml::Media::ImageSource ThumbSource() const { return thumb_; }
        Microsoft::UI::Xaml::Visibility ThumbVisibility() const { return thumb_ ? Microsoft::UI::Xaml::Visibility::Visible : Microsoft::UI::Xaml::Visibility::Collapsed; }
        Microsoft::UI::Xaml::Visibility GlyphVisibility() const { return thumb_ || shellLarge_ || HasArchiveIcon() ? Microsoft::UI::Xaml::Visibility::Collapsed : Microsoft::UI::Xaml::Visibility::Visible; }
        // An archive inside the archive: its file-type icon in place of the glyph (not the root of the tree, which has RootIcon; not an encrypted entry: it keeps the lock)
        bool HasArchiveIcon() const { return nodeId_ != 0 && category() == FileCategory::Archive; }
        Microsoft::UI::Xaml::Media::ImageSource ArchiveIcon() const { return HasArchiveIcon() ? lycan_app::FileIconImage(lycan::IconFamilyForPath(std::wstring{name_}), 32) : nullptr; }
        Microsoft::UI::Xaml::Media::ImageSource ArchiveIconLarge() const { return HasArchiveIcon() ? lycan_app::FileIconImage(lycan::IconFamilyForPath(std::wstring{name_}), iconSize_ > 56 ? 256 : 64) : nullptr; }
        Microsoft::UI::Xaml::Visibility ArchiveIconVisibility() const { return HasArchiveIcon() && !thumb_ && !shellLarge_ ? Microsoft::UI::Xaml::Visibility::Visible : Microsoft::UI::Xaml::Visibility::Collapsed; }
        Microsoft::UI::Xaml::Visibility ListArchiveVisibility() const { return HasArchiveIcon() && !shellSmall_ ? Microsoft::UI::Xaml::Visibility::Visible : Microsoft::UI::Xaml::Visibility::Collapsed; }
        Microsoft::UI::Xaml::Visibility ListGlyphVisibility() const { return HasArchiveIcon() || shellSmall_ ? Microsoft::UI::Xaml::Visibility::Collapsed : Microsoft::UI::Xaml::Visibility::Visible; }
        // The icons of Windows: which one ("<dir>", ".txt", "." = a file without an extension; empty = none: the root, an encrypted item keeps its lock)
        hstring IconKey() const
        {
            if (nodeId_ == 0 || isEncrypted_) return {};
            if (isDir_) return L"<dir>";
            std::wstring n{name_};
            const auto dot = n.rfind(L'.');
            if (dot == std::wstring::npos || dot + 1 >= n.size()) return L".";
            std::wstring ext = n.substr(dot);
            std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
            return hstring{ext};
        }
        void SetShellIcons(Microsoft::UI::Xaml::Media::ImageSource const& smallIcon, Microsoft::UI::Xaml::Media::ImageSource const& largeIcon)
        {
            shellSmall_ = smallIcon;
            shellLarge_ = largeIcon;
            for (auto n : {L"ShellIcon", L"ShellIconLarge", L"ShellIconVisibility", L"ShellIconLargeVisibility", L"ListGlyphVisibility", L"ListArchiveVisibility",
                           L"GlyphVisibility", L"ArchiveIconVisibility", L"TreeGlyphVisibility"}) Raise(n);
        }
        Microsoft::UI::Xaml::Media::ImageSource ShellIcon() const { return shellSmall_; }
        Microsoft::UI::Xaml::Media::ImageSource ShellIconLarge() const { return shellLarge_; }
        Microsoft::UI::Xaml::Visibility ShellIconVisibility() const { return shellSmall_ ? Microsoft::UI::Xaml::Visibility::Visible : Microsoft::UI::Xaml::Visibility::Collapsed; }
        Microsoft::UI::Xaml::Visibility ShellIconLargeVisibility() const { return shellLarge_ && !thumb_ ? Microsoft::UI::Xaml::Visibility::Visible : Microsoft::UI::Xaml::Visibility::Collapsed; }
        Microsoft::UI::Xaml::Media::ImageSource RootIcon() const { return rootIcon_; }
        Microsoft::UI::Xaml::Visibility RootIconVisibility() const { return rootIcon_ ? Microsoft::UI::Xaml::Visibility::Visible : Microsoft::UI::Xaml::Visibility::Collapsed; }
        Microsoft::UI::Xaml::Visibility TreeGlyphVisibility() const { return rootIcon_ || shellSmall_ ? Microsoft::UI::Xaml::Visibility::Collapsed : Microsoft::UI::Xaml::Visibility::Visible; }
        void SetRootIcon(Microsoft::UI::Xaml::Media::ImageSource const& s)
        {
            rootIcon_ = s;
            Raise(L"RootIcon");
            Raise(L"RootIconVisibility");
            Raise(L"TreeGlyphVisibility");
        }
        void SetLayout(double itemWidth, double boxHeight, int nameLines, double iconSize) { itemWidth_ = itemWidth; boxHeight_ = boxHeight; nameLines_ = nameLines; iconSize_ = iconSize; }
        double IconSize() const { return iconSize_; }
        double GlyphSize() const { return iconSize_ * 0.9; }
        void SetThumb(Microsoft::UI::Xaml::Media::ImageSource const& s)
        {
            if (thumb_ == s) return;
            thumb_ = s;
            Raise(L"ThumbSource");
            Raise(L"ThumbVisibility");
            Raise(L"GlyphVisibility");
            Raise(L"ArchiveIconVisibility");
            Raise(L"ShellIconLargeVisibility");
        }

        winrt::event_token PropertyChanged(Microsoft::UI::Xaml::Data::PropertyChangedEventHandler const& h) { return changed_.add(h); }
        void PropertyChanged(winrt::event_token const& t) noexcept { changed_.remove(t); }

        Microsoft::UI::Xaml::GridLength PathWidth() const
        {
            return {showPath_ ? 2.0 : 0.0, showPath_ ? Microsoft::UI::Xaml::GridUnitType::Star : Microsoft::UI::Xaml::GridUnitType::Pixel};
        }

    private:
        // node 0 is the root of the folder tree (the archive itself); the list never has a row for it
        FileCategory category() const { return nodeId_ == 0 ? FileCategory::Archive : CategoryOfName(std::wstring(name_), isDir_, isEncrypted_); }
        void RaiseCheck() { Raise(L"CheckOpacity"); Raise(L"GlyphOpacity"); Raise(L"ChipOpacity"); Raise(L"CheckHitTest"); }
        void Raise(wchar_t const* name) { changed_(*this, Microsoft::UI::Xaml::Data::PropertyChangedEventArgs{name}); }
        winrt::event<Microsoft::UI::Xaml::Data::PropertyChangedEventHandler> changed_;
        bool selected_ = false, hover_ = false;
        double itemWidth_ = 104.0, boxHeight_ = 50.0, iconSize_ = 44.0;
        int nameLines_ = 3;
        Microsoft::UI::Xaml::Media::ImageSource thumb_{nullptr};
        Microsoft::UI::Xaml::Media::ImageSource rootIcon_{nullptr};
        Microsoft::UI::Xaml::Media::ImageSource shellSmall_{nullptr}, shellLarge_{nullptr};
        uint32_t nodeId_;
        hstring name_, folder_, sizeText_, packedText_, modifiedText_;
        bool isDir_, isEncrypted_, showPath_;
        hstring ratioText_, crcText_, methodText_;
        bool lock_ = false;
    };
}

namespace winrt::LycanZip::implementation
{
    struct CrumbItem : CrumbItemT<CrumbItem>
    {
        CrumbItem(hstring const& name, bool isLast) : name_(name), last_(isLast) {}
        hstring Name() const { return name_; }
        hstring Tip() const { return last_ ? name_ : hstring{lycan::Tf(L"main.go", name_)}; }
        Microsoft::UI::Xaml::Visibility NormalVisibility() const { return last_ ? Microsoft::UI::Xaml::Visibility::Collapsed : Microsoft::UI::Xaml::Visibility::Visible; }
        Microsoft::UI::Xaml::Visibility LastVisibility() const { return last_ ? Microsoft::UI::Xaml::Visibility::Visible : Microsoft::UI::Xaml::Visibility::Collapsed; }

    private:
        hstring name_;
        bool last_;
    };
}

namespace winrt::LycanZip::factory_implementation
{
    struct CrumbItem : CrumbItemT<CrumbItem, implementation::CrumbItem> {};
    struct EntryRow : EntryRowT<EntryRow, implementation::EntryRow> {};
}
