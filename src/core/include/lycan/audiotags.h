#pragma once
// Title / artist / album of an audio file, read from the bytes in memory (the audio preview reads the file out of the
// archive anyway). Understands ID3v2 / ID3v1 (MP3, AAC with tags), FLAC, Ogg (Vorbis, Opus), MP4 / M4A, WAV (LIST INFO)
// and WMA (ASF). Never throws; whatever cannot be read stays empty. The data is untrusted: every read is bounds-checked.
#include <cstddef>
#include <cstdint>
#include <string>

namespace lycan {

struct AudioTags {
    std::wstring title, artist, album;
    bool Empty() const { return title.empty() && artist.empty() && album.empty(); }
};

AudioTags ReadAudioTags(const uint8_t* data, size_t size);

}  // namespace lycan
