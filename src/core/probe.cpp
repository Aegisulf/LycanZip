#include "lycan/probe.h"

#include <windows.h>
#include <compressapi.h>

#include <algorithm>
#include <cmath>

namespace lycan {

namespace {

constexpr uint64_t kSampleBytes = 32 * 1024;

// One XPRESS-Huffman compressor, made on demand; null when the system has none.
class TrialCompressor {
public:
    TrialCompressor() {
        if (!CreateCompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, nullptr, &h_)) h_ = nullptr;
    }
    ~TrialCompressor() { if (h_) CloseCompressor(h_); }
    TrialCompressor(const TrialCompressor&) = delete;
    TrialCompressor& operator=(const TrialCompressor&) = delete;
    // Size after compression; 0 when it failed.
    size_t Size(const std::vector<unsigned char>& in) const {
        if (!h_ || in.empty()) return 0;
        std::vector<unsigned char> out(in.size() + in.size() / 8 + 4096);
        SIZE_T n = 0;
        if (!Compress(h_, in.data(), in.size(), out.data(), out.size(), &n)) return 0;
        return n;
    }
private:
    COMPRESSOR_HANDLE h_ = nullptr;
};

}  // namespace

double ByteEntropy(const unsigned char* data, size_t size) {
    if (!data || !size) return 0;
    uint32_t count[256] = {};
    for (size_t i = 0; i < size; ++i) ++count[data[i]];
    double h = 0;
    const double n = static_cast<double>(size);
    for (uint32_t c : count) {
        if (!c) continue;
        const double p = c / n;
        h -= p * std::log2(p);
    }
    return h;
}

bool SamplesLookIncompressible(const std::vector<std::vector<unsigned char>>& samples) {
    if (samples.empty()) return false;
    for (auto const& s : samples) {
        if (s.empty()) return false;
        if (ByteEntropy(s.data(), s.size()) < kProbeLowEntropy) return false;
    }
    const double saved = TrialSavings(samples);
    return saved >= 0 && saved < kProbeMinGain;  // no compressor (-1): pack it
}

double TrialSavings(const std::vector<std::vector<unsigned char>>& samples) {
    TrialCompressor tc;
    size_t in = 0, out = 0;
    for (auto const& s : samples) {
        const size_t c = tc.Size(s);
        if (!c) return -1;
        in += s.size();
        out += (std::min)(c, s.size());
    }
    return in ? 1.0 - static_cast<double>(out) / static_cast<double>(in) : -1;
}

bool FileLooksIncompressible(const std::filesystem::path& file, uint64_t size, double* trialSavings) {
    if (size == 0) return false;
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    // three samples (start, middle, end); five from 16 MB on. Short files get shorter, non-overlapping samples.
    const unsigned places = size >= (16ull << 20) ? 5 : 3;
    const uint64_t len = (std::min)(kSampleBytes, size / places);
    std::vector<std::vector<unsigned char>> samples;
    bool ok = len > 0;
    for (unsigned i = 0; ok && i < places; ++i) {
        const uint64_t pos = places == 1 ? 0 : (size - len) * i / (places - 1);
        LARGE_INTEGER li;
        li.QuadPart = static_cast<LONGLONG>(pos);
        std::vector<unsigned char> buf(static_cast<size_t>(len));
        DWORD got = 0;
        if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN) || !ReadFile(h, buf.data(), static_cast<DWORD>(len), &got, nullptr) || got != len) ok = false;
        else samples.push_back(std::move(buf));
    }
    CloseHandle(h);
    if (trialSavings) *trialSavings = ok ? TrialSavings(samples) : -1;
    return ok && SamplesLookIncompressible(samples);
}

}  // namespace lycan
