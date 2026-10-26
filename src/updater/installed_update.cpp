#include "installed_update.h"

#include <windows.h>

#include <algorithm>

#include <Velopack.hpp>

#include "lycan/error.h"

namespace fs = std::filesystem;

namespace lycan {
namespace {

std::string Lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::wstring WideAscii(const std::string& s) { return std::wstring(s.begin(), s.end()); }

// What Velopack asks the program for: the feed and the packages. Everything that comes from the web is checked here.
class SignedFeedSource final : public Velopack::IUpdateSource {
public:
    SignedFeedSource(std::wstring base, UpdatePublicKey key) : base_(std::move(base)), key_(key) {}

    // Velopack calls these from its own threads and through a C interface: nothing may be thrown out of them.
    const std::string GetReleaseFeed(const std::string releasesName) override {
        try {
            auto feed = FetchSignedFeed(base_, releasesName, key_);
            return feed ? *feed : std::string();   // empty = no feed
        } catch (...) {
            return {};
        }
    }

    bool DownloadReleaseEntry(const Velopack::VelopackAsset& asset, const std::string localFilePath, Velopack::vpkc_progress_send_t progress) override {
        try {
            // the signed feed names the package and its hash; a package without a hash is not accepted
            if (!IsPlainPackageName(asset.FileName) || asset.SHA256.size() != 64 || asset.Size == 0 || asset.Size > kMaxPackage) return false;
            const fs::path to{std::u8string(localFilePath.begin(), localFilePath.end())};
            int last = -1;
            const bool got = HttpDownloadFile(base_ + WideAscii(asset.FileName), to, asset.Size, [&](uint64_t done, uint64_t) {
                const int percent = static_cast<int>(std::min<uint64_t>(100, done * 100 / asset.Size));
                if (progress && percent != last) { last = percent; progress(static_cast<int16_t>(percent)); }
                return true;
            });
            std::error_code ec;
            if (!got) return false;
            if (fs::file_size(to, ec) != asset.Size || FileSha256Hex(to) != Lower(asset.SHA256)) {   // not what the signed feed lists: deleted
                fs::remove(to, ec);
                return false;
            }
            return true;
        } catch (...) {
            return false;
        }
    }

private:
    static constexpr uint64_t kMaxPackage = 600ull * 1024 * 1024;
    std::wstring base_;
    UpdatePublicKey key_;
};

std::string Utf8(const fs::path& p) {
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

}  // namespace

struct InstalledUpdater::Impl {
    std::unique_ptr<Velopack::UpdateManager> manager;
    std::optional<Velopack::UpdateInfo> info;
};

InstalledUpdater::InstalledUpdater(const std::wstring& feedBase, const UpdatePublicKey& key, const FakeInstall* fake) : impl_(std::make_unique<Impl>()) {
    auto source = std::make_unique<SignedFeedSource>(feedBase, key);
    if (fake) {
        Velopack::VelopackLocatorConfig loc{Utf8(fake->root), Utf8(fake->updateExe), Utf8(fake->packages), Utf8(fake->manifest), Utf8(fake->currentDir), false};
        impl_->manager = std::make_unique<Velopack::UpdateManager>(std::move(source), nullptr, &loc);
    } else {
        impl_->manager = std::make_unique<Velopack::UpdateManager>(std::move(source));
    }
}

InstalledUpdater::~InstalledUpdater() = default;

std::optional<UpdateOffer> InstalledUpdater::Check(std::wstring* error) {
    if (error) error->clear();
    impl_->info.reset();
    try {
        auto found = impl_->manager->CheckForUpdates();
        if (!found) return std::nullopt;   // up to date (or: no feed; the source returns nothing in both cases of failure, Velopack then reports an exception)
        UpdateOffer offer;
        offer.version = found->TargetFullRelease.Version;
        offer.isDelta = !found->DeltasToTarget.empty();
        if (offer.isDelta) { for (const auto& d : found->DeltasToTarget) offer.bytes += d.Size; }
        else offer.bytes = found->TargetFullRelease.Size;
        impl_->info = std::move(*found);
        return offer;
    } catch (const std::exception& e) {
        if (error) *error = WideFromUtf8(e.what());
        return std::nullopt;
    }
}

bool InstalledUpdater::Download(const std::function<void(int)>& progress, std::wstring* error) {
    if (!impl_->info) { if (error) *error = L"nothing to download"; return false; }
    try {
        impl_->manager->DownloadUpdates(*impl_->info, [](void* user, size_t percent) {
            const auto* fn = static_cast<const std::function<void(int)>*>(user);
            if (fn && *fn) (*fn)(static_cast<int>(percent));
        }, const_cast<std::function<void(int)>*>(&progress));
        return true;
    } catch (const std::exception& e) {
        if (error) *error = WideFromUtf8(e.what());
        return false;
    }
}

bool InstalledUpdater::ApplyAfterExit(std::wstring* error) {
    if (!impl_->info) { if (error) *error = L"nothing to apply"; return false; }
    try {
        impl_->manager->WaitExitThenApplyUpdates(*impl_->info, true, true);
        return true;
    } catch (const std::exception& e) {
        if (error) *error = WideFromUtf8(e.what());
        return false;
    }
}

}  // namespace lycan
