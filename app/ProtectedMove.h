#pragma once
// Putting finished work into a folder that needs administrator rights (Program Files): the shell's file operation (the one of File
// Explorer) moves it there and asks for the rights itself. Used by TaskQueue::SetProtectedMove (Core writes into a scratch folder first).
#include <windows.h>
#include <filesystem>
#include <functional>
#include <vector>

#include "lycan/message.h"

namespace lycan_app
{
    // Runs on the calling thread (a COM apartment of its own). false + `error` when nothing or not everything was moved.
    // replace: a file of the same name is replaced without asking (the archive changed in place); otherwise Windows asks as in File Explorer.
    bool ShellMoveInto(HWND owner, std::vector<std::filesystem::path> const& items, std::filesystem::path const& into, bool replace, lycan::Message& error);

    // Files that the program writes itself into `folder` (checksum files, a saved picture): `write` gets the folder to write into, which
    // is `folder` itself, or, when that needs administrator rights, a scratch folder whose files the shell then moves in (asking for the
    // rights; what is there under the same name is replaced). Runs on the calling thread (not the UI thread: the move waits for the user).
    // false when `write` failed or the move did not happen.
    bool WriteInto(HWND owner, std::filesystem::path const& folder, std::function<bool(std::filesystem::path const& where)> const& write);
}
