#pragma once
// Reads the prompts and parameters that AI generated pictures carry inside:
//  - Stable Diffusion WebUI / Forge (tEXt "parameters" of PNG, EXIF UserComment of JPEG / WebP)
//  - ComfyUI (the "prompt" workflow JSON of PNG), NovelAI ("Comment" JSON), InvokeAI ("invokeai_metadata" JSON)
// tEXt / zTXt / iTXt of PNG are all supported (compressed ones are unpacked with a small built-in inflate); a parse failure always counts as no data and never throws.
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace lycan_app {

struct AiMeta {
    bool found = false;
    std::wstring source;                                       // for example "Stable Diffusion WebUI (A1111 / Forge)"
    std::wstring positive, negative;                           // positive / negative prompt
    std::vector<std::pair<std::wstring, std::wstring>> params; // Steps, Sampler, CFG scale, Seed, Size, Model ...
    std::vector<std::pair<std::wstring, std::wstring>> other;  // other text metadata (Software, Comment ...)
    std::wstring raw;                                          // the original text (cut off) when it is recognised as AI data but cannot be split up
};

// exifComment: the EXIF UserComment of JPEG / WebP (already text; an empty string if none)
AiMeta ParseAiMeta(const std::vector<uint8_t>& imageBytes, const std::wstring& exifComment);

// Turns the raw bytes of an EXIF UserComment (with the 8-byte character set prefix) into text
std::wstring DecodeUserComment(const uint8_t* data, size_t size);

}  // namespace lycan_app
