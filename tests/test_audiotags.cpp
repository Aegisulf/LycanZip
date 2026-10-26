// Title / artist / album from audio files in memory: ID3v2 / ID3v1, FLAC, Ogg (Vorbis, Opus), MP4, WAV, WMA - and garbage
#include "no_dialogs.h"
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "lycan/audiotags.h"

using namespace lycan;
using Bytes = std::vector<uint8_t>;

static int failures = 0;
#define EXPECT(cond)                                                     \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); \
            ++failures;                                                  \
        }                                                                \
    } while (0)

static void Put(Bytes& b, const std::string& s) { b.insert(b.end(), s.begin(), s.end()); }
static Bytes ToBytes(const std::string& s) { return Bytes(s.begin(), s.end()); }
static void Le32(Bytes& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i))); }
static void Be32(Bytes& b, uint32_t v) { for (int i = 3; i >= 0; --i) b.push_back(uint8_t(v >> (8 * i))); }
static void SyncSafe(Bytes& b, uint32_t v) { b.push_back((v >> 21) & 0x7F); b.push_back((v >> 14) & 0x7F); b.push_back((v >> 7) & 0x7F); b.push_back(v & 0x7F); }
static Bytes Utf16(const std::wstring& s, bool bom, bool be = false) {
    Bytes r;
    if (bom) { r.push_back(0xFF); r.push_back(0xFE); }
    for (wchar_t c : s) { if (be) { r.push_back(uint8_t(c >> 8)); r.push_back(uint8_t(c)); } else { r.push_back(uint8_t(c)); r.push_back(uint8_t(c >> 8)); } }
    return r;
}
static std::string U8(const std::wstring& w) {
    std::string s(WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), (int)s.size(), nullptr, nullptr);
    return s;
}

// ID3v2.3 / 2.4 frame
static Bytes Id3Frame(int ver, const char* id, uint8_t enc, const Bytes& text) {
    Bytes f;
    Put(f, id);
    const uint32_t size = uint32_t(text.size() + 1);
    if (ver == 4) SyncSafe(f, size); else Be32(f, size);
    f.push_back(0); f.push_back(0);
    f.push_back(enc);
    f.insert(f.end(), text.begin(), text.end());
    return f;
}
static Bytes Id3Tag(int ver, const std::vector<Bytes>& frames) {
    Bytes body;
    for (auto& f : frames) body.insert(body.end(), f.begin(), f.end());
    body.insert(body.end(), 20, 0);  // padding
    Bytes t;
    Put(t, "ID3");
    t.push_back(uint8_t(ver)); t.push_back(0); t.push_back(0);
    SyncSafe(t, uint32_t(body.size()));
    t.insert(t.end(), body.begin(), body.end());
    return t;
}

static Bytes VorbisComment(const std::vector<std::string>& comments, const std::string& vendor = "vendor") {
    Bytes b;
    Le32(b, uint32_t(vendor.size())); Put(b, vendor);
    Le32(b, uint32_t(comments.size()));
    for (auto& c : comments) { Le32(b, uint32_t(c.size())); Put(b, c); }
    return b;
}

static Bytes OggPage(const Bytes& payload, uint8_t flags, uint32_t seq, const std::vector<uint8_t>& lacing) {
    Bytes p;
    Put(p, "OggS");
    p.push_back(0); p.push_back(flags);
    p.insert(p.end(), 8, 0); Le32(p, 1); Le32(p, seq); Le32(p, 0);
    p.push_back(uint8_t(lacing.size()));
    p.insert(p.end(), lacing.begin(), lacing.end());
    p.insert(p.end(), payload.begin(), payload.end());
    return p;
}

static Bytes Atom(const char* type, const Bytes& body) {
    Bytes a;
    Be32(a, uint32_t(8 + body.size()));
    Put(a, type);
    a.insert(a.end(), body.begin(), body.end());
    return a;
}
static Bytes Cat(std::initializer_list<Bytes> parts) { Bytes r; for (auto& p : parts) r.insert(r.end(), p.begin(), p.end()); return r; }
static Bytes Mp4Item(uint8_t c0, const char* three, const std::string& text) {
    Bytes d; Be32(d, 1); Be32(d, 0); Put(d, text);
    std::string type; type += char(c0); type += three;
    Bytes item = Atom("data", d);
    Bytes a; Be32(a, uint32_t(8 + item.size())); Put(a, type); a.insert(a.end(), item.begin(), item.end());
    return a;
}

