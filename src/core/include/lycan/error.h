#pragma once
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

#include "lycan/message.h"

namespace lycan {

std::string ToUtf8(std::wstring_view w);  // util.cpp
std::wstring WideFromUtf8(const std::string& utf8);  // util.cpp
// logs and the CLI are in English (a template, so that a plain wstring does not become a Message here)
template <class T, std::enable_if_t<std::is_same_v<T, Message>, int> = 0>
std::string ToUtf8(const T& m) { return ToUtf8(m.English()); }

// The one exception type of the Core layer; hr is an HRESULT (E_ABORT means cancelled by the user).
class Error : public std::runtime_error {
public:
    // msg: UTF-8 text that is not an id of the catalog (shown as it is in every language; what() is the same text)
    Error(long hr, const std::string& msg) : std::runtime_error(msg), hr_(hr) {}
    // A message with an id: what() is its English text (logs, CLI, tests); the UI asks message().Text() for the language of the user.
    Error(long hr, const Message& m) : std::runtime_error(ToUtf8(m.English())), hr_(hr), msg_(m) {}
    Message message() const { return msg_.id.empty() ? Message(WideFromUtf8(what())) : msg_; }
    long hr() const noexcept { return hr_; }
    bool cancelled() const noexcept { return hr_ == static_cast<long>(0x80004004L); }  // E_ABORT
    // a password was given but opening / reading failed, and the reason is that the password is wrong (HRESULT_FROM_WIN32(ERROR_INVALID_PASSWORD))
    bool wrongPassword() const noexcept { return hr_ == kWrongPassword; }
    static constexpr long kWrongPassword = static_cast<long>(0x80070056L);
    // The target disk is full (ERROR_DISK_FULL / ERROR_HANDLE_DISK_FULL as HRESULT)
    // The scratch folder (tar intermediate files) has too little room; the message carries the numbers (see tempdir.h)
    bool tempSpace() const noexcept { return hr_ == kTempSpace; }
    static constexpr long kTempSpace = static_cast<long>(0xA0000070L);
    bool diskFull() const noexcept { return hr_ == static_cast<long>(0x80070070L) || hr_ == static_cast<long>(0x80070027L) || tempSpace(); }
private:
    long hr_;
    Message msg_;
};

}  // namespace lycan
