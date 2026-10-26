#pragma once
// The installed form of LycanZip (made by the installer, which is built with Velopack) and its automatic update.
// the design notes "Installer and automatic update".

namespace lycan_app {

// Is the Velopack library (velopack_libc.dll) next to the exe? Only the installed copy has it: the zip packages and the Store package do not,
// and the program then neither answers the installer's arguments nor updates itself.
bool VelopackPresent();

}  // namespace lycan_app
