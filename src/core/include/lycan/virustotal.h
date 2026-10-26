#pragma once
// A link to the VirusTotal page of a file, built from its SHA-256. The user opens it in a browser: nothing is uploaded and no API is used;
// the only thing that reaches VirusTotal is the hash that the user chose to look up. Header-only on purpose (a few lines, and a unit test).
#include <string>
#include <string_view>

namespace lycan {

// The address for a SHA-256 value (64 hexadecimal digits, upper or lower case); empty when the text is not one. Only digits are put in the address,
// so nothing else can get into it.
inline std::wstring VirusTotalUrl(std::string_view sha256Hex) {
    if (sha256Hex.size() != 64) return {};
    std::wstring url = L"https://www.virustotal.com/gui/search/";
    for (char c : sha256Hex) {
        if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
        const bool digit = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!digit) return {};
        url += static_cast<wchar_t>(c);
    }
    return url;
}

}  // namespace lycan
