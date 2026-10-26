// libFuzzer target: operations on an existing archive. The bytes are a list of operations (add / delete / rename / make a folder / replace) with arbitrary
// names; they are previewed, estimated and applied to a copy of a small zip. A bad operation must be refused with lycan::Error, never crash, and an
// archive that was changed must still open and list.
#include <windows.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "lycan/archive_source.h"
#include "lycan/compress.h"
#include "lycan/error.h"
#include "lycan/update.h"

using namespace lycan;
namespace fs = std::filesystem;

namespace {
fs::path g_dir, g_seed, g_add;
}

extern "C" int LLVMFuzzerInitialize(int*, char***) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    g_dir = fs::temp_directory_path() / (L"lycan_fuzz_upd_" + std::to_wstring(GetCurrentProcessId()));
    fs::create_directories(g_dir / L"src" / L"a" / L"b");
    for (const char* n : {"x.txt", "y.txt", "a/z.txt", "a/b/deep.txt"}) {
        std::ofstream f(g_dir / L"src" / fs::path(std::string(n)), std::ios::binary);
        f << "content of " << n;
    }
    g_seed = g_dir / L"seed.zip";
    g_add = g_dir / L"added.txt";
    {
        std::ofstream f(g_add, std::ios::binary);
        f << "added";
    }
    CompressOptions co;
    co.type = ArchiveType::Zip;
    CreateArchive(g_seed, {g_dir / L"src"}, co);
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 3 || size > 4096) return 0;
    const fs::path arc = g_dir / L"work.zip";
    std::error_code ec;
    fs::copy_file(g_seed, arc, fs::copy_options::overwrite_existing, ec);
    // [kind][mode][length][UTF-16 name]... : two names per operation
    std::vector<UpdateOp> ops;
    size_t i = 1;
    auto name = [&]() {
        std::wstring s;
        if (i >= size) return s;
        size_t n = data[i++] % 40;
        while (n-- && i + 1 < size) { s += static_cast<wchar_t>(data[i] | (data[i + 1] << 8)); i += 2; }
        return s;
    };
    while (i < size && ops.size() < 8) {
        const uint8_t kind = data[i++] % 5;
        std::wstring a = name();
        switch (kind) {
            case 0: ops.push_back(UpdateOp::Add(a, {g_add})); break;
            case 1: ops.push_back(UpdateOp::Delete(a)); break;
            case 2: ops.push_back(UpdateOp::Rename(a, name())); break;
            case 3: ops.push_back(UpdateOp::MakeDir(a)); break;
            default: ops.push_back(UpdateOp::Replace(a, g_add)); break;
        }
    }
    UpdateOptions uo;
    uo.mode = static_cast<UpdateMode>(data[0] % 4);
    uo.compress.storeCompressed = false;
    try { PreviewUpdate(arc, ops, uo); } catch (const Error&) {} catch (const std::exception&) {}
    try { EstimateUpdate(arc, ops, uo); } catch (const Error&) {} catch (const std::exception&) {}
    bool applied = false;
    try { applied = UpdateArchive(arc, ops, uo).changed; } catch (const Error&) {} catch (const std::exception&) {}
    if (applied) {
        // whatever was done, the result is an archive
        auto s = ArchiveSource::Open(arc);
        (void)s->Entries();
    }
    return 0;
}
