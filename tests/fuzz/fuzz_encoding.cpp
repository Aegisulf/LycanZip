// libFuzzer target: file-name encoding detection and decoding on arbitrary bytes.
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "lycan/encoding.h"

using namespace lycan;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 2) return 0;
    const unsigned acp = (data[0] & 1) ? kCpBig5 : 1252;
    std::vector<std::string> names;
    std::string cur;
    for (size_t i = 1; i < size; ++i) {
        if (data[i] == 0 || data[i] == '\n') {
            names.push_back(cur);
            cur.clear();
        } else {
            cur += static_cast<char>(data[i]);
        }
    }
    names.push_back(cur);
    EncodingGuess g = DetectNameEncoding(names, acp);
    (void)g;
    for (unsigned cp : {kCpUtf8, kCpBig5, kCpGbk, kCpShiftJis, kCpEucKr, kCpOem437}) {
        for (auto& n : names) {
            std::wstring w = DecodeName(n, cp);
            std::string raw;
            if (cp == kCpOem437) (void)TryRecoverRawBytes(w, raw);
        }
        (void)CodepageName(cp);
    }
    return 0;
}
