#pragma once
// A message of the Core that the user may read (an error, a warning, a skipped item): an id of the string catalog plus its arguments,
// or - for text that has not been converted yet - plain text. The Core never decides the language: the UI calls Text() (the language of
// the user), logs and the CLI call English() (always English, from the table compiled into the Core, see core_messages.inc).
// An id looks like "core.cannot_list_folder"; its English text is "{0} : cannot list the folder" in app/Assets/Lang/en-US.tsv.
#include <string>
#include <utility>
#include <vector>

#include "lycan/i18n.h"

namespace lycan {

namespace i18n {
// The English text of an id of the Core (ids that start with "core."), independent of the loaded language and of the files on disk.
// An unknown id gives the id itself.
std::wstring English(const std::wstring& id, const std::vector<std::wstring>& args);
}  // namespace i18n

struct Message;
namespace i18n_detail {
std::wstring ToW(const Message& m);  // a message as an argument of another message (one level): see Message::Text
}

struct Message {
    std::wstring id;                 // empty = plain text in raw
    std::vector<std::wstring> args;
    std::wstring raw;                // text that was not converted to an id yet (shown as it is, in every language)

    Message() = default;
    Message(std::wstring text) : raw(std::move(text)) {}       // NOLINT: implicit on purpose, old code keeps working
    Message(const wchar_t* text) : raw(text ? text : L"") {}   // NOLINT
    Message(std::wstring i, std::vector<std::wstring> a) : id(std::move(i)), args(std::move(a)) {}

    template <class... Args>
    static Message Make(const wchar_t* id, Args&&... args) {
        return Message(id, {i18n_detail::ToW(args)...});
    }

    bool empty() const { return id.empty() && raw.empty(); }
    // In the language of the user (the UI).
    std::wstring Text() const { return id.empty() ? raw : i18n_detail::FormatImpl(id.c_str(), Resolve(false)); }
    // Always English (logs, the CLI, tests).
    std::wstring English() const { return id.empty() ? raw : i18n::English(id, Resolve(true)); }

private:
    // An argument that is a message itself (for example the reason of an error) is stored as "<SOH> id <STX> arg <STX> arg ..." (or "<SOH><STX> raw text"
    // when it has no id); here it becomes text. Paths cannot contain control characters, so there is no clash.
    std::vector<std::wstring> Resolve(bool english) const {
        std::vector<std::wstring> out = args;
        for (auto& a : out) {
            if (a.empty() || a[0] != L'\x01') continue;
            std::vector<std::wstring> parts;
            size_t from = 1;
            for (;;) {
                const size_t at = a.find(L'\x02', from);
                parts.push_back(a.substr(from, at == std::wstring::npos ? std::wstring::npos : at - from));
                if (at == std::wstring::npos) break;
                from = at + 1;
            }
            Message m;
            if (parts[0].empty()) m.raw = parts.size() > 1 ? parts[1] : L"";
            else { m.id = parts[0]; m.args.assign(parts.begin() + 1, parts.end()); }
            a = english ? m.English() : m.Text();
        }
        return out;
    }
};

namespace i18n_detail {
inline std::wstring ToW(const Message& m) {
    std::wstring out = L"\x01";
    if (m.id.empty()) return out + L"\x02" + m.raw;
    out += m.id;
    for (auto& a : m.args) out += L"\x02" + a;
    return out;
}
}  // namespace i18n_detail

}  // namespace lycan
