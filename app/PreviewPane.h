#pragma once
// The preview pane under the folder tree, and the larger preview window that shows the same thing (DESIGN.md "預覽窗格").
// - The main window describes what is selected (PaneInfo: a title, detail rows, an icon, the item); the view decides what it can show
//   (picture, text, PDF, audio tags) and reads the bytes itself through a PaneSource.
// - Nothing waits on the UI thread: reads run in the background with a lower priority and stop when the selection moved on (a solid
//   archive also stops in the middle); the last items stay in a small cache; the old content stays until the new one is ready, and
//   "loading" only shows when it takes longer than 150 ms.
// - The larger window follows the selection (the main window passes every PaneInfo to both); it has no button of its own.
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "lycan/archive_source.h"

namespace lycan_app {

enum class PaneKind { Empty, Archive, Multi, Folder, File };

struct PaneInfo {
    PaneKind kind = PaneKind::Empty;
    std::wstring title;                                        // the name (of the item, or of the archive)
    std::vector<std::pair<std::wstring, std::wstring>> rows;   // label, value (in the language of the user)
    winrt::Microsoft::UI::Xaml::Media::ImageSource icon{nullptr};   // the large icon shown when there is nothing to draw
    std::wstring glyph;                                        // a glyph instead when there is no icon
    // a file
    uint32_t node = 0;
    uint32_t entryIndex = 0;
    uint64_t size = 0;
    bool locked = false;      // encrypted and the password is not known yet: nothing is read
};

// The reader of one main window, shared by the pane and the larger window. Read is thread-safe (one read at a time).
class PaneSource {
public:
    struct Data {
        std::shared_ptr<std::vector<uint8_t>> bytes;
        bool complete = false;    // the whole item (false = only the first `limit` bytes)
        bool cancelled = false;
        std::wstring error;
    };
    void Reset(const std::filesystem::path& archive, const lycan::OpenOptions& open);
    // The first `limit` bytes of an item (all of it when it is smaller). Blocks: call it in the background. keepGoing false = stop.
    Data Read(uint32_t entry, uint64_t limit, const std::function<bool()>& keepGoing);

private:
    struct Cached { uint32_t entry; std::shared_ptr<std::vector<uint8_t>> bytes; bool complete; };
    std::mutex m_;
    std::unique_ptr<lycan::ArchiveSource> src_;
    std::filesystem::path archive_;
    lycan::OpenOptions open_;
    std::vector<Cached> cache_;   // the most recently used first
    uint64_t session_ = 0;
};

class PaneView;

// The pane itself (an element the main window puts under the folder tree)
class PreviewPane {
public:
    PreviewPane();
    ~PreviewPane();
    winrt::Microsoft::UI::Xaml::FrameworkElement Root() const;
    void Reset(const std::filesystem::path& archive, const lycan::OpenOptions& open);   // another archive / the password changed
    void Show(PaneInfo info);              // the selection changed (the main window waits until it stops moving)
    void SetActive(bool on);               // a hidden pane reads nothing
    std::function<void(uint32_t node)> onPlayAudio;    // "Play" of an audio file
    std::function<void()> onPassword;                  // "Enter password" of an encrypted item
    std::function<void()> onEnlarge;                   // the "larger preview" button (the main window opens it with its options)
    // The larger window: follows Show; rect "x,y,w,h" (screen pixels, empty = a default size next to the owner); onClosed gets where it was
    struct LargeOptions {
        std::string rect;
        winrt::Microsoft::UI::Xaml::ElementTheme theme = winrt::Microsoft::UI::Xaml::ElementTheme::Default;
        HWND owner = nullptr;
        std::function<void(std::string const& rect)> onClosed;
    };
    void OpenLarge(LargeOptions o);
    void CloseLarge();
    bool LargeOpen() const;

private:
    std::shared_ptr<PaneSource> source_;
    std::shared_ptr<PaneView> view_, large_;
    winrt::Microsoft::UI::Xaml::Window largeWin_{nullptr};
    std::function<void(std::string const&)> largeClosed_;
    PaneInfo last_;
    bool active_ = true;
    std::shared_ptr<int> life_ = std::make_shared<int>(0);   // queued work checks that the pane still exists
    std::string LargeRect() const;
};

}  // namespace lycan_app
