// Unit tests of file name encoding detection: real file names are encoded with each code page, then the detection result and the recovery are checked.
#include "no_dialogs.h"
#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "lycan/encoding.h"

using namespace lycan;

static int failures = 0;
#define EXPECT(cond)                                                     \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); \
            ++failures;                                                  \
        }                                                                \
    } while (0)

static std::string Enc(const std::wstring& w, unsigned cp) {
    int n = WideCharToMultiByte(cp, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(cp, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

static void Case(const char* label, unsigned cp, const std::vector<std::wstring>& names, unsigned acp) {
    std::vector<std::string> raw;
    for (auto& n : names) raw.push_back(Enc(n, cp));
    auto g = DetectNameEncoding(raw, acp);
    bool ok = g.codepage == cp;
    std::printf("%-28s acp=%u -> %u (score %.2f)%s %s\n", label, acp, g.codepage, g.score, g.ambiguous ? " ambiguous" : "",
                ok ? "ok" : "MISMATCH");
    if (!ok) {
        ++failures;
        for (auto& c : g.ranked) std::printf("    cp%u %.2f\n", c.codepage, c.score);
    }
    // Recovery check: decode back to the original string
    if (ok)
        for (size_t i = 0; i < names.size(); ++i) EXPECT(DecodeName(raw[i], cp) == names[i]);
}

int main() {
    const std::vector<std::wstring> tw = {L"測試資料夾/報告書.docx", L"測試資料夾/會議紀錄.txt", L"使用手冊.pdf"};
    const std::vector<std::wstring> cn = {L"测试资料夹/报告书.docx", L"测试资料夹/会议记录.txt", L"使用手册.pdf"};
    const std::vector<std::wstring> jp = {L"テスト/日本語のファイル.txt", L"設計書/説明資料.pptx", L"ひらがなカタカナ.txt"};
    const std::vector<std::wstring> kr = {L"한국어/파일.txt", L"프로젝트/설계문서.docx", L"안녕하세요.txt"};
    const std::vector<std::wstring> one_tw = {L"我的文件.txt"};

    Case("Big5 (acp 950)", kCpBig5, tw, 950);
    Case("Big5 (acp 936)", kCpBig5, tw, 936);   // a non-Traditional-Chinese system should also get it right
    Case("Big5 (acp 1252)", kCpBig5, tw, 1252);
    Case("GBK (acp 936)", kCpGbk, cn, 936);
    Case("GBK (acp 950)", kCpGbk, cn, 950);     // a Traditional Chinese system receives a Simplified Chinese archive
    Case("GBK (acp 1252)", kCpGbk, cn, 1252);
    Case("Shift-JIS (acp 950)", kCpShiftJis, jp, 950);
    Case("Shift-JIS (acp 1252)", kCpShiftJis, jp, 1252);
    Case("EUC-KR (acp 950)", kCpEucKr, kr, 950);
    Case("Big5 single short name", kCpBig5, one_tw, 950);

    // UTF-8 bytes (the common case of Info-ZIP without the flag)
    {
        std::vector<std::string> raw;
        for (auto& n : tw) raw.push_back(Enc(n, CP_UTF8));
        auto g = DetectNameEncoding(raw, 950);
        EXPECT(g.codepage == kCpUtf8);
    }
    // pure ASCII: no decision
    {
        auto g = DetectNameEncoding({"readme.txt", "dir/file.bin"}, 950);
        EXPECT(g.codepage == 0);
    }
    // Recover CP437: ASCII and high bytes make the round trip; a character that CP437 does not have counts as Unicode all along
    {
        std::string raw;
        EXPECT(TryRecoverRawBytes(DecodeName(Enc(L"測試.txt", 950), kCpOem437), raw));
        EXPECT(raw == Enc(L"測試.txt", 950));
        EXPECT(!TryRecoverRawBytes(L"測試.txt", raw));
    }

    if (failures == 0) std::puts("test_encoding: all passed");
    return failures ? 1 : 0;
}
