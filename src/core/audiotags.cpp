#include "lycan/audiotags.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace lycan {
namespace {

struct Span {
    const uint8_t* p;
    size_t n;
};

uint32_t Be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
uint32_t Le32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
uint64_t Be64(const uint8_t* p) { return (uint64_t(Be32(p)) << 32) | Be32(p + 4); }
uint32_t SyncSafe(const uint8_t* p) { return (uint32_t(p[0] & 0x7F) << 21) | (uint32_t(p[1] & 0x7F) << 14) | (uint32_t(p[2] & 0x7F) << 7) | (p[3] & 0x7F); }

std::wstring Clean(std::wstring s) {
    for (auto& c : s)
        if (c < 0x20) c = L' ';
    auto notSpace = [](wchar_t c) { return c != L' ' && c != L'　' && c != 0xFEFF; };
    auto b = std::find_if(s.begin(), s.end(), notSpace);
    auto e = std::find_if(s.rbegin(), s.rend(), notSpace).base();
    s = b < e ? std::wstring(b, e) : std::wstring();
    if (s.size() > 256) s.resize(256);
    return s;
}

std::wstring Latin1(const uint8_t* p, size_t n) {
    std::wstring s;
    s.reserve(n);
    for (size_t i = 0; i < n; ++i) s.push_back(static_cast<wchar_t>(p[i]));
    return s;
}

// UTF-8 when valid, otherwise the ANSI code page of the machine (old tags are often in the local encoding)
std::wstring Utf8OrAnsi(const uint8_t* p, size_t n) {
    if (n == 0) return {};
    if (n > (1u << 20)) n = 1u << 20;
    for (UINT cp : {static_cast<UINT>(CP_UTF8), static_cast<UINT>(CP_ACP)}) {
        const int len = MultiByteToWideChar(cp, cp == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0, reinterpret_cast<const char*>(p), static_cast<int>(n), nullptr, 0);
        if (len <= 0) continue;
        std::wstring w(static_cast<size_t>(len), L'\0');
        MultiByteToWideChar(cp, cp == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0, reinterpret_cast<const char*>(p), static_cast<int>(n), w.data(), len);
        return w;
    }
    return Latin1(p, n);
}

std::wstring Utf16(const uint8_t* p, size_t n, bool bigEndian) {
    std::wstring s;
    for (size_t i = 0; i + 1 < n; i += 2) s.push_back(static_cast<wchar_t>(bigEndian ? (p[i] << 8) | p[i + 1] : (p[i + 1] << 8) | p[i]));
    return s;
}

void Fill(std::wstring& dst, std::wstring v) {
    v = Clean(std::move(v));
    if (dst.empty() && !v.empty()) dst = std::move(v);
}

// ---- ID3v2 --------------------------------------------------------------------------------------------------------

std::wstring Id3Text(const uint8_t* p, size_t n) {
    if (n < 2) return {};
    const uint8_t enc = p[0];
    p += 1;
    n -= 1;
    std::wstring s;
    if (enc == 0) {
        size_t len = 0;
        while (len < n && p[len]) ++len;
        // ISO-8859-1 on paper; tags written on Chinese / Japanese systems are in the local code page
        s = Utf8OrAnsi(p, len);
    } else if (enc == 1 || enc == 2) {
        bool be = enc == 2;
        if (enc == 1 && n >= 2) {
            if (p[0] == 0xFE && p[1] == 0xFF) { be = true; p += 2; n -= 2; }
            else if (p[0] == 0xFF && p[1] == 0xFE) { be = false; p += 2; n -= 2; }
        }
        size_t len = 0;
        while (len + 1 < n && !(p[len] == 0 && p[len + 1] == 0)) len += 2;
        s = Utf16(p, len, be);
    } else {
        size_t len = 0;
        while (len < n && p[len]) ++len;
        s = Utf8OrAnsi(p, len);
    }
    return s;
}

// returns the offset just after the ID3v2 tag (0 when there is none)
size_t ReadId3v2(Span d, AudioTags& t) {
    if (d.n < 10 || std::memcmp(d.p, "ID3", 3) != 0) return 0;
    const uint8_t ver = d.p[3], flags = d.p[5];
    if (ver < 2 || ver > 4) return 0;
    const size_t tagSize = SyncSafe(d.p + 6);
    const size_t end = std::min<size_t>(d.n, 10 + tagSize);
    size_t pos = 10;
    if (flags & 0x40) {  // extended header
        if (pos + 4 > end) return end;
        const size_t ext = ver == 4 ? SyncSafe(d.p + pos) : size_t(Be32(d.p + pos)) + 4;
        pos += ext;
    }
    const size_t idLen = ver == 2 ? 3 : 4, hdr = ver == 2 ? 6 : 10;
    while (pos + hdr <= end) {
        const uint8_t* f = d.p + pos;
        if (f[0] == 0) break;  // padding
        size_t size = ver == 2 ? (size_t(f[3]) << 16) | (size_t(f[4]) << 8) | f[5] : ver == 4 ? SyncSafe(f + 4) : Be32(f + 4);
        size_t body = pos + hdr;
        if (size > end - body) break;
        size_t skip = 0;
        if (ver == 4 && (f[9] & 0x01)) skip = 4;  // data length indicator
        std::string id(reinterpret_cast<const char*>(f), idLen);
        if (size > skip) {
            const uint8_t* b = d.p + body + skip;
            const size_t bn = size - skip;
            if (id == "TIT2" || id == "TT2") Fill(t.title, Id3Text(b, bn));
            else if (id == "TPE1" || id == "TP1") Fill(t.artist, Id3Text(b, bn));
            else if (id == "TALB" || id == "TAL") Fill(t.album, Id3Text(b, bn));
        }
        pos = body + size;
    }
    return end;
}

void ReadId3v1(Span d, AudioTags& t) {
    if (d.n < 128 || std::memcmp(d.p + d.n - 128, "TAG", 3) != 0) return;
    const uint8_t* b = d.p + d.n - 128;
    auto field = [&](const uint8_t* p, size_t n) {
        size_t len = 0;
        while (len < n && p[len]) ++len;
        return Utf8OrAnsi(p, len);
    };
    Fill(t.title, field(b + 3, 30));
    Fill(t.artist, field(b + 33, 30));
    Fill(t.album, field(b + 63, 30));
}

// ---- Vorbis comments (FLAC, Ogg Vorbis, Opus) ---------------------------------------------------------------------

void ReadVorbisComment(const uint8_t* p, size_t n, AudioTags& t) {
    if (n < 8) return;
    size_t pos = 0;
    const uint32_t vendor = Le32(p);
    pos = 4;
    if (vendor > n - pos) return;
    pos += vendor;
    if (pos + 4 > n) return;
    uint32_t count = Le32(p + pos);
    pos += 4;
    for (uint32_t i = 0; i < count && pos + 4 <= n; ++i) {
        const uint32_t len = Le32(p + pos);
        pos += 4;
        if (len > n - pos) break;
        const uint8_t* c = p + pos;
        pos += len;
        const uint8_t* eq = static_cast<const uint8_t*>(std::memchr(c, '=', len));
        if (!eq) continue;
        std::string key(reinterpret_cast<const char*>(c), static_cast<size_t>(eq - c));
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
        const uint8_t* v = eq + 1;
        const size_t vn = len - static_cast<size_t>(eq - c) - 1;
        if (key == "TITLE") Fill(t.title, Utf8OrAnsi(v, vn));
        else if (key == "ARTIST") Fill(t.artist, Utf8OrAnsi(v, vn));
        else if (key == "ALBUM") Fill(t.album, Utf8OrAnsi(v, vn));
    }
}

void ReadFlac(Span d, size_t pos, AudioTags& t) {
    if (pos + 4 > d.n || std::memcmp(d.p + pos, "fLaC", 4) != 0) return;
    pos += 4;
    while (pos + 4 <= d.n) {
        const uint8_t h = d.p[pos];
        const size_t len = (size_t(d.p[pos + 1]) << 16) | (size_t(d.p[pos + 2]) << 8) | d.p[pos + 3];
        pos += 4;
        if (len > d.n - pos) return;
        if ((h & 0x7F) == 4) ReadVorbisComment(d.p + pos, len, t);
        pos += len;
        if (h & 0x80) return;
    }
}

// The second Ogg packet holds the comments. Packets can span pages, so reassemble them from the page lacing.
void ReadOgg(Span d, AudioTags& t) {
    size_t pos = 0;
    std::vector<uint8_t> cur;
    int packets = 0;
    for (int page = 0; page < 256 && pos + 27 <= d.n; ++page) {
        if (std::memcmp(d.p + pos, "OggS", 4) != 0) return;
        const size_t segs = d.p[pos + 26];
        if (pos + 27 + segs > d.n) return;
        const uint8_t* lace = d.p + pos + 27;
        size_t data = pos + 27 + segs;
        for (size_t i = 0; i < segs; ++i) {
            const size_t l = lace[i];
            if (l > d.n - data) return;
            if (cur.size() + l > (16u << 20)) return;
            cur.insert(cur.end(), d.p + data, d.p + data + l);
            data += l;
            if (l < 255) {  // packet complete
                if (++packets == 2) {
                    const uint8_t* p = cur.data();
                    size_t n = cur.size();
                    if (n > 7 && std::memcmp(p, "\x03vorbis", 7) == 0) ReadVorbisComment(p + 7, n - 7, t);
                    else if (n > 8 && std::memcmp(p, "OpusTags", 8) == 0) ReadVorbisComment(p + 8, n - 8, t);
                    return;
                }
                cur.clear();
            }
        }
        pos = data;
    }
}

// ---- MP4 / M4A ----------------------------------------------------------------------------------------------------

struct Atom {
    uint32_t type;
    size_t body, end;  // payload range
};

bool NextAtom(Span d, size_t pos, size_t limit, Atom& a, size_t& next) {
    if (pos + 8 > limit) return false;
    uint64_t size = Be32(d.p + pos);
    a.type = Be32(d.p + pos + 4);
    size_t hdr = 8;
    if (size == 1) {
        if (pos + 16 > limit) return false;
        size = Be64(d.p + pos + 8);
        hdr = 16;
    } else if (size == 0) {
        size = limit - pos;
    }
    if (size < hdr || size > limit - pos) return false;
    a.body = pos + hdr;
    a.end = pos + static_cast<size_t>(size);
    next = a.end;
    return true;
}

constexpr uint32_t Fourcc(const char (&s)[5]) { return (uint32_t(uint8_t(s[0])) << 24) | (uint32_t(uint8_t(s[1])) << 16) | (uint32_t(uint8_t(s[2])) << 8) | uint8_t(s[3]); }

void ReadMp4(Span d, AudioTags& t) {
    // moov / udta / meta / ilst / (©nam | ©ART | ©alb) / data
    auto find = [&](size_t from, size_t to, uint32_t type, Atom& out) {
        size_t pos = from, next = 0;
        Atom a{};
        while (NextAtom(d, pos, to, a, next)) {
            if (a.type == type) { out = a; return true; }
            pos = next;
        }
        return false;
    };
    Atom moov{}, udta{}, meta{}, ilst{};
    if (!find(0, d.n, Fourcc("moov"), moov)) return;
    if (!find(moov.body, moov.end, Fourcc("udta"), udta)) return;
    if (!find(udta.body, udta.end, Fourcc("meta"), meta)) return;
    if (!find(meta.body + 4, meta.end, Fourcc("ilst"), ilst)) return;  // meta is a full box: 4 bytes of version / flags
    size_t pos = ilst.body, next = 0;
    Atom item{};
    while (NextAtom(d, pos, ilst.end, item, next)) {
        pos = next;
        std::wstring* dst = item.type == 0xA96E616D ? &t.title : item.type == 0xA9415254 ? &t.artist : item.type == 0xA9616C62 ? &t.album : nullptr;
        if (!dst) continue;
        Atom data{};
        if (!find(item.body, item.end, Fourcc("data"), data) || data.body + 8 > data.end) continue;
        Fill(*dst, Utf8OrAnsi(d.p + data.body + 8, data.end - data.body - 8));
    }
}

// ---- WAV (RIFF LIST INFO) -----------------------------------------------------------------------------------------

void ReadWav(Span d, AudioTags& t) {
    size_t pos = 12;
    while (pos + 8 <= d.n) {
        const uint32_t size = Le32(d.p + pos + 4);
        const size_t body = pos + 8;
        const size_t avail = std::min<size_t>(size, d.n - body);
        if (std::memcmp(d.p + pos, "LIST", 4) == 0 && avail >= 4 && std::memcmp(d.p + body, "INFO", 4) == 0) {
            size_t p = body + 4;
            const size_t end = body + avail;
            while (p + 8 <= end) {
                const uint32_t sz = Le32(d.p + p + 4);
                const size_t b = p + 8;
                if (sz > end - b) break;
                size_t len = sz;
                while (len > 0 && d.p[b + len - 1] == 0) --len;
                if (std::memcmp(d.p + p, "INAM", 4) == 0) Fill(t.title, Utf8OrAnsi(d.p + b, len));
                else if (std::memcmp(d.p + p, "IART", 4) == 0) Fill(t.artist, Utf8OrAnsi(d.p + b, len));
                else if (std::memcmp(d.p + p, "IPRD", 4) == 0) Fill(t.album, Utf8OrAnsi(d.p + b, len));
                p = b + sz + (sz & 1);
            }
        } else if (std::memcmp(d.p + pos, "id3 ", 4) == 0 || std::memcmp(d.p + pos, "ID3 ", 4) == 0) {
            ReadId3v2({d.p + body, avail}, t);
        }
        pos = body + size + (size & 1);
    }
}

// ---- WMA (ASF content description) --------------------------------------------------------------------------------

void ReadAsf(Span d, AudioTags& t) {
    static const uint8_t kContent[16] = {0x33, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11, 0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C};
    if (d.n < 30) return;
    size_t pos = 30;
    for (int i = 0; i < 64 && pos + 24 <= d.n; ++i) {
        uint64_t size = 0;
        std::memcpy(&size, d.p + pos + 16, 8);  // little-endian host
        if (size < 24 || size > d.n - pos) return;
        if (std::memcmp(d.p + pos, kContent, 16) == 0 && size >= 24 + 10) {
            const uint8_t* b = d.p + pos + 24;
            size_t n = static_cast<size_t>(size) - 24;
            uint16_t len[5];
            for (int k = 0; k < 5; ++k) len[k] = static_cast<uint16_t>(b[k * 2] | (b[k * 2 + 1] << 8));
            size_t o = 10;
            if (len[0] <= n - o) { Fill(t.title, Utf16(b + o, len[0], false)); o += len[0]; } else return;
            if (len[1] <= n - o) Fill(t.artist, Utf16(b + o, len[1], false));
            return;
        }
        pos += static_cast<size_t>(size);
    }
}

}  // namespace

AudioTags ReadAudioTags(const uint8_t* data, size_t size) {
    AudioTags t;
    if (!data || size < 12) return t;
    const Span d{data, size};
    try {
        const size_t afterId3 = ReadId3v2(d, t);
        if (afterId3 > 0) ReadFlac(d, afterId3, t);  // FLAC files may start with an ID3v2 tag too
        else if (std::memcmp(data, "fLaC", 4) == 0) ReadFlac(d, 0, t);
        else if (std::memcmp(data, "OggS", 4) == 0) ReadOgg(d, t);
        else if (std::memcmp(data, "RIFF", 4) == 0 && std::memcmp(data + 8, "WAVE", 4) == 0) ReadWav(d, t);
        else if (std::memcmp(data + 4, "ftyp", 4) == 0) ReadMp4(d, t);
        else if (size >= 16 && std::memcmp(data, "\x30\x26\xB2\x75\x8E\x66\xCF\x11", 8) == 0) ReadAsf(d, t);
        if (t.title.empty() || t.artist.empty()) ReadId3v1(d, t);
    } catch (...) {
    }
    return t;
}

}  // namespace lycan
