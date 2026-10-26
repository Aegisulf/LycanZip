#pragma once
// Settings broadcast shared by the whole process (UI thread only). With several windows each MainWindow holds its own working copy of the settings;
// after any window saves, it calls Publish and the other windows get the newest settings at once and apply them (same UI thread, a direct synchronous call),
// so "window B still has old settings and overwrites what window A just changed when it saves" cannot happen, and there is no need to wait until a window is activated to synchronize.
// The file itself is still written once by the publisher; an outside change such as editing settings.ini by hand is picked up by MainWindow::ReloadSettingsIfChanged when the window is activated.
#include <algorithm>
#include <functional>
#include <utility>
#include <vector>

#include "lycan/settings.h"

namespace lycan_app {

class SettingsHub {
public:
    using Handler = std::function<void(const lycan::Settings&)>;

    static SettingsHub& Get() {
        static SettingsHub hub;
        return hub;
    }

    void Subscribe(const void* key, Handler h) {
        Unsubscribe(key);
        subs_.emplace_back(key, std::move(h));
    }
    void Unsubscribe(const void* key) {
        subs_.erase(std::remove_if(subs_.begin(), subs_.end(), [&](auto const& s) { return s.first == key; }), subs_.end());
    }

    // Notifies all subscribers except from. A handler may Unsubscribe itself (copy the list first, then call).
    void Publish(const void* from, const lycan::Settings& s) {
        auto copy = subs_;
        for (auto const& [key, h] : copy)
            if (key != from && h) h(s);
    }

private:
    std::vector<std::pair<const void*, Handler>> subs_;
};

}  // namespace lycan_app
