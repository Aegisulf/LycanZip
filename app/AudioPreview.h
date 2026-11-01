#pragma once
// Audio preview: a window with the audio files of the current folder on the left and a small player on the right.
// A file is read out of the archive into memory when it is played (up to 256 MB), the title / artist / album come from
// its tags (lycan/audiotags.h), and the Windows MediaPlayer plays it, so the formats are the ones this PC can decode.
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "lycan/archive_source.h"

namespace lycan_app {

struct AudioItem {
    uint32_t entryIndex = 0;
    std::wstring name;   // file name
    std::wstring path;   // full path inside the archive
    uint64_t size = 0;   // original size
};

struct AudioRequest {
    std::filesystem::path archive;
    lycan::OpenOptions open;         // password / file-name encoding in use
    std::vector<AudioItem> items;    // the audio files of the current list, in screen order
    size_t start = 0;                // which one to play first
    const void* owner = nullptr;     // the main window that asked (closing it closes its player)
    std::function<void()> onClosed;  // the player window was closed (UI thread)
    int loopMode = 0;                // 0 play on and stop after the last track, 1 repeat all, 2 repeat the track, 3 shuffle (lycan::AudioLoop)
    std::function<void(int)> onLoopMode;   // the user chose another mode (UI thread): the owner remembers it
};

// Known audio extensions (whether this PC can decode the format is only found out when it is played)
bool IsPreviewableAudioName(const std::wstring& name);
// The content type the Windows media source is given for this file (by its extension)
const wchar_t* AudioContentType(const std::wstring& name);

// Opens (or reuses) the player window of req.owner and plays req.items[req.start]
void ShowAudioPreview(AudioRequest req);
// owner == nullptr: close every player window; otherwise only the one of that main window
void CloseAudioPreview(const void* owner = nullptr);

}  // namespace lycan_app