int main() {
    const std::wstring zhTitle = L"月光奏鳴曲", zhArtist = L"貝多芬", album = L"Piano Sonatas";

    // ID3v2.3: UTF-16 with BOM, and ISO-8859-1 text
    {
        Bytes data = Id3Tag(3, {Id3Frame(3, "TIT2", 1, Utf16(zhTitle, true)), Id3Frame(3, "TPE1", 0, Bytes{'A', 'r', 't', 'i', 's', 't'}), Id3Frame(3, "TALB", 1, Utf16(album, true))});
        Put(data, std::string(2000, '\xFF'));  // audio
        auto t = ReadAudioTags(data.data(), data.size());
        EXPECT(t.title == zhTitle && t.artist == L"Artist" && t.album == album);
    }
    // ID3v2.4: UTF-8 and UTF-16BE
    {
        Bytes data = Id3Tag(4, {Id3Frame(4, "TIT2", 3, ToBytes(U8(zhTitle))), Id3Frame(4, "TPE1", 2, Utf16(zhArtist, false, true))});
        Put(data, std::string(500, '\x55'));
        auto t = ReadAudioTags(data.data(), data.size());
        EXPECT(t.title == zhTitle && t.artist == zhArtist);
    }
    // ID3v1 at the end of a tag-less stream
    {
        Bytes data(3000, 0x11);
        Bytes v1(128, 0);
        Put(v1, "");
        std::memcpy(v1.data(), "TAG", 3);
        std::memcpy(v1.data() + 3, "Old Song", 8);
        std::memcpy(v1.data() + 33, "Old Band", 8);
        data.insert(data.end(), v1.begin(), v1.end());
        auto t = ReadAudioTags(data.data(), data.size());
        EXPECT(t.title == L"Old Song" && t.artist == L"Old Band");
    }
    // FLAC: STREAMINFO block then a Vorbis comment block
    {
        Bytes data;
        Put(data, "fLaC");
        data.push_back(0); data.push_back(0); data.push_back(0); data.push_back(34); data.insert(data.end(), 34, 0);  // STREAMINFO
        Bytes vc = VorbisComment({"title=" + U8(zhTitle), "ARTIST=" + U8(zhArtist), "Album=Moon"});
        data.push_back(0x84); data.push_back(uint8_t(vc.size() >> 16)); data.push_back(uint8_t(vc.size() >> 8)); data.push_back(uint8_t(vc.size()));
        data.insert(data.end(), vc.begin(), vc.end());
        Put(data, std::string(100, '\x77'));
        auto t = ReadAudioTags(data.data(), data.size());
        EXPECT(t.title == zhTitle && t.artist == zhArtist && t.album == L"Moon");
    }
    // Ogg Opus: the comment packet is the second packet
    {
        Bytes head; Put(head, "OpusHead"); head.insert(head.end(), 11, 0);
        Bytes tags; Put(tags, "OpusTags"); Bytes vc = VorbisComment({"TITLE=Opus Title", "ARTIST=Opus Artist"}); tags.insert(tags.end(), vc.begin(), vc.end());
        Bytes data = OggPage(head, 2, 0, {uint8_t(head.size())});
        Bytes p2 = OggPage(tags, 0, 1, {uint8_t(tags.size())});
        data.insert(data.end(), p2.begin(), p2.end());
        auto t = ReadAudioTags(data.data(), data.size());
        EXPECT(t.title == L"Opus Title" && t.artist == L"Opus Artist");
    }
    // Ogg Vorbis: a big (cover art) comment first, so the packet spans two pages
    {
        Bytes id; Put(id, "\x01vorbis"); id.insert(id.end(), 23, 0);
        std::string big(300, 'x');
        Bytes tags; Put(tags, "\x03vorbis"); Bytes vc = VorbisComment({"METADATA_BLOCK_PICTURE=" + big, "TITLE=Spanning Title", "ARTIST=Spanning Artist"}); tags.insert(tags.end(), vc.begin(), vc.end());
        // split the packet: 255-byte segments on page one (continued), the rest on page two
        const size_t first = 255 * 1;
        Bytes a(tags.begin(), tags.begin() + first), b(tags.begin() + first, tags.end());
        std::vector<uint8_t> lace2;
        size_t rest = b.size();
        while (rest >= 255) { lace2.push_back(255); rest -= 255; }
        lace2.push_back(uint8_t(rest));
        Bytes data = OggPage(id, 2, 0, {uint8_t(id.size())});
        Bytes p2 = OggPage(a, 0, 1, {255});
        Bytes p3 = OggPage(b, 1, 2, lace2);
        data.insert(data.end(), p2.begin(), p2.end());
        data.insert(data.end(), p3.begin(), p3.end());
        auto t = ReadAudioTags(data.data(), data.size());
        EXPECT(t.title == L"Spanning Title" && t.artist == L"Spanning Artist");
    }
    // MP4 / M4A: ftyp, then moov/udta/meta/ilst
    {
        Bytes ilst = Atom("ilst", Cat({Mp4Item(0xA9, "nam", U8(zhTitle)), Mp4Item(0xA9, "ART", U8(zhArtist)), Mp4Item(0xA9, "alb", "Album X")}));
        Bytes metaBody; Be32(metaBody, 0); metaBody.insert(metaBody.end(), ilst.begin(), ilst.end());
        Bytes moov = Atom("moov", Atom("udta", Atom("meta", metaBody)));
        Bytes ftyp = Atom("ftyp", Bytes{'M', '4', 'A', ' ', 0, 0, 0, 0});
        Bytes mdat = Atom("mdat", Bytes(1000, 0x33));
        Bytes data = Cat({ftyp, mdat, moov});  // moov after mdat, like many files
        auto t = ReadAudioTags(data.data(), data.size());
        EXPECT(t.title == zhTitle && t.artist == zhArtist && t.album == L"Album X");
    }
    // WAV: LIST INFO with INAM / IART
    {
        Bytes info; Put(info, "INFO");
        auto sub = [&](const char* id, const std::string& s) { Put(info, id); Le32(info, uint32_t(s.size() + 1)); Put(info, s); info.push_back(0); if ((s.size() + 1) & 1) info.push_back(0); };
        sub("INAM", U8(zhTitle)); sub("IART", "Wav Artist");
        Bytes data; Put(data, "RIFF"); Le32(data, 0); Put(data, "WAVE");
        Put(data, "fmt "); Le32(data, 16); data.insert(data.end(), 16, 0);
        Put(data, "LIST"); Le32(data, uint32_t(info.size())); data.insert(data.end(), info.begin(), info.end());
        Put(data, "data"); Le32(data, 8); data.insert(data.end(), 8, 0);
        auto t = ReadAudioTags(data.data(), data.size());
        EXPECT(t.title == zhTitle && t.artist == L"Wav Artist");
    }
    // WMA: ASF header with a content description object
    {
        const uint8_t kHeader[16] = {0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11, 0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C};
        const uint8_t kContent[16] = {0x33, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11, 0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C};
        Bytes ti = Utf16(L"WMA Title\0", false), ar = Utf16(L"WMA Artist\0", false);
        ti = Utf16(std::wstring(L"WMA Title") + L'\0', false); ar = Utf16(std::wstring(L"WMA Artist") + L'\0', false);
        Bytes obj(kContent, kContent + 16);
        uint64_t osz = 24 + 10 + ti.size() + ar.size();
        for (int i = 0; i < 8; ++i) obj.push_back(uint8_t(osz >> (8 * i)));
        uint16_t lens[5] = {uint16_t(ti.size()), uint16_t(ar.size()), 0, 0, 0};
        for (auto l : lens) { obj.push_back(uint8_t(l)); obj.push_back(uint8_t(l >> 8)); }
        obj.insert(obj.end(), ti.begin(), ti.end()); obj.insert(obj.end(), ar.begin(), ar.end());
        Bytes data(kHeader, kHeader + 16);
        uint64_t hsz = 30 + obj.size();
        for (int i = 0; i < 8; ++i) data.push_back(uint8_t(hsz >> (8 * i)));
        Le32(data, 1); data.push_back(1); data.push_back(2);
        data.insert(data.end(), obj.begin(), obj.end());
        auto t = ReadAudioTags(data.data(), data.size());
        EXPECT(t.title == L"WMA Title" && t.artist == L"WMA Artist");
    }
    // Nothing to read / not audio
    {
        Bytes text(5000, 'a');
        EXPECT(ReadAudioTags(text.data(), text.size()).Empty());
        EXPECT(ReadAudioTags(nullptr, 0).Empty());
        Bytes tiny{'I', 'D', '3'};
        EXPECT(ReadAudioTags(tiny.data(), tiny.size()).Empty());
    }
    // Garbage and truncated tags must neither crash nor loop: every prefix of valid files, and random mutations
    {
        std::mt19937 rng(7);
        Bytes id3 = Id3Tag(3, {Id3Frame(3, "TIT2", 1, Utf16(zhTitle, true)), Id3Frame(3, "TPE1", 0, Bytes{'A', 'b'})});
        Put(id3, std::string(200, '\x01'));
        Bytes flac; Put(flac, "fLaC"); { Bytes vc = VorbisComment({"TITLE=T", "ARTIST=A"}); flac.push_back(0x84); flac.push_back(0); flac.push_back(0); flac.push_back(uint8_t(vc.size())); flac.insert(flac.end(), vc.begin(), vc.end()); }
        Bytes mp4; { Bytes ilst = Atom("ilst", Mp4Item(0xA9, "nam", "Song")); Bytes mb; Be32(mb, 0); mb.insert(mb.end(), ilst.begin(), ilst.end()); mp4 = Cat({Atom("ftyp", Bytes{'M', '4', 'A', ' ', 0, 0, 0, 0}), Atom("moov", Atom("udta", Atom("meta", mb)))}); }
        for (const Bytes* src : {&id3, &flac, &mp4}) {
            for (size_t n = 0; n <= src->size(); ++n) ReadAudioTags(src->data(), n);
            for (int i = 0; i < 3000; ++i) {
                Bytes m = *src;
                for (int k = 0; k < 1 + int(rng() % 4); ++k) m[rng() % m.size()] = uint8_t(rng());
                ReadAudioTags(m.data(), m.size());
            }
        }
        for (int i = 0; i < 2000; ++i) {
            Bytes r(rng() % 600 + 12);
            for (auto& c : r) c = uint8_t(rng());
            if (i % 2) std::memcpy(r.data(), "OggS", 4);
            ReadAudioTags(r.data(), r.size());
        }
    }

    if (failures) std::fprintf(stderr, "%d failure(s)\n", failures);
    else std::printf("audiotags: all passed\n");
    return failures ? 1 : 0;
}
