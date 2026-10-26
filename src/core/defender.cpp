#include "lycan/defender.h"

#include <windows.h>
#include <winevt.h>

#include <algorithm>
#include <cwctype>

namespace lycan {

namespace {

// The text of <Data Name='name'>...</Data> in the XML of an event ("" when not there)
std::wstring Field(const std::wstring& xml, const wchar_t* name) {
    const std::wstring key = std::wstring(L"<Data Name='") + name + L"'>";
    const size_t at = xml.find(key);
    if (at == std::wstring::npos) return {};
    const size_t from = at + key.size();
    const size_t to = xml.find(L"</Data>", from);
    if (to == std::wstring::npos) return {};
    std::wstring v = xml.substr(from, to - from);
    struct Rep { const wchar_t* from; const wchar_t* to; };
    for (const Rep& r : {Rep{L"&lt;", L"<"}, Rep{L"&gt;", L">"}, Rep{L"&quot;", L"\""}, Rep{L"&apos;", L"'"}, Rep{L"&amp;", L"&"}}) {
        size_t p = 0;
        const std::wstring f = r.from;
        while ((p = v.find(f, p)) != std::wstring::npos) { v.replace(p, f.size(), r.to); ++p; }
    }
    return v;
}

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

std::wstring IsoTime(uint64_t ft) {
    FILETIME f;
    f.dwLowDateTime = static_cast<DWORD>(ft & 0xFFFFFFFFu);
    f.dwHighDateTime = static_cast<DWORD>(ft >> 32);
    SYSTEMTIME st{};
    FileTimeToSystemTime(&f, &st);
    wchar_t buf[40];
    swprintf_s(buf, L"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return buf;
}

uint64_t ParseIso(const std::wstring& s) {  // "2026-10-11T17:37:21.9701241Z" -> FILETIME; 0 when it does not parse
    SYSTEMTIME st{};
    unsigned ms = 0;
    if (swscanf_s(s.c_str(), L"%hu-%hu-%huT%hu:%hu:%hu", &st.wYear, &st.wMonth, &st.wDay, &st.wHour, &st.wMinute, &st.wSecond) != 6) return 0;
    const size_t dot = s.find(L'.');
    if (dot != std::wstring::npos) {
        unsigned digits = 0;
        for (size_t i = dot + 1; i < s.size() && iswdigit(s[i]) && digits < 3; ++i, ++digits) ms = ms * 10 + (s[i] - L'0');
        while (digits++ < 3) ms *= 10;
    }
    st.wMilliseconds = static_cast<WORD>(ms);
    FILETIME f;
    if (!SystemTimeToFileTime(&st, &f)) return 0;
    return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
}

}  // namespace

std::vector<DefenderDetection> DefenderDetectionsSince(uint64_t sinceFileTime) {
    std::vector<DefenderDetection> out;
    wchar_t self[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, self, static_cast<DWORD>(std::size(self)));
    if (!n || n >= std::size(self)) return out;
    const std::wstring mine = Lower(std::wstring(self, n));

    const std::wstring query = L"*[System[(EventID=1116) and TimeCreated[@SystemTime>='" + IsoTime(sinceFileTime) + L"']]]";
    EVT_HANDLE q = EvtQuery(nullptr, L"Microsoft-Windows-Windows Defender/Operational", query.c_str(), EvtQueryChannelPath | EvtQueryForwardDirection);
    if (!q) return out;
    EVT_HANDLE events[16];
    DWORD got = 0;
    while (EvtNext(q, static_cast<DWORD>(std::size(events)), events, 2000, 0, &got) && got) {
        for (DWORD i = 0; i < got; ++i) {
            DWORD used = 0, props = 0;
            std::wstring xml;
            if (!EvtRender(nullptr, events[i], EvtRenderEventXml, 0, nullptr, &used, &props) && GetLastError() == ERROR_INSUFFICIENT_BUFFER && used < (1u << 20)) {
                xml.resize(used / sizeof(wchar_t) + 1);
                if (EvtRender(nullptr, events[i], EvtRenderEventXml, static_cast<DWORD>(xml.size() * sizeof(wchar_t)), xml.data(), &used, &props)) xml.resize(wcslen(xml.c_str()));
                else xml.clear();
            }
            EvtClose(events[i]);
            if (xml.empty()) continue;
            if (Field(xml, L"Source Name") != L"AMSI") continue;
            if (Lower(Field(xml, L"Process Name")) != mine) continue;
            DefenderDetection d;
            d.name = Field(xml, L"Threat Name");
            d.severity = Field(xml, L"Severity Name");
            d.category = Field(xml, L"Category Name");
            d.time = ParseIso(Field(xml, L"Detection Time"));
            if (!d.name.empty()) out.push_back(std::move(d));
        }
    }
    EvtClose(q);
    std::stable_sort(out.begin(), out.end(), [](const DefenderDetection& a, const DefenderDetection& b) { return a.time < b.time; });
    return out;
}

}  // namespace lycan
