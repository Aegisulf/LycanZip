#include "pch.h"
#include "ProtectedMove.h"

#include <shobjidl_core.h>

#include "lycan/diag.h"
#include "lycan/taskqueue.h"
#include "lycan/tempdir.h"

namespace lycan_app
{
    bool ShellMoveInto(HWND owner, std::vector<std::filesystem::path> const& items, std::filesystem::path const& into, bool replace, lycan::Message& error)
    {
        const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        HRESULT hr = S_OK;
        BOOL aborted = FALSE;
        {
            winrt::com_ptr<IFileOperation> op;
            winrt::com_ptr<IShellItem> folder;
            hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(op.put()));
            if (SUCCEEDED(hr)) hr = op->SetOwnerWindow(owner);
            // no "create this folder?" question; the rights and a name that exists are asked as in File Explorer
            if (SUCCEEDED(hr)) hr = op->SetOperationFlags(FOF_NOCONFIRMMKDIR | FOFX_SHOWELEVATIONPROMPT | (replace ? FOF_NOCONFIRMATION : 0));
            if (SUCCEEDED(hr)) hr = SHCreateItemFromParsingName(into.lexically_normal().c_str(), nullptr, IID_PPV_ARGS(folder.put()));   // ("x\." is refused)
            for (auto const& p : items)
            {
                if (FAILED(hr)) break;
                winrt::com_ptr<IShellItem> item;
                hr = SHCreateItemFromParsingName(p.c_str(), nullptr, IID_PPV_ARGS(item.put()));
                if (SUCCEEDED(hr)) hr = op->MoveItem(item.get(), folder.get(), nullptr, nullptr);
            }
            if (SUCCEEDED(hr)) hr = op->PerformOperations();
            if (op) op->GetAnyOperationsAborted(&aborted);
        }
        if (SUCCEEDED(init)) CoUninitialize();
        if (SUCCEEDED(hr) && !aborted) return true;
        lycan::diag::Warn(L"moving into {} (needs administrator rights): hr 0x{:08X}, aborted {}", lycan::diag::Name(into), static_cast<unsigned>(hr), aborted != FALSE);
        if (aborted || hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) error = lycan::Message::Make(L"core.protected_move_cancelled", into.wstring());
        else
        {
            wchar_t code[16];
            swprintf_s(code, L"0x%08X", static_cast<unsigned>(hr));
            error = lycan::Message::Make(L"core.protected_move_failed", into.wstring(), std::wstring(code));
        }
        return false;
    }

    bool WriteInto(HWND owner, std::filesystem::path const& folder, std::function<bool(std::filesystem::path const& where)> const& write)
    {
        if (lycan::CanWriteInto(folder)) return write(folder);
        wchar_t name[64];
        static std::atomic<unsigned> seq{0};
        swprintf_s(name, L"lycan-staged-%lu-w%u", GetCurrentProcessId(), ++seq);
        const std::filesystem::path staging = lycan::ScratchDirectory({}) / name;
        std::error_code ec;
        std::filesystem::create_directories(staging, ec);
        bool ok = false;
        try { ok = write(staging); } catch (...) {}
        if (ok)
        {
            std::vector<std::filesystem::path> items;
            for (auto const& e : std::filesystem::directory_iterator(staging, ec)) items.push_back(e.path());
            lycan::Message error;
            ok = items.empty() || ShellMoveInto(owner, items, folder, true, error);
        }
        std::filesystem::remove_all(staging, ec);
        return ok;
    }
}
