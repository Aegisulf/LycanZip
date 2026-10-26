#pragma once
// Content probe for "smart store" (CompressOptions::storeCompressed): decides from a few samples of a file whether packing it
// would gain anything. The extension list (incompressible.h) is the first, free test; this is the second one for files the
// list does not know (no or odd extension, game resources, encrypted containers, new media formats).
//
//   1. Samples are taken at several places of the file (the start of a file is often an uncompressed header).
//   2. A sample whose byte entropy is clearly low (< kProbeLowEntropy) means the file is compressible: pack it.
//   3. Otherwise the samples are compressed with Windows' XPRESS (Huffman) as a trial; less than kProbeMinGain saved = store.
//      (Entropy alone is not enough: random data that repeats itself has a high entropy and still packs very well. The trial is
//      cheap: about 100 KB per file.)
// Any doubt (read error, no compressor) means "pack it": a wrong "pack" costs time, a wrong "store" costs archive size.
#include <cstdint>
#include <filesystem>
#include <vector>

namespace lycan {

inline constexpr double kProbeLowEntropy = 7.0;    // bits per byte; below it a sample is plainly compressible
inline constexpr double kProbeMinGain = 0.05;      // the trial must save at least this share to call the file compressible

// Shannon entropy of the byte values, 0..8 bits per byte (0 for no data).
double ByteEntropy(const unsigned char* data, size_t size);

// Share of the samples that the trial compressor saves (0 = nothing, 0.5 = half); -1 when it could not run.
double TrialSavings(const std::vector<std::vector<unsigned char>>& samples);

// The verdict for samples that are read already: true = store the file as it is.
bool SamplesLookIncompressible(const std::vector<std::vector<unsigned char>>& samples);

// Reads the samples of a file of `size` bytes (at least kStoreMinBytes) and judges them. False = pack it (also on any error).
bool FileLooksIncompressible(const std::filesystem::path& file, uint64_t size, double* trialSavings = nullptr);

}  // namespace lycan
