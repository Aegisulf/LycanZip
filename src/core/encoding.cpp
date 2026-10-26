#include "lycan/encoding.h"

#include <windows.h>

#include <algorithm>
#include <cmath>

namespace lycan {
namespace {

struct Score {
    double sum = 0;
    int units = 0;  // not ASCII
    int invalid = 0;
    double Mean() const { return units ? (sum - 3.0 * invalid) / units : 0; }
};

bool Between(unsigned v, unsigned lo, unsigned hi) { return v >= lo && v <= hi; }

// ---- UTF-8 ----------------------------------------------------------------
Score ScoreUtf8(const std::string& s) {
    Score r;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) { ++i; continue; }
        int len = (c >= 0xC2 && c <= 0xDF) ? 2 : (c >= 0xE0 && c <= 0xEF) ? 3 : (c >= 0xF0 && c <= 0xF4) ? 4 : 0;
        bool ok = len && i + len <= s.size();
        for (int k = 1; ok && k < len; ++k) ok = (static_cast<unsigned char>(s[i + k]) & 0xC0) == 0x80;
        ++r.units;
        if (!ok) { ++r.invalid; ++i; continue; }
        r.sum += 2.0;  // a valid UTF-8 multi-byte sequence is very unlikely to be a coincidence
        i += len;
    }
    return r;
}

// ---- Big5 (CP950) -----------------------------------------------------------
Score ScoreBig5(const std::string& s) {
    Score r;
    size_t i = 0;
    while (i < s.size()) {
        unsigned lead = static_cast<unsigned char>(s[i]);
        if (lead < 0x80) { ++i; continue; }
        ++r.units;
        unsigned trail = i + 1 < s.size() ? static_cast<unsigned char>(s[i + 1]) : 0;
        bool trailOk = Between(trail, 0x40, 0x7E) || Between(trail, 0xA1, 0xFE);
        if (lead == 0x80 || lead == 0xFF || !Between(lead, 0x81, 0xFE) || !trailOk) { ++r.invalid; ++i; continue; }
        double w;
        if (Between(lead, 0xA4, 0xC6)) w = 1.0;           // common characters
        else if (Between(lead, 0xA1, 0xA3)) w = 0.6;      // punctuation and symbols
        else if (Between(lead, 0xC9, 0xF9)) w = 0.35;     // less common characters
        else if (lead < 0xA1) w = 0.1;                    // extension area (HKSCS etc.)
        else w = 0.2;
        r.sum += w;
        i += 2;
    }
    return r;
}

// ---- GBK (CP936) --------------------------------------------------------------
Score ScoreGbk(const std::string& s) {
    Score r;
    size_t i = 0;
    while (i < s.size()) {
        unsigned lead = static_cast<unsigned char>(s[i]);
        if (lead < 0x80) { ++i; continue; }
        ++r.units;
        unsigned trail = i + 1 < s.size() ? static_cast<unsigned char>(s[i + 1]) : 0;
        bool trailOk = Between(trail, 0x40, 0x7E) || Between(trail, 0x80, 0xFE);
        if (lead == 0x80 || lead == 0xFF || !trailOk) { ++r.invalid; ++i; continue; }
        bool gb2312Trail = trail >= 0xA1;
        double w;
        if (Between(lead, 0xB0, 0xD7)) w = gb2312Trail ? 1.0 : 0.15;   // GB2312 level 1
        else if (Between(lead, 0xD8, 0xF7)) w = gb2312Trail ? 0.6 : 0.15;  // GB2312 level 2
        else if (Between(lead, 0xA1, 0xA9)) w = gb2312Trail ? 0.6 : 0.15;  // symbols
        else w = 0.2;                                                   // GBK extension
        r.sum += w;
        i += 2;
    }
    return r;
}

// ---- Shift-JIS (CP932) ---------------------------------------------------------
Score ScoreSjis(const std::string& s) {
    Score r;
    size_t i = 0;
    while (i < s.size()) {
        unsigned c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) { ++i; continue; }
        ++r.units;
        if (Between(c, 0xA1, 0xDF)) { r.sum += 0.4; ++i; continue; }  // half-width katakana
        bool isLead = Between(c, 0x81, 0x9F) || Between(c, 0xE0, 0xFC);
        unsigned trail = i + 1 < s.size() ? static_cast<unsigned char>(s[i + 1]) : 0;
        bool trailOk = (Between(trail, 0x40, 0x7E) || Between(trail, 0x80, 0xFC));
        if (!isLead || !trailOk) { ++r.invalid; ++i; continue; }
        double w;
        if (c == 0x82 && Between(trail, 0x9F, 0xF1)) w = 1.3;        // hiragana
        else if (c == 0x83 && Between(trail, 0x40, 0x96)) w = 1.3;   // katakana
        else if (Between(c, 0x88, 0x9F)) w = 1.0;                    // level 1 kanji
        else if (Between(c, 0xE0, 0xEA)) w = 0.9;                    // level 2 kanji
        else if (c == 0x81 || c == 0x82 || c == 0x83 || c == 0x84) w = 0.6;
        else w = 0.2;
        r.sum += w;
        i += 2;
    }
    return r;
}

