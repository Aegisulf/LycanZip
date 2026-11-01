#pragma once
// The passwords of this run and the saved ones (lycan/passwords.h, passwords.dat next to the settings), shared by every window of the
// process (DESIGN.md "密碼管理員"). Called on the UI thread; Candidates() hands out a copy for the trying in the background.
// Nothing here is ever written to the diagnostic log.
// With a master password the saved list is Locked until it is unlocked once; it then stays open until the process ends (or Lock()).
// The slow steps (deriving the key) are done by the caller off the UI thread (lycan::UnlockPasswords, lycan::MakeMasterKey) and
// handed in here.
#include <filesystem>
#include <string>
#include <vector>

#include "lycan/passwords.h"

namespace lycan_app {

class PasswordVault {
public:
    static PasswordVault& Get();
    void SetFile(std::filesystem::path file);   // passwords.dat (the first window sets it)
    lycan::PasswordFileState State();           // of the saved list (read the first time it is asked)
    std::vector<lycan::SavedPassword> Saved();  // a copy, in the order of trying (empty while locked)
    bool SetSaved(std::vector<lycan::SavedPassword> items);   // the settings page changed the list (saved at once)
    bool ClearUnreadable();                     // deletes a file that cannot be read (another account / PC)
    // The passwords to try, the most likely first: this run's, then the saved ones (no duplicates), at most `limit`
    std::vector<std::wstring> Candidates(bool run, bool saved, size_t limit);
    // A password worked: kept for the run (keepForRun), its time moved forward in the saved list, added to it when `save`
    void Worked(const std::wstring& password, bool keepForRun, bool save);

    // The master password
    bool HasMaster();                           // set (locked or unlocked)
    bool Locked();                              // set and not unlocked yet
    std::string LockedBytes();                  // for lycan::UnlockPasswords
    lycan::MasterKey Key();                     // a copy (for lycan::CheckMasterKey)
    void Unlocked(std::vector<lycan::SavedPassword> items, lycan::MasterKey key);   // after UnlockPasswords succeeded
    bool SetMaster(lycan::MasterKey key);       // set or change it (the list must be open): saved again with the new key
    bool RemoveMaster();                        // turn it off (the list must be open): saved with DPAPI only
    void Lock();                                // forget the key and the list until the next unlock
    bool Reset();                               // the master password is forgotten: deletes passwords.dat (every saved password)

private:
    void Load();
    bool Save();
    std::filesystem::path file_;
    bool loaded_ = false;
    lycan::PasswordFileState state_ = lycan::PasswordFileState::Missing;
    std::vector<lycan::SavedPassword> saved_;
    std::string locked_;      // the file's bytes while Locked
    lycan::MasterKey key_;    // while unlocked
    std::vector<std::wstring> run_;   // the most recent first
};

}  // namespace lycan_app
