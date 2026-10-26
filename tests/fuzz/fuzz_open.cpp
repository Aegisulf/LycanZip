// libFuzzer target: open an arbitrary file as an archive, list it, read some entries and extract it.
// Only Core's own code is instrumented (7z.dll is not), so this looks for bugs in the wrapper, the tree builder
// and the extraction layer rather than in the 7-Zip format handlers.
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "lycan/archive_source.h"
#include "lycan/error.h"
#include "lycan/extract.h"
#include "lycan/tree.h"

using namespace lycan;
namespace fs = std::filesystem;

namespace {
struct Counter : ByteSink {
    size_t n = 0;
    void Write(const void*, size_t k) override { n += k; }
};
const wchar_t* const kExt[] = {L".zip", L".7z", L".tar", L".tar.gz", L".tar.xz", L".tar.bz2", L".gz",
                               L".rar", L".iso", L".cab", L".7z.001", L".lzh", L".zip"};
}  // namespace

extern "C" int LLVMFuzzerInitialize(int*, char***) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 2 || size > (1u << 20)) return 0;
    static const fs::path dir = fs::temp_directory_path() / (L"lycan_fuzz_" + std::to_wstring(GetCurrentProcessId()));
    static std::atomic<unsigned> n{0};
    fs::create_directories(dir);
    const fs::path file = dir / (L"in" + std::to_wstring(n++ % 4) + kExt[data[0] % (sizeof kExt / sizeof kExt[0])]);
    {
        std::ofstream f(file, std::ios::binary);
        f.write(reinterpret_cast<const char*>(data + 1), static_cast<std::streamsize>(size - 1));
    }
    const fs::path out = dir / L"out";
    try {
        OpenOptions oo;
        oo.password = [](std::wstring&) { return false; };
        auto src = ArchiveSource::Open(file, oo);
        ArchiveTree tree(src->Entries());
        (void)tree.Descendants(tree.Root());
        int read = 0;
        for (auto& e : src->Entries()) {
            if (e.isDir || e.size > (1u << 20) || ++read > 8) continue;
            Counter c;
            try { src->ReadEntry(e.index, c); } catch (const Error&) {}
        }
        ExtractOptions eo;
        eo.overwrite = OverwriteMode::Overwrite;
        eo.maxTotalBytes = 8u << 20;
        eo.propagateMotw = false;
        eo.password = [](std::wstring&) { return false; };
        try { ExtractTo(*src, out, eo); } catch (const Error&) {}
        (void)src->NameEncoding();
    } catch (const Error&) {
    }
    std::error_code ec;
    fs::remove_all(out, ec);
    return 0;
}
