#pragma once
// The welcome window (first start): three steps, a small window of its own. Look and sound, viewing and previews, and the integration
// with Windows (context menus, file associations). The window edits a copy of the settings; the choices of the first two steps show at
// once in the main window (host.preview), nothing is saved until "Finish", and nothing is registered with Windows before it either.
#include <functional>
#include <string>
#include <vector>

#include <winrt/Windows.UI.h>

#include "lycan/settings.h"

namespace lycan_app {

struct WelcomeAccent {
    std::wstring name;            // translated
    winrt::Windows::UI::Color color{};
    bool followSystem = false;    // the last one: the accent colour of Windows
};

struct WelcomeHost {
    lycan::Settings settings;                      // the settings now (the wizard starts from them)
    std::vector<WelcomeAccent> accents;            // in the order of lycan::AccentTheme
    bool win11Possible = false;                    // the Windows 11 menu can be switched on (Windows 11 and a menu package next to the program)
    std::wstring win11Note;                        // why not (shown under the switch)
    // Not empty: the menus and the file types are not this copy's to switch (the Store version has them built in, or another copy is the Store
    // version); the three switches show integrationOn, cannot be changed, and this text is shown under each
    std::wstring integrationNote;
    bool integrationOn = false;
    // The Store version: the context-menu switch can still be changed (it is the shell_menu setting, not a registration); its caption is this text
    std::wstring menuSwitchNote;
    std::function<void(lycan::Settings const&)> preview;   // live look: theme, accent, animations, sound; nothing is saved
    std::function<void()> playSample;              // a short sound of the chosen set (called after `preview`)
    // "Finish": the settings to keep and what to register with Windows. UI thread.
    std::function<void(lycan::Settings const&, bool classicMenu, bool win11Menu, bool fileAssoc)> finish;
    // The window was closed or "Skip" was pressed: nothing changes, the window is not shown again. UI thread.
    std::function<void()> skipped;
};

void ShowWelcome(WelcomeHost host);
bool WelcomeIsOpen();
void CloseWelcome();

}  // namespace lycan_app
