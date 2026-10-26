#include "pch.h"
#include "SingleInstance.h"

#include <sddl.h>

#include <cwchar>
#include <cwctype>

#include "lycan/datalocation.h"

#include <vector>

namespace {
constexpr DWORD kMaxMessageBytes = 4 * 1024 * 1024;
}

SingleInstance::~SingleInstance() {
    Stop();
    if (mutex_) CloseHandle(mutex_);
}

std::wstring SingleInstance::PipeName() {
    wchar_t user[256] = L"";
    DWORD n = static_cast<DWORD>(std::size(user));
    GetUserNameW(user, &n);
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    return L"\\\\.\\pipe\\LycanZip." + std::wstring(user) + L"." + std::to_wstring(session) + L"." + CopyTag();
}

std::wstring SingleInstance::CopyTag() {
    static const std::wstring tag = [] {
        std::wstring dir = lycan::GetDataLocation().dataDir.lexically_normal().wstring();
        while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
        unsigned long long h = 14695981039346656037ull;   // FNV-1a of the folder, case-insensitive
        for (wchar_t c : dir) { h ^= static_cast<unsigned long long>(towlower(c)); h *= 1099511628211ull; }
        wchar_t buf[17];
        swprintf_s(buf, L"%016llx", h);
        return std::wstring(buf);
    }();
    return tag;
}

bool SingleInstance::BecomePrimary() {
    // Local\ namespace = separate for each logon session; one main instance per copy (CopyTag)
    HANDLE m = CreateMutexW(nullptr, FALSE, (L"Local\\LycanZip.Primary." + CopyTag()).c_str());
    if (!m) return true;  // cannot create it: better to act as the main instance than to block the user
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(m);
        return false;
    }
    mutex_ = m;
    return true;
}

bool SingleInstance::Forward(const std::wstring& message, DWORD timeoutMs) {
    const std::wstring name = PipeName();
    ULONGLONG end = GetTickCount64() + timeoutMs;
    do {
        if (WaitNamedPipeW(name.c_str(), 200)) {
            HANDLE h = CreateFileW(name.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, SECURITY_SQOS_PRESENT | SECURITY_ANONYMOUS, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                BOOL ok = WriteFile(h, message.data(), static_cast<DWORD>(message.size() * sizeof(wchar_t)), &written, nullptr);
                FlushFileBuffers(h);
                CloseHandle(h);
                if (ok) return true;
            }
        } else {
            Sleep(50);
        }
    } while (GetTickCount64() < end);
    return false;
}

void SingleInstance::StartServer(std::function<void(std::wstring)> onMessage) {
    onMessage_ = std::move(onMessage);
    stopping_ = false;
    server_ = std::thread([this] { ServerLoop(); });
}

void SingleInstance::Stop() {
    if (!server_.joinable()) return;
    stopping_ = true;
    // wake up the server thread that is blocked in ConnectNamedPipe
    HANDLE h = CreateFileW(PipeName().c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    server_.join();
}

void SingleInstance::ServerLoop() {
    // only the owner and SYSTEM may access it (the default DACL would let Everyone read)
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, FALSE};
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;OW)(A;;GA;;;SY)", SDDL_REVISION_1, &sd, nullptr))
        sa.lpSecurityDescriptor = sd;

    while (!stopping_) {
        HANDLE pipe = CreateNamedPipeW(PipeName().c_str(), PIPE_ACCESS_INBOUND,
                                       PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 4,
                                       0, 64 * 1024, 0, &sa);
        if (pipe == INVALID_HANDLE_VALUE) { Sleep(200); continue; }
        BOOL connected = ConnectNamedPipe(pipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (connected && !stopping_) {
            std::vector<char> data;
            char buf[8192];
            for (;;) {
                DWORD n = 0;
                BOOL ok = ReadFile(pipe, buf, sizeof(buf), &n, nullptr);
                data.insert(data.end(), buf, buf + n);
                if (data.size() > kMaxMessageBytes) { data.clear(); break; }
                if (ok) break;                              // the message has been read completely
                if (GetLastError() != ERROR_MORE_DATA) break;
            }
            if (!data.empty() && data.size() % sizeof(wchar_t) == 0 && onMessage_)
                onMessage_(std::wstring(reinterpret_cast<wchar_t*>(data.data()), data.size() / sizeof(wchar_t)));
        }
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
    if (sd) LocalFree(sd);
}
