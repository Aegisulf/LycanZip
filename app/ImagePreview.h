#pragma once
// Preview of pictures in an archive: a separate frosted-glass window, read only when clicked.
// - Each "reader" (the current one / read-ahead / thumbnails) has its own ArchiveSource (one cannot be used by several threads at once) and checks in the progress callback whether it was replaced, to stop.
// - Read-ahead: when paging, the pictures before and after are read into the cache in the background (the total is capped; a setting can turn it off or raise it); it pauses when the last picture was slow to read (solid archive, large file).
// - Thumbnail strip on the left: only the visible thumbnails are made; a solid archive / one without random access is not read actively, only pictures seen before (in the cache) have a thumbnail.
// - Safety limits: 64 MB per file, 100 megapixels; a picture wider than 4096 pixels is decoded at a smaller size.
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <winrt/Windows.Foundation.h>

#include "lycan/archive_source.h"

namespace lycan_app {

struct PreviewItem {
    uint32_t entryIndex = 0;
    std::wstring name;   // file name
    std::wstring path;   // full path inside the archive (for display)
    uint64_t size = 0;   // original size
    uint32_t node = 0;   // node in the tree of the main window (for "open with the associated program")
    uint64_t packedSize = 0;      // packed size (for the picture information)
    uint64_t mtimeFileTime = 0;   // modified time, Windows FILETIME (0 = unknown)
};

struct PreviewRequest {
    std::filesystem::path archive;
    lycan::OpenOptions open;           // includes the password in use / the file name encoding
    std::vector<PreviewItem> items;    // all pictures in the same folder, in the order of the screen
    size_t start = 0;                  // which item of items is shown first
    uint64_t prefetchBytes = 256ull << 20;  // upper limit of the read-ahead cache; 0 = no read-ahead
    std::function<void(uint32_t node)> openExternal;  // Enter: handed back to the main window to open with the associated program (called on the UI thread)
    bool enlargeSmall = false;         // whether "fit window / width / height" also enlarges small pictures
    bool animations = true;            // the animation setting: the next picture fades in
    std::function<void(bool on)> setEnlargeSmall;  // the preview menu toggles "also enlarge small pictures": handed back to the main window to save in the settings (called on the UI thread)
    bool thumbReads = true;            // files are read actively for thumbnails only when the archive has random access (not solid)
    const void* owner = nullptr;       // the main window that made the request (several windows: closing a main window closes only its own preview)
    std::function<void()> onClosed;    // the preview window was closed (UI thread)
    // Reading mode (DESIGN.md "閱讀模式"): every picture of the archive in path order, two pages side by side, right to left
    std::vector<PreviewItem> book;     // the pages of the reading mode; empty = `items` is the whole archive already
    bool reading = false;              // start in the reading mode (a comic book archive opened from Explorer)
    std::filesystem::path readingStore;   // the file that remembers where each book was left (empty = not remembered)
};

// Whether the extension is a "known picture format" (does not guarantee that this computer has a decoder)
bool IsPreviewableImageName(const std::wstring& name);
// Whether the Windows imaging codecs (WIC) of this computer know the extension (for example JXL needs an extension from the Microsoft Store)
bool IsImageDecoderAvailable(const std::wstring& name);
// A known picture format that has a decoder = it can really be previewed
bool CanPreviewImage(const std::wstring& name);
// The explanation when there is no decoder (what to install); a generic message if there is nothing special. Without a final punctuation mark.
std::wstring MissingDecoderHint(const std::wstring& name);

// A picture for the preview pane (PreviewPane.h): decoded at most maxWidth pixels wide (same size and pixel limits as the preview
// window), with the EXIF turn that is still to be applied (rotate = clockwise quarter turns, mirror first). UI thread; throws when the
// picture cannot be decoded.
struct PaneImage {
    winrt::Microsoft::UI::Xaml::Media::ImageSource image{nullptr};
    uint32_t w = 0, h = 0;   // the size of the picture (upright)
    int rotate = 0;
    bool mirror = false;
};
winrt::Windows::Foundation::IAsyncAction DecodePaneImage(std::shared_ptr<std::vector<uint8_t>> data, uint32_t maxWidth, std::shared_ptr<PaneImage> out);

// Opens (or reuses the one that is open) the preview window and loads the picture req.start
void ShowImagePreview(PreviewRequest req);
// Each main window (owner) has its own preview window (several windows can be compared side by side). owner null = close all; otherwise close only the previews of this main window
void CloseImagePreview(const void* owner = nullptr);

// Picture thumbnails of the file list (icon view): read one by one in the background and decoded at a smaller size; the results are in a memory cache with a size limit.
// Only for archives with random access that are not solid (canRead); nothing is read from other archives (the caller has to say so on the screen). All calls are on the UI thread.
class ListThumbs {
public:
    using Done = std::function<void(winrt::Windows::Foundation::IInspectable const&)>;  // the argument is a BitmapImage
    ListThumbs();
    ~ListThumbs();
    // The archive (or the password) changed: clear the cache and the queued work
    void Reset(const std::filesystem::path& archive, const lycan::OpenOptions& open, bool canRead);
    bool CanRead() const;
    // A thumbnail from the cache (nullptr if none); px = upper limit of the decoded width
    winrt::Windows::Foundation::IInspectable Get(uint32_t entryIndex, uint32_t px);
    // Queue a read; processed in the order queued. Items that exist or failed already are not repeated
    void Request(uint32_t entryIndex, uint64_t size, uint32_t px, Done done);
    void Cancel(uint32_t entryIndex);

private:
    struct Impl;
    std::shared_ptr<Impl> p_;
};

}  // namespace lycan_app
