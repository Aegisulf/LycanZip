#pragma once
// The automatic update of the installed form of LycanZip (installer and update program: Velopack). Only built when third_party/velopack is there
// (scripts/fetch-velopack.ps1). The feed is read through our own source (updatefeed.h): its signature and the hash of every package are checked before
// Velopack sees them, so a changed feed or package is never installed. the design notes "Installer and automatic update".
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "lycan/updatefeed.h"

namespace lycan {

struct UpdateOffer {
    std::string version;       // "0.9.2"
    uint64_t bytes = 0;        // what is downloaded (the delta packages, or the full package)
    bool isDelta = false;
};

// Tests: a made-up installation (Velopack asks for the places of the installation; the real ones exist only in an installed copy)
struct FakeInstall {
    std::filesystem::path root, updateExe, packages, manifest, currentDir;
};

class InstalledUpdater {
public:
    // feedBase: lycan::UpdateFeedBase(); key: lycan::GetUpdatePublicKey(); fake: only tests
    InstalledUpdater(const std::wstring& feedBase, const UpdatePublicKey& key, const FakeInstall* fake = nullptr);
    ~InstalledUpdater();
    InstalledUpdater(const InstalledUpdater&) = delete;
    InstalledUpdater& operator=(const InstalledUpdater&) = delete;

    // Asks the signed feed. No offer and an empty error: up to date. No offer and an error: the feed could not be read or did not pass the check.
    std::optional<UpdateOffer> Check(std::wstring* error);
    // Downloads what Check offered (progress 0..100; false from `cancel` stops). The packages are in the packages folder of the installation afterwards.
    bool Download(const std::function<void(int)>& progress, std::wstring* error);
    // Starts the update program that waits for this process to end, replaces the program and starts it again. The caller ends the process at once.
    bool ApplyAfterExit(std::wstring* error);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lycan
