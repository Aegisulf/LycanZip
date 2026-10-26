#include "pch.h"
#include "lycan/i18n.h"
#include "AiMeta.h"

#include <algorithm>
#include <map>
#include <set>

namespace lycan_app {
namespace {

constexpr size_t kMaxText = 2u << 20;  // upper limit of one text block (after unpacking): 2 MB

// ---- Minimal zlib / DEFLATE decompression (RFC 1950 / 1951), only for reading the compressed text blocks of PNG
struct Bits {
    const uint8_t* p;
    size_t n, pos = 0;
    uint32_t bitBuf = 0;
    int bitCnt = 0;
    bool eof = false;
    int Need(int need) {
        long v = bitBuf;
        while (bitCnt < need) {
            if (pos >= n) { eof = true; return 0; }
            v |= static_cast<long>(p[pos++]) << bitCnt;
            bitCnt += 8;
        }
        bitBuf = static_cast<uint32_t>(v >> need);
        bitCnt -= need;
        return static_cast<int>(v & ((1L << need) - 1));
    }
};

struct Huff {
    short count[16];
    short symbol[288];
};

int Decode(Bits& s, const Huff& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; ++len) {
        code |= s.Need(1);
        if (s.eof) return -1;
        int count = h.count[len];
        if (code - count < first) return h.symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

int Construct(Huff& h, const short* length, int n) {
    for (int i = 0; i < 16; ++i) h.count[i] = 0;
    for (int i = 0; i < n; ++i) ++h.count[length[i]];
    if (h.count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len <= 15; ++len) {
        left <<= 1;
        left -= h.count[len];
        if (left < 0) return left;
    }
    short offs[16];
    offs[1] = 0;
    for (int len = 1; len < 15; ++len) offs[len + 1] = static_cast<short>(offs[len] + h.count[len]);
    for (int i = 0; i < n; ++i)
        if (length[i] != 0) h.symbol[offs[length[i]]++] = static_cast<short>(i);
    return left;
}

bool InflateRaw(const uint8_t* in, size_t n, std::vector<uint8_t>& out) {
    static const short lens[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const short lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const short dists[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static const short dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    Bits s{in, n};
    int last;
    do {
        last = s.Need(1);
        int type = s.Need(2);
        if (s.eof) return false;
        if (type == 0) {
            s.bitBuf = 0;
            s.bitCnt = 0;
            if (s.pos + 4 > n) return false;
            unsigned len = in[s.pos] | (in[s.pos + 1] << 8);
            unsigned nlen = in[s.pos + 2] | (in[s.pos + 3] << 8);
            s.pos += 4;
            if (len != (~nlen & 0xFFFF) || s.pos + len > n || out.size() + len > kMaxText) return false;
            out.insert(out.end(), in + s.pos, in + s.pos + len);
            s.pos += len;
        } else if (type == 1 || type == 2) {
            Huff lencode, distcode;
            if (type == 1) {
                short l[288];
                int i = 0;
                for (; i < 144; ++i) l[i] = 8;
                for (; i < 256; ++i) l[i] = 9;
                for (; i < 280; ++i) l[i] = 7;
                for (; i < 288; ++i) l[i] = 8;
                Construct(lencode, l, 288);
                for (i = 0; i < 30; ++i) l[i] = 5;
                Construct(distcode, l, 30);
            } else {
                int nlen = s.Need(5) + 257, ndist = s.Need(5) + 1, ncode = s.Need(4) + 4;
                if (s.eof || nlen > 286 || ndist > 30) return false;
                static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
                short lengths[320];
                int idx = 0;
                for (; idx < ncode; ++idx) lengths[order[idx]] = static_cast<short>(s.Need(3));
                for (; idx < 19; ++idx) lengths[order[idx]] = 0;
                Huff lc;
                if (Construct(lc, lengths, 19) != 0) return false;
                idx = 0;
                while (idx < nlen + ndist) {
                    int symbol = Decode(s, lc);
                    if (symbol < 0) return false;
                    if (symbol < 16) lengths[idx++] = static_cast<short>(symbol);
                    else {
                        int len = 0, rep;
                        if (symbol == 16) {
                            if (idx == 0) return false;
                            len = lengths[idx - 1];
                            rep = 3 + s.Need(2);
                        } else if (symbol == 17) rep = 3 + s.Need(3);
                        else rep = 11 + s.Need(7);
                        if (idx + rep > nlen + ndist) return false;
                        while (rep--) lengths[idx++] = static_cast<short>(len);
                    }
                }
                if (lengths[256] == 0) return false;
                if (Construct(lencode, lengths, nlen) < 0) return false;
                if (Construct(distcode, lengths + nlen, ndist) < 0) return false;
            }
            for (;;) {
                int symbol = Decode(s, lencode);
                if (symbol < 0) return false;
                if (symbol < 256) {
                    if (out.size() >= kMaxText) return false;
                    out.push_back(static_cast<uint8_t>(symbol));
                } else if (symbol == 256) break;
                else {
                    symbol -= 257;
                    if (symbol >= 29) return false;
                    int len = lens[symbol] + s.Need(lext[symbol]);
                    int ds = Decode(s, distcode);
                    if (ds < 0 || ds >= 30) return false;
                    size_t dist = static_cast<size_t>(dists[ds] + s.Need(dext[ds]));
                    if (s.eof || dist > out.size() || out.size() + len > kMaxText) return false;
                    size_t from = out.size() - dist;
                    for (int i = 0; i < len; ++i) out.push_back(out[from + i]);
                }
            }
        } else return false;
    } while (!last);
    return true;
}

bool InflateZlib(const uint8_t* in, size_t n, std::vector<uint8_t>& out) {
    if (n < 6 || (in[0] & 0x0F) != 8) return false;
    return InflateRaw(in + 2, n - 2, out);
}

// ---- Text conversion
std::wstring Utf8OrLatin1(const uint8_t* p, size_t n) {
    if (n == 0) return {};
    int w = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, reinterpret_cast<const char*>(p), static_cast<int>(n), nullptr, 0);
    if (w > 0) {
        std::wstring s(static_cast<size_t>(w), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, reinterpret_cast<const char*>(p), static_cast<int>(n), s.data(), w);
        return s;
    }
    std::wstring s(n, L'\0');  // not valid UTF-8: treat it as Latin-1
    for (size_t i = 0; i < n; ++i) s[i] = p[i];
    return s;
}

std::wstring Trim(std::wstring s) {
    auto sp = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L'\0'; };
    while (!s.empty() && sp(s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && sp(s[i])) ++i;
    return s.substr(i);
}

uint32_t BE32(const uint8_t* p) { return (static_cast<uint32_t>(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

// Text blocks of PNG: keyword -> text (the first one for the same name)
std::vector<std::pair<std::wstring, std::wstring>> ReadPngText(const std::vector<uint8_t>& b) {
    std::vector<std::pair<std::wstring, std::wstring>> out;
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (b.size() < 12 || memcmp(b.data(), sig, 8) != 0) return out;
    size_t pos = 8;
    while (pos + 12 <= b.size()) {
        const uint32_t len = BE32(&b[pos]);
        const char* type = reinterpret_cast<const char*>(&b[pos + 4]);
        const size_t data = pos + 8;
        if (static_cast<uint64_t>(data) + len + 4 > b.size()) break;
        if (!memcmp(type, "IEND", 4)) break;
        const bool isT = !memcmp(type, "tEXt", 4), isZ = !memcmp(type, "zTXt", 4), isI = !memcmp(type, "iTXt", 4);
        if ((isT || isZ || isI) && len < (8u << 20)) {
            const uint8_t* d = &b[data];
            size_t kw = 0;
            while (kw < len && d[kw] != 0) ++kw;
            if (kw < len && kw > 0 && kw < 80) {
                std::wstring key = Utf8OrLatin1(d, kw);
                std::wstring text;
                if (isT) text = Utf8OrLatin1(d + kw + 1, len - kw - 1);
                else if (isZ) {
                    if (kw + 2 <= len) {
                        std::vector<uint8_t> raw;
                        if (InflateZlib(d + kw + 2, len - kw - 2, raw)) text = Utf8OrLatin1(raw.data(), raw.size());
                    }
                } else {
                    // iTXt: keyword\0 compFlag compMethod lang\0 translated\0 text
                    size_t i = kw + 1;
                    if (i + 2 <= len) {
                        const bool comp = d[i] != 0;
                        i += 2;
                        while (i < len && d[i] != 0) ++i;  // language tag
                        ++i;
                        while (i < len && d[i] != 0) ++i;  // translated keyword
                        ++i;
                        if (i <= len) {
                            if (comp) {
                                std::vector<uint8_t> raw;
                                if (InflateZlib(d + i, len - i, raw)) text = Utf8OrLatin1(raw.data(), raw.size());
                            } else text = Utf8OrLatin1(d + i, len - i);
                        }
                    }
                }
                if (!text.empty()) out.push_back({key, text});
            }
        }
        pos = data + len + 4;
    }
    return out;
}

std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return s;
}

// ---- The "parameters" text of Stable Diffusion WebUI / Forge
// positive prompt (may be several lines)\nNegative prompt: negative prompt\nSteps: 20, Sampler: Euler a, CFG scale: 7, Seed: 1, Size: 512x512, Model: xxx, ...
void ParseA1111(const std::wstring& text, AiMeta& m) {
    std::wstring head = text, paramLine;
    size_t p = text.rfind(L"\nSteps: ");
    if (p != std::wstring::npos) { head = text.substr(0, p); paramLine = text.substr(p + 1); }
    else if (text.rfind(L"Steps: ", 0) == 0) { head.clear(); paramLine = text; }
    size_t neg = head.find(L"\nNegative prompt:");
    if (neg != std::wstring::npos) {
        m.positive = Trim(head.substr(0, neg));
        m.negative = Trim(head.substr(neg + 17));
    } else if (head.rfind(L"Negative prompt:", 0) == 0) {
        m.negative = Trim(head.substr(16));
    } else m.positive = Trim(head);
    // Parameter line: split at ", " (not inside quotes), then each at the first ": " into key / value
    std::vector<std::wstring> parts;
    std::wstring cur;
    bool quote = false;
    for (size_t i = 0; i < paramLine.size(); ++i) {
        wchar_t c = paramLine[i];
        if (c == L'"') quote = !quote;
        if (!quote && c == L',' && i + 1 < paramLine.size() && paramLine[i + 1] == L' ') { parts.push_back(cur); cur.clear(); ++i; continue; }
        cur.push_back(c);
    }
    if (!cur.empty()) parts.push_back(cur);
    for (auto& part : parts) {
        size_t c = part.find(L": ");
        if (c == std::wstring::npos) continue;
        m.params.push_back({Trim(part.substr(0, c)), Trim(part.substr(c + 2))});
    }
    m.source = lycan::Tr(L"preview.stable_diffusion_webui_a1111_forge");
    m.found = !m.positive.empty() || !m.negative.empty() || !m.params.empty();
}

using winrt::Windows::Data::Json::IJsonValue;
using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Data::Json::JsonValueType;
using winrt::hstring;

std::wstring JsonText(IJsonValue const& v) {
    switch (v.ValueType()) {
        case JsonValueType::String: return std::wstring(v.GetString());
        case JsonValueType::Number: {
            wchar_t b[48];
            double d = v.GetNumber();
            if (d == static_cast<long long>(d)) swprintf_s(b, L"%lld", static_cast<long long>(d)); else swprintf_s(b, L"%g", d);
            return b;
        }
        case JsonValueType::Boolean: return v.GetBoolean() ? L"true" : L"false";
        default: return {};
    }
}

std::wstring Str(JsonObject const& o, const wchar_t* k) {
    if (o.HasKey(k)) return JsonText(o.GetNamedValue(k));
    return {};
}

void AddParam(AiMeta& m, const wchar_t* label, std::wstring v) {
    if (!v.empty()) m.params.push_back({label, std::move(v)});
}

// NovelAI: Comment is JSON with prompt / uc (negative) / steps / scale / seed / sampler ...
bool ParseNovelAI(JsonObject const& o, AiMeta& m) {
    if (!o.HasKey(L"prompt") || !(o.HasKey(L"uc") || o.HasKey(L"steps") || o.HasKey(L"scale"))) return false;
    m.source = L"NovelAI";
    m.positive = Trim(Str(o, L"prompt"));
    m.negative = Trim(Str(o, L"uc"));
    AddParam(m, L"Steps", Str(o, L"steps"));
    AddParam(m, L"Scale", Str(o, L"scale"));
    AddParam(m, L"Sampler", Str(o, L"sampler"));
    AddParam(m, L"Seed", Str(o, L"seed"));
    AddParam(m, L"Strength", Str(o, L"strength"));
    AddParam(m, L"Size", Str(o, L"width").empty() ? L"" : Str(o, L"width") + L"x" + Str(o, L"height"));
    m.found = true;
    return true;
}

// InvokeAI: invokeai_metadata JSON
bool ParseInvoke(JsonObject const& o, AiMeta& m) {
    if (!o.HasKey(L"positive_prompt") && !o.HasKey(L"negative_prompt")) return false;
    m.source = L"InvokeAI";
    m.positive = Trim(Str(o, L"positive_prompt"));
    m.negative = Trim(Str(o, L"negative_prompt"));
    AddParam(m, L"Steps", Str(o, L"steps"));
    AddParam(m, L"CFG scale", Str(o, L"cfg_scale"));
    AddParam(m, L"Scheduler", Str(o, L"scheduler"));
    AddParam(m, L"Seed", Str(o, L"seed"));
    AddParam(m, L"Size", Str(o, L"width").empty() ? L"" : Str(o, L"width") + L"x" + Str(o, L"height"));
    if (o.HasKey(L"model") && o.GetNamedValue(L"model").ValueType() == JsonValueType::Object) AddParam(m, L"Model", Str(o.GetNamedObject(L"model"), L"model_name"));
    m.found = true;
    return true;
}

// ComfyUI: prompt is a node graph (API format). From the positive / negative links of KSampler find the text of CLIPTextEncode.
bool ParseComfy(JsonObject const& o, AiMeta& m) {
    bool any = false;
    auto node = [&](const std::wstring& id) -> JsonObject {
        if (o.HasKey(id) && o.GetNamedValue(id).ValueType() == JsonValueType::Object) return o.GetNamedObject(id);
        return nullptr;
    };
    auto linked = [&](JsonObject const& inputs, const wchar_t* key) -> std::wstring {
        if (!inputs.HasKey(key)) return {};
        auto v = inputs.GetNamedValue(key);
        if (v.ValueType() != JsonValueType::Array) return {};
        auto a = v.GetArray();
        if (a.Size() < 1) return {};
        auto n = node(JsonText(a.GetAt(0)));
        if (!n || !n.HasKey(L"inputs")) return {};
        auto in = n.GetNamedObject(L"inputs");
        // may be CLIPTextEncode (text), or connected further up (for example ConditioningCombine): only direct text is handled
        return in.HasKey(L"text") ? Trim(JsonText(in.GetNamedValue(L"text"))) : std::wstring();
    };
    for (auto const& kv : o) {
        if (kv.Value().ValueType() != JsonValueType::Object) continue;
        auto n = kv.Value().GetObject();
        const std::wstring cls = Str(n, L"class_type");
        if (!n.HasKey(L"inputs")) continue;
        auto in = n.GetNamedObject(L"inputs");
        if (cls.rfind(L"KSampler", 0) == 0 && !any) {
            m.positive = linked(in, L"positive");
            m.negative = linked(in, L"negative");
            AddParam(m, L"Seed", Str(in, L"seed").empty() ? Str(in, L"noise_seed") : Str(in, L"seed"));
            AddParam(m, L"Steps", Str(in, L"steps"));
            AddParam(m, L"CFG", Str(in, L"cfg"));
            AddParam(m, L"Sampler", Str(in, L"sampler_name"));
            AddParam(m, L"Scheduler", Str(in, L"scheduler"));
            AddParam(m, L"Denoise", Str(in, L"denoise"));
            any = true;
        } else if (cls == L"CheckpointLoaderSimple") AddParam(m, L"Model", Str(in, L"ckpt_name"));
        else if (cls == L"EmptyLatentImage") AddParam(m, L"Size", Str(in, L"width") + L"x" + Str(in, L"height"));
    }
    if (!any) return false;
    m.source = L"ComfyUI";
    m.found = true;
    return true;
}

}  // namespace

std::wstring DecodeUserComment(const uint8_t* d, size_t n) {
    if (n <= 8) return {};
    const uint8_t* p = d + 8;
    const size_t len = n - 8;
    if (!memcmp(d, "UNICODE\0", 8)) {
        // usually UTF-16 big endian (A1111); with a BOM follow the BOM
        bool be = true;
        size_t off = 0;
        if (len >= 2 && p[0] == 0xFF && p[1] == 0xFE) { be = false; off = 2; }
        else if (len >= 2 && p[0] == 0xFE && p[1] == 0xFF) off = 2;
        std::wstring s;
        for (size_t i = off; i + 1 < len; i += 2) s.push_back(static_cast<wchar_t>(be ? (p[i] << 8 | p[i + 1]) : (p[i + 1] << 8 | p[i])));
        return Trim(s);
    }
    if (!memcmp(d, "ASCII\0\0\0", 8) || !memcmp(d, "\0\0\0\0\0\0\0\0", 8)) return Trim(Utf8OrLatin1(p, len));
    return {};
}

AiMeta ParseAiMeta(const std::vector<uint8_t>& bytes, const std::wstring& exifComment) {
    AiMeta m;
    try {
        auto texts = ReadPngText(bytes);
        std::map<std::wstring, std::wstring> byKey;
        for (auto& [k, v] : texts) byKey.emplace(Lower(k), v);
        auto get = [&](const wchar_t* k) -> const std::wstring* {
            auto it = byKey.find(k);
            return it == byKey.end() ? nullptr : &it->second;
        };
        auto tryJson = [](const std::wstring& s) -> JsonObject {
            JsonObject o{nullptr};
            if (s.size() > 1 && Trim(s)[0] == L'{' && JsonObject::TryParse(hstring{s}, o)) return o;
            return nullptr;
        };
        std::set<std::wstring> used;

        if (auto p = get(L"parameters")) { ParseA1111(*p, m); used.insert(L"parameters"); }
        else if (!exifComment.empty() && (exifComment.find(L"Steps: ") != std::wstring::npos || exifComment.find(L"Negative prompt:") != std::wstring::npos)) {
            ParseA1111(exifComment, m);
        }
        if (!m.found) {
            if (auto c = get(L"comment")) if (auto o = tryJson(*c)) if (ParseNovelAI(o, m)) { used.insert(L"comment"); used.insert(L"description"); }
        }
        if (!m.found) {
            if (auto c = get(L"invokeai_metadata")) if (auto o = tryJson(*c)) if (ParseInvoke(o, m)) used.insert(L"invokeai_metadata");
        }
        if (!m.found) {
            if (auto c = get(L"prompt")) if (auto o = tryJson(*c)) if (ParseComfy(o, m)) { used.insert(L"prompt"); used.insert(L"workflow"); }
        }
        // recognised as AI data but cannot be split up: keep the original text
        if (!m.found) {
            for (const wchar_t* k : {L"parameters", L"prompt", L"comment", L"invokeai_metadata", L"sd-metadata"})
                if (auto p = get(k)) { m.found = true; m.source = lycan::Tf(L"preview.embedded_text_data", std::wstring(k)); m.raw = p->substr(0, 4000); used.insert(k); break; }
        }
        // other text metadata (listed only when it is surely an AI picture or the content is short, so that a whole workflow JSON is not dumped)
        if (m.found) {
            for (auto& [k, v] : texts) {
                const std::wstring lk = Lower(k);
                if (used.count(lk) || lk == L"workflow") continue;
                if (v.size() > 400) continue;
                m.other.push_back({k, v});
            }
        }
    } catch (...) {
        m = AiMeta{};
    }
    return m;
}

}  // namespace lycan_app
