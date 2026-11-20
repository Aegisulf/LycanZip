#include "no_dialogs.h"
// Unit tests of the path safety layer (no external framework, returns non-zero on failure)
#include <cstdio>
#include <string>

#include "lycan/security.h"

static int failures = 0;

#define EXPECT(cond)                                                    \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); \
            ++failures;                                                 \
        }                                                               \
    } while (0)

using lycan::SanitizeEntryPath;
using lycan::HasBidiControls;
using lycan::VisibleName;
using lycan::LooksDisguised;
using lycan::NameWarnings;
using lycan::ScanEntryNames;

int main() {
    // normal
    {
        auto r = SanitizeEntryPath(L"dir/sub/file.txt");
        EXPECT(!r.rejected && !r.modified && r.relative == std::filesystem::path(L"dir/sub/file.txt"));
    }
    EXPECT(!SanitizeEntryPath(L"a\\b\\c.txt").rejected);
    EXPECT(!SanitizeEntryPath(L"./a/./b.txt").rejected);

    // path traversal and absolute paths
    EXPECT(SanitizeEntryPath(L"../evil.txt").rejected);
    EXPECT(SanitizeEntryPath(L"a/../../evil.txt").rejected);
    EXPECT(SanitizeEntryPath(L"a\\..\\..\\evil.txt").rejected);
    EXPECT(SanitizeEntryPath(L"/etc/passwd").rejected);
    EXPECT(SanitizeEntryPath(L"\\\\server\\share\\x").rejected);
    EXPECT(SanitizeEntryPath(L"C:\\Windows\\x.dll").rejected);
    EXPECT(SanitizeEntryPath(L"C:evil.txt").rejected);
    EXPECT(SanitizeEntryPath(L"a/b.txt:hidden").rejected);  // ADS
    EXPECT(SanitizeEntryPath(L"").rejected);
    EXPECT(SanitizeEntryPath(L"/").rejected);

    // Cleaning (not refused, but marked modified)
    {
        auto r = SanitizeEntryPath(L"CON.txt");
        EXPECT(!r.rejected && r.modified && r.relative == std::filesystem::path(L"_CON.txt"));
    }
    {
        auto r = SanitizeEntryPath(L"dir/nul");
        EXPECT(!r.rejected && r.modified && r.relative == std::filesystem::path(L"dir/_nul"));
    }
    EXPECT(SanitizeEntryPath(L"COM1").modified);
    EXPECT(!SanitizeEntryPath(L"COM10").modified);  // COM10 is not a reserved name
    EXPECT(!SanitizeEntryPath(L"console.txt").modified);
    // also reserved: a space before the dot, the console devices, COM and LPT with a superscript digit
    EXPECT(SanitizeEntryPath(L"CON .txt").relative == std::filesystem::path(L"_CON .txt"));
    EXPECT(SanitizeEntryPath(L"CONIN$").modified);
    EXPECT(SanitizeEntryPath(L"conout$.log").modified);
    EXPECT(SanitizeEntryPath(L"COM¹.txt").modified);
    EXPECT(SanitizeEntryPath(L"lpt³").modified);
    EXPECT(!SanitizeEntryPath(L"COM0.txt").modified);
    EXPECT(!SanitizeEntryPath(L"CONIN.txt").modified);
    {
        auto r = SanitizeEntryPath(L"a?b*c.txt");
        EXPECT(!r.rejected && r.modified && r.relative == std::filesystem::path(L"a_b_c.txt"));
    }
    {
        auto r = SanitizeEntryPath(L"dir./file. ");
        EXPECT(!r.rejected && r.modified && r.relative == std::filesystem::path(L"dir/file"));
    }
    // empty after cleaning
    EXPECT(SanitizeEntryPath(L"dir/...").rejected);

    // Names that pretend to be another type
    {
        const std::wstring rlo = L"x\u202Etxt.exe";   // shown by Windows as "xexe.txt"
        EXPECT(HasBidiControls(rlo) && !HasBidiControls(L"readme.txt"));
        EXPECT(VisibleName(rlo) == L"x[RLO]txt.exe" && VisibleName(L"a\u200Fb\u2066c") == L"a[RLM]b[LRI]c" && VisibleName(L"plain.txt") == L"plain.txt");
        EXPECT(LooksDisguised(rlo) && LooksDisguised(L"docs/invoice.PDF.exe") && LooksDisguised(L"photo.jpg      .scr") && LooksDisguised(L"a/b/report.docx..cmd"));
        EXPECT(!LooksDisguised(L"setup.exe") && !LooksDisguised(L"report.pdf") && !LooksDisguised(L"tool.v2.exe") && !LooksDisguised(L"data.tar.gz"));
        const NameWarnings w = ScanEntryNames({L"ok.txt", L"invoice.pdf.exe", L"../evil.txt", rlo, L"C:/abs.txt"});
        EXPECT(w.disguised == 2 && w.unsafePaths == 2 && w.example == L"invoice.pdf.exe");
        EXPECT(ScanEntryNames({L"a.txt", L"setup.exe", L"dir/b.png"}).disguised == 0);
    }

    if (failures == 0) std::puts("test_security: all passed");
    return failures ? 1 : 0;
}
