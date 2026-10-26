#pragma once
// A high contrast look of our own (not following the system): the dark theme with its brushes replaced by pure black, white and
// two bright colours, like the Windows "Night sky"-less contrast black theme. WinUI loads its real high contrast resources only when
// Windows is in a contrast theme, so this overrides the brushes of the theme dictionaries of the application instead (the same way the accent
// colour is done). The original colours are kept to switch back.
#include <winrt/Microsoft.UI.Xaml.h>

namespace lycan_app {

// Switches the look on or off (the window must use ElementTheme::Dark while it is on). Safe to call again: it re-applies on top of
// colours that were changed in between (the accent), and restores the originals when switched off.
void SetContrastTheme(bool on);
bool ContrastThemeOn();

}  // namespace lycan_app
