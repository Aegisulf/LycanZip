#pragma once
// File name encoding detection. The names in ZIP / TAR etc. have no encoding mark, a common source of garbled names:
// archives made with Big5 / GBK / Shift-JIS / EUC-KR and unpacked on a system of another language.
// The input is the raw bytes; they are scored by structural plausibility (double-byte ranges, common character blocks); pure functions, unit-testable.
#include <string>
#include <vector>

namespace lycan {

constexpr unsigned kCpUtf8 = 65001;
constexpr unsigned kCpBig5 = 950;
constexpr unsigned kCpGbk = 936;
constexpr unsigned kCpShiftJis = 932;
constexpr unsigned kCpEucKr = 949;
constexpr unsigned kCpOem437 = 437;

struct EncodingCandidate {
    unsigned codepage = 0;
    double score = 0;  // average score per non-ASCII unit, about 0..2, higher = more likely
};

struct EncodingGuess {
    unsigned codepage = 0;               // 0 = cannot tell (no non-ASCII bytes, or all candidates score too low)
    double score = 0;
    bool ambiguous = false;              // the first two score close (for example Big5 vs GBK); the user should confirm
    std::vector<EncodingCandidate> ranked;  // from high to low
};

// names: the raw file name bytes of the items. The ANSI code page of the system is used as the preference in a tie.
EncodingGuess DetectNameEncoding(const std::vector<std::string>& names, unsigned systemAcp);

// Decodes with the given code page; bytes that cannot be decoded are replaced by U+FFFD.
std::wstring DecodeName(const std::string& raw, unsigned codepage);

// Reverse: turns a string that 7-Zip decoded as CP437 back into the original bytes. If the string has a character that CP437 cannot represent
// (the name was Unicode all along, for example the UTF-8 flag), returns false.
bool TryRecoverRawBytes(const std::wstring& decoded437, std::string& raw);

const wchar_t* CodepageName(unsigned codepage);

}  // namespace lycan
