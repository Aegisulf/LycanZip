#pragma once
// Dynamic loading of 7z.dll and the list of formats (internal header, not public).
#include <windows.h>

#include <filesystem>
#include <string>
#include <vector>

#include "7zip/Archive/IArchive.h"
#include "7zip/IPassword.h"
#include "Common/MyCom.h"

namespace lycan {

struct FormatInfo {
    std::wstring name;
    GUID clsid{};
    std::vector<std::wstring> extensions;  // lower case, without '.'
    bool canUpdate = false;                // can create / update (IOutArchive)
};

class SevenZipLib {
public:
    // Loads 7z.dll on the first call; throws lycan::Error when it cannot be found.
    static SevenZipLib& Get();

    const std::vector<FormatInfo>& Formats() const { return formats_; }
    CMyComPtr<IInArchive> CreateInArchive(const GUID& clsid) const;
    CMyComPtr<IOutArchive> CreateOutArchive(const GUID& clsid) const;
    const FormatInfo* FindFormat(const std::wstring& lowerName) const;  // find a format by name (case-insensitive)
    const std::filesystem::path& DllPath() const { return dllPath_; }

private:
    SevenZipLib();
    SevenZipLib(const SevenZipLib&) = delete;

    using CreateObjectFn = UINT32(WINAPI*)(const GUID*, const GUID*, void**);
    using GetNumberOfFormatsFn = UINT32(WINAPI*)(UINT32*);
    using GetHandlerProperty2Fn = UINT32(WINAPI*)(UINT32, PROPID, PROPVARIANT*);

    HMODULE module_ = nullptr;
    CreateObjectFn createObject_ = nullptr;
    std::filesystem::path dllPath_;
    std::vector<FormatInfo> formats_;
};

}  // namespace lycan
