// libFuzzer target: write an ISO image of arbitrary item names (WriteIso: the tree, the 8.3 and the Joliet names, the directory records, the path tables)
// and check that what comes out is a well-formed image: the descriptor says the size of the file, every directory record is inside its sector and its
// extent, the path tables agree between the two byte orders, and the walk of both trees finds the same number of items.
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "lycan/error.h"
#include "lycan/iso.h"

using namespace lycan;
namespace fs = std::filesystem;

namespace {
constexpr uint32_t kSector = 2048;

uint32_t Le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
uint32_t Be32(const uint8_t* p) { return p[3] | (p[2] << 8) | (p[1] << 16) | (static_cast<uint32_t>(p[0]) << 24); }

[[noreturn]] void Bad(const char* what) {
    std::fprintf(stderr, "ISO check failed: %s\n", what);
    std::abort();
}

// Walks a directory; returns the number of items below it. Aborts on a malformed record.
uint64_t Walk(const std::vector<uint8_t>& img, uint32_t extent, uint32_t size, int depth) {
    if (depth > 300) Bad("directories nested too deep");
    if (static_cast<uint64_t>(extent) * kSector + size > img.size()) Bad("a directory extent is outside the image");
    uint64_t count = 0;
    uint32_t o = 0;
    while (o < size) {
        const uint8_t* d = &img[static_cast<size_t>(extent) * kSector + o];
        const uint8_t len = d[0];
        if (len == 0) { o = (o / kSector + 1) * kSector; continue; }
        if (o % kSector + len > kSector) Bad("a record crosses a sector");
        if (len < 34 || len < 33 + d[32]) Bad("a record is shorter than its identifier");
        if (len % 2) Bad("a record has an odd length");
        const uint32_t ext = Le32(d + 2), sz = Le32(d + 10);
        if (ext != Be32(d + 6) || sz != Be32(d + 14)) Bad("the two byte orders of a record differ");
        const bool isDir = (d[25] & 2) != 0;
        const bool special = d[32] == 1 && (d[33] == 0 || d[33] == 1);
        if (!special) {
            ++count;
            if (isDir) count += Walk(img, ext, sz, depth + 1);
            else if (static_cast<uint64_t>(ext) * kSector + sz > img.size()) Bad("a file extent is outside the image");
        }
        o += len;
    }
    return count;
}

void CheckImage(const std::vector<uint8_t>& img) {
    if (img.size() < 19 * kSector || img.size() % kSector) Bad("the size is not a whole number of sectors");
    uint64_t counts[2] = {0, 0};
    for (int t = 0; t < 2; ++t) {
        const uint8_t* d = &img[static_cast<size_t>(16 + t) * kSector];
        if (d[0] != (t == 0 ? 1 : 2) || std::memcmp(d + 1, "CD001", 5) != 0) Bad("a volume descriptor is missing");
        if (static_cast<uint64_t>(Le32(d + 80)) * kSector != img.size()) Bad("the volume size is not the size of the file");
        const uint32_t pathSize = Le32(d + 132), lPath = Le32(d + 140), mPath = Be32(d + 148);
        if (static_cast<uint64_t>(lPath) * kSector + pathSize > img.size() || static_cast<uint64_t>(mPath) * kSector + pathSize > img.size()) Bad("a path table is outside the image");
        for (uint32_t i = 0; i < pathSize; ++i) {   // the same table in the two byte orders: the lengths and the identifiers are equal
            // (only the extent and the parent number differ in byte order: compare them as numbers)
        }
        uint32_t o = 0;
        while (o < pathSize) {
            const uint8_t* l = &img[static_cast<size_t>(lPath) * kSector + o];
            const uint8_t* m = &img[static_cast<size_t>(mPath) * kSector + o];
            const uint8_t idLen = l[0];
            if (idLen == 0 || idLen != m[0]) Bad("the path tables differ in an identifier length");
            if (Le32(l + 2) != Be32(m + 2)) Bad("the path tables differ in an extent");
            if (std::memcmp(l + 8, m + 8, idLen) != 0) Bad("the path tables differ in an identifier");
            o += 8 + idLen + (idLen % 2);
        }
        if (o != pathSize) Bad("the path table does not end where it says");
        counts[t] = Walk(img, Le32(d + 158), Le32(d + 166), 0);
    }
    if (counts[0] != counts[1]) Bad("the two trees do not have the same number of items");
}
}  // namespace

extern "C" int LLVMFuzzerInitialize(int*, char***) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 2 || size > 8192) return 0;
    static const fs::path dir = fs::temp_directory_path() / (L"lycan_fuzz_iso_" + std::to_wstring(GetCurrentProcessId()));
    static const fs::path content = dir / L"content.bin";
    static bool made = false;
    if (!made) {
        fs::create_directories(dir);
        std::ofstream f(content, std::ios::binary);
        f << std::string(300, 'c');
        made = true;
    }
    // the bytes are a list of items: each is [flags][size][name as UTF-16 code units up to a 0 unit]
    std::vector<IsoItem> items;
    size_t i = 0;
    while (i + 3 <= size && items.size() < 300) {
        IsoItem it;
        const uint8_t flags = data[i], sz = data[i + 1];
        i += 2;
        std::wstring name;
        while (i + 1 < size) {
            const wchar_t c = static_cast<wchar_t>(data[i] | (data[i + 1] << 8));
            i += 2;
            if (c == 0) break;
            name += c;
            if (name.size() > 300) break;
        }
        it.path = name;
        it.isDir = (flags & 1) != 0;
        it.size = it.isDir ? 0 : (std::min)(static_cast<uint64_t>(sz), uint64_t{300});
        it.source = content;
        it.mtime = (flags & 2) ? 0 : (static_cast<uint64_t>(data[(i - 1) % size]) << 40);   // some are far from 2020 on purpose
        items.push_back(std::move(it));
    }
    const fs::path out = dir / L"out.iso";
    HANDLE h = CreateFileW(out.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    bool wrote = false;
    try {
        IsoOptions opt;
        opt.label.assign(reinterpret_cast<const wchar_t*>(data), (std::min)(size / 2, size_t{20}));
        WriteIso(h, items, opt);
        wrote = true;
    } catch (const Error&) {   // no items, or something the format cannot hold: refused, not a crash
    } catch (const std::exception&) {
    }
    CloseHandle(h);
    if (wrote) {
        std::ifstream f(out, std::ios::binary);
        std::vector<uint8_t> img((std::istreambuf_iterator<char>(f)), {});
        CheckImage(img);
    }
    return 0;
}