// ---- EUC-KR / UHC (CP949) ---------------------------------------------------------
Score ScoreKorean(const std::string& s) {
    Score r;
    size_t i = 0;
    while (i < s.size()) {
        unsigned lead = static_cast<unsigned char>(s[i]);
        if (lead < 0x80) { ++i; continue; }
        ++r.units;
        unsigned trail = i + 1 < s.size() ? static_cast<unsigned char>(s[i + 1]) : 0;
        bool trailOk = Between(trail, 0x41, 0x5A) || Between(trail, 0x61, 0x7A) || Between(trail, 0x81, 0xFE);
        if (lead == 0x80 || lead == 0xFF || !trailOk) { ++r.invalid; ++i; continue; }
        double w;
        if (Between(lead, 0xB0, 0xC8) && trail >= 0xA1) w = 1.2;       // KS X 1001 hangul
        else if (Between(lead, 0xCA, 0xFD) && trail >= 0xA1) w = 0.3;  // hanja
        else if (Between(lead, 0xA1, 0xAF) && trail >= 0xA1) w = 0.5;  // symbols
        else if (Between(lead, 0x81, 0xA0)) w = 0.5;                   // UHC extension hangul
        else w = 0.15;
        r.sum += w;
        i += 2;
    }
    return r;
}

Score Run(unsigned cp, const std::string& s) {
    switch (cp) {
        case kCpUtf8: return ScoreUtf8(s);
        case kCpBig5: return ScoreBig5(s);
        case kCpGbk: return ScoreGbk(s);
        case kCpShiftJis: return ScoreSjis(s);
        default: return ScoreKorean(s);
    }
}

}  // namespace

EncodingGuess DetectNameEncoding(const std::vector<std::string>& names, unsigned systemAcp) {
    EncodingGuess g;
    std::string all;
    for (auto& n : names)
        if (std::any_of(n.begin(), n.end(), [](char c) { return static_cast<unsigned char>(c) >= 0x80; })) {
            all += n;
            all += '\n';  // separator, so that the bytes of neighbouring names do not join up
        }
    if (all.empty()) return g;

    static const unsigned cands[] = {kCpUtf8, kCpBig5, kCpGbk, kCpShiftJis, kCpEucKr};
    int bestUnits = 0;
    for (unsigned cp : cands) {
        Score s = Run(cp, all);
        bestUnits = std::max(bestUnits, s.units);
        // If a name contains an invalid sequence it counts against UTF-8 (its validity is decided in one go, but the invalid ones are not counted)
        double mean = (cp == kCpUtf8 && s.invalid) ? -1.0 : s.Mean();
        g.ranked.push_back({cp, mean});
    }
    // Two safeguards: the ANSI code page of the system gets a small bonus (Big5 / GBK are close, and this leans towards the language of the user)
    for (auto& c : g.ranked)
        if (c.codepage == systemAcp) c.score += 0.05;
    std::stable_sort(g.ranked.begin(), g.ranked.end(),
                     [](const EncodingCandidate& a, const EncodingCandidate& b) { return a.score > b.score; });

    const auto& best = g.ranked[0];
    if (best.score < 0.55) return g;  // no candidate stands out: keep the sample (the caller uses the OEM decoding)
    g.codepage = best.codepage;
    g.score = best.score;
    // With little evidence (a few characters) a small score gap means nothing: ask for confirmation more readily.
    const double margin = bestUnits < 8 ? 0.35 : 0.15;
    g.ambiguous = g.ranked.size() > 1 && best.score - g.ranked[1].score < margin && g.ranked[1].score >= 0.55;
    return g;
}

std::wstring DecodeName(const std::string& raw, unsigned codepage) {
    if (raw.empty()) return {};
    int n = MultiByteToWideChar(codepage, 0, raw.data(), static_cast<int>(raw.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(codepage, 0, raw.data(), static_cast<int>(raw.size()), w.data(), n);
    return w;
}

bool TryRecoverRawBytes(const std::wstring& decoded, std::string& raw) {
    raw.clear();
    if (decoded.empty()) return true;
    BOOL usedDefault = FALSE;
    int n = WideCharToMultiByte(kCpOem437, WC_NO_BEST_FIT_CHARS, decoded.data(), static_cast<int>(decoded.size()),
                                nullptr, 0, nullptr, &usedDefault);
    if (n <= 0 || usedDefault) return false;
    raw.resize(static_cast<size_t>(n));
    usedDefault = FALSE;
    WideCharToMultiByte(kCpOem437, WC_NO_BEST_FIT_CHARS, decoded.data(), static_cast<int>(decoded.size()), raw.data(), n,
                        nullptr, &usedDefault);
    return !usedDefault;
}

const wchar_t* CodepageName(unsigned cp) {
    switch (cp) {
        case kCpUtf8: return L"UTF-8";
        case kCpBig5: return L"Big5 (CP950)";
        case kCpGbk: return L"GBK (CP936)";
        case kCpShiftJis: return L"Shift-JIS (CP932)";
        case kCpEucKr: return L"EUC-KR (CP949)";
        case kCpOem437: return L"CP437";
        default: return L"?";
    }
}

}  // namespace lycan
