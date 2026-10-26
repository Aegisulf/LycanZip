// Which volume of a split archive a file name is (volumes.h)
#include "no_dialogs.h"

#include <cstdio>

#include "lycan/volumes.h"

using namespace lycan;
namespace fs = std::filesystem;

static int failures = 0;
#define EXPECT(cond)                                                     \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); \
            ++failures;                                                  \
        }                                                                \
    } while (0)

static void Later(const wchar_t* file, unsigned index, const wchar_t* first) {
    const VolumeInfo v = DetectVolume(file);
    if (v.index != index || v.first != fs::path(first)) {
        std::fprintf(stderr, "FAIL %ls: index %u (want %u), first '%ls' (want '%ls')\n", file, v.index, index, v.first.c_str(), first);
        ++failures;
    }
}
static void NotLater(const wchar_t* file, unsigned index) {
    const VolumeInfo v = DetectVolume(file);
    if (v.index != index || v.IsLater()) {
        std::fprintf(stderr, "FAIL %ls: index %u (want %u)\n", file, v.index, index);
        ++failures;
    }
}

int main() {
    // RAR with partN
    Later(L"D:\\dl\\Movie.part2.rar", 2, L"D:\\dl\\Movie.part1.rar");
    Later(L"D:\\dl\\Movie.part03.rar", 3, L"D:\\dl\\Movie.part01.rar");
    Later(L"D:\\dl\\Movie.part012.rar", 12, L"D:\\dl\\Movie.part001.rar");
    Later(L"D:\\dl\\Movie.PART7.RAR", 7, L"D:\\dl\\Movie.PART1.RAR");  // case is kept
    Later(L"D:\\dl\\my.movie.part2.rar", 2, L"D:\\dl\\my.movie.part1.rar");
    NotLater(L"D:\\dl\\Movie.part1.rar", 1);
    NotLater(L"D:\\dl\\Movie.part01.rar", 1);
    NotLater(L"D:\\dl\\Movie.rar", 1);
    NotLater(L"D:\\dl\\Movie.part.rar", 1);  // "part" without a number: an ordinary name
    // old style RAR
    Later(L"D:\\dl\\old.r00", 2, L"D:\\dl\\old.rar");
    Later(L"D:\\dl\\old.r01", 3, L"D:\\dl\\old.rar");
    Later(L"D:\\dl\\old.R12", 14, L"D:\\dl\\old.rar");
    // split zip
    Later(L"D:\\dl\\s.z01", 2, L"D:\\dl\\s.zip");
    Later(L"D:\\dl\\s.z09", 10, L"D:\\dl\\s.zip");
    NotLater(L"D:\\dl\\s.zip", 0);
    // plain splits
    Later(L"D:\\dl\\a.7z.002", 2, L"D:\\dl\\a.7z.001");
    Later(L"D:\\dl\\a.7z.0010", 10, L"D:\\dl\\a.7z.0001");
    Later(L"D:\\dl\\big.iso.003", 3, L"D:\\dl\\big.iso.001");
    NotLater(L"D:\\dl\\a.7z.001", 1);
    // not volumes
    NotLater(L"D:\\dl\\a.zip", 0);
    NotLater(L"D:\\dl\\a.7z", 0);
    NotLater(L"D:\\dl\\readme.txt", 0);
    NotLater(L"D:\\dl\\a.r", 0);       // too short to be .rNN
    NotLater(L"D:\\dl\\a.rxx", 0);
    NotLater(L"D:\\dl\\a.12", 0);      // two digits: not a split
    NotLater(L"D:\\dl\\noext", 0);
    NotLater(L"D:\\dl\\trailing.", 0);
    // no folder: the first volume has none either
    Later(L"Movie.part2.rar", 2, L"Movie.part1.rar");
    Later(L"é中文.part2.rar", 2, L"é中文.part1.rar");

    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::printf("OK\n");
    return 0;
}
