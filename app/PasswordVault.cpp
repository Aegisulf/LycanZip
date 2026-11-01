#include "pch.h"
#include "PasswordVault.h"

#include <algorithm>

namespace lycan_app {

PasswordVault& PasswordVault::Get() {
    static PasswordVault vault;
    return vault;
}

void PasswordVault::SetFile(std::filesystem::path file) {
    if (!file_.empty() || file.empty()) return;
    file_ = std::move(file);
}

void PasswordVault::Load() {
    if (loaded_) return;
    loaded_ = true;
    if (file_.empty()) return;
    auto f = lycan::LoadPasswords(file_);
    state_ = f.state;
    saved_ = std::move(f.items);
    locked_ = std::move(f.locked);
    lycan::SortForTrying(saved_);
}

bool PasswordVault::Save() {
    if (!lycan::SavePasswords(file_, saved_, key_.empty() ? nullptr : &key_)) return false;
    state_ = lycan::PasswordFileState::Ok;
    return true;
}

lycan::PasswordFileState PasswordVault::State() {
    Load();
    return state_;
}

std::vector<lycan::SavedPassword> PasswordVault::Saved() {
    Load();
    return saved_;
}

bool PasswordVault::SetSaved(std::vector<lycan::SavedPassword> items) {
    Load();
    // never overwrite a file we could not read, nor a locked one
    if (state_ == lycan::PasswordFileState::Unreadable || state_ == lycan::PasswordFileState::Locked || file_.empty()) return false;
    saved_ = std::move(items);
    lycan::SortForTrying(saved_);
    return Save();
}

bool PasswordVault::ClearUnreadable() {
    Load();
    std::error_code ec;
    std::filesystem::remove(file_, ec);
    if (ec) return false;
    state_ = lycan::PasswordFileState::Missing;
    saved_.clear();
    return true;
}

std::vector<std::wstring> PasswordVault::Candidates(bool run, bool saved, size_t limit) {
    std::vector<std::wstring> out;
    auto add = [&](std::wstring const& p) {
        if (out.size() < limit && !p.empty() && std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
    };
    if (run) for (auto const& p : run_) add(p);
    if (saved) {
        Load();
        for (auto const& s : saved_) add(s.password);
    }
    return out;
}

void PasswordVault::Worked(const std::wstring& password, bool keepForRun, bool save) {
    if (password.empty()) return;
    if (keepForRun) {
        run_.erase(std::remove(run_.begin(), run_.end(), password), run_.end());
        run_.insert(run_.begin(), password);
        if (run_.size() > 50) run_.resize(50);
    }
    Load();
    if (state_ == lycan::PasswordFileState::Unreadable || state_ == lycan::PasswordFileState::Locked || file_.empty()) return;
    const bool known = std::any_of(saved_.begin(), saved_.end(), [&](auto const& s) { return s.password == password; });
    if (!known && !save) return;
    lycan::TouchPassword(saved_, password, lycan::FileTimeNow(), save);
    lycan::SortForTrying(saved_);
    Save();
}

bool PasswordVault::HasMaster() {
    Load();
    return state_ == lycan::PasswordFileState::Locked || !key_.empty();
}

bool PasswordVault::Locked() {
    Load();
    return state_ == lycan::PasswordFileState::Locked;
}

std::string PasswordVault::LockedBytes() {
    Load();
    return locked_;
}

lycan::MasterKey PasswordVault::Key() { return key_; }

void PasswordVault::Unlocked(std::vector<lycan::SavedPassword> items, lycan::MasterKey key) {
    if (!Locked()) return;
    saved_ = std::move(items);
    lycan::SortForTrying(saved_);
    key_ = std::move(key);
    locked_.clear();
    state_ = lycan::PasswordFileState::Ok;
}

bool PasswordVault::SetMaster(lycan::MasterKey key) {
    Load();
    if (key.empty() || file_.empty() || state_ == lycan::PasswordFileState::Locked || state_ == lycan::PasswordFileState::Unreadable) return false;
    auto old = std::move(key_);
    key_ = std::move(key);
    if (Save()) { old.Clear(); return true; }
    key_.Clear();
    key_ = std::move(old);
    return false;
}

bool PasswordVault::RemoveMaster() {
    Load();
    if (key_.empty() || state_ != lycan::PasswordFileState::Ok) return false;
    auto old = std::move(key_);
    key_ = {};
    if (Save()) { old.Clear(); return true; }
    key_ = std::move(old);
    return false;
}

void PasswordVault::Lock() {
    if (key_.empty()) return;
    key_.Clear();
    for (auto& s : saved_) SecureZeroMemory(s.password.data(), s.password.size() * sizeof(wchar_t));
    saved_.clear();
    loaded_ = false;
    Load();   // reads the file again: Locked
}

bool PasswordVault::Reset() {
    Load();
    std::error_code ec;
    std::filesystem::remove(file_, ec);
    if (ec && std::filesystem::exists(file_)) return false;
    key_.Clear();
    saved_.clear();
    locked_.clear();
    state_ = lycan::PasswordFileState::Missing;
    return true;
}

}  // namespace lycan_app
