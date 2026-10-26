// Parsing and serializing of start requests, and the context menu registration (written only to a temporary test key, the real Classes is not touched)
#include "no_dialogs.h"
#include <windows.h>

#include <cstdio>
#include <fstream>

#include "lycan/launch.h"
#include "lycan/shellreg.h"
#include "lycan/version.h"

using namespace lycan;
namespace fs = std::filesystem;

static int failures = 0;
#define EXPECT(cond)                                                     \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); \
            ++failures;                                                  \
        }                                                                \
    } while (0)

static std::wstring ReadReg(const std::wstring& sub, const wchar_t* name) {
    wchar_t buf[2048];
    DWORD n = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, sub.c_str(), name, RRF_RT_REG_SZ, nullptr, buf, &n) != ERROR_SUCCESS) return L"<missing>";
    return buf;
}

int main() {
    // ---- Command line parsing ----
    {
        auto r = ParseLaunchArgs({});
        EXPECT(r.valid && r.action == LaunchAction::None && r.paths.empty());
    }
    {
        auto r = ParseLaunchArgs({L"C:\\a.zip"});
        EXPECT(r.valid && r.action == LaunchAction::Open && r.paths.size() == 1);
        EXPECT(!r.IsQuickAction());
    }
    {
        auto r = ParseLaunchArgs({L"C:\\a.txt", L"C:\\b.txt"});
        EXPECT(r.valid && r.action == LaunchAction::Add && r.paths.size() == 2);
    }
    {
        auto r = ParseLaunchArgs({L"--compress", L"7z", L"C:\\a", L"C:\\b"});
        EXPECT(r.valid && r.action == LaunchAction::Compress && r.format == ArchiveType::SevenZip && r.paths.size() == 2 && r.IsQuickAction());
        r = ParseLaunchArgs({L"--COMPRESS", L"ZIP", L"x"});  // case-insensitive
        EXPECT(r.valid && r.action == LaunchAction::Compress);
    }
    {   // a built-in preset: the current name, and a name of the first versions (Chinese) gives the current one
        EXPECT(ParseLaunchArgs({L"--compress", L"zip", L"--template", L"Backup", L"x"}).preset == L"Backup");
        EXPECT(ParseLaunchArgs({L"--compress", L"zip", L"--template", L"備份", L"x"}).preset == L"Backup");
        EXPECT(ParseLaunchArgs({L"--compress", L"zip", L"--template", L"My preset", L"x"}).preset == L"My preset");
    }
    EXPECT(!ParseLaunchArgs({L"--compress"}).valid);
    EXPECT(!ParseLaunchArgs({L"--compress", L"rar", L"x"}).valid);
    EXPECT(!ParseLaunchArgs({L"--extract-here"}).valid);  // no file
    EXPECT(!ParseLaunchArgs({L"--bogus", L"x"}).valid);
    {
        auto r = ParseLaunchArgs({L"--extract-here", L"C:\\a.zip"});
        EXPECT(r.valid && r.action == LaunchAction::ExtractHere && r.IsQuickAction());
        r = ParseLaunchArgs({L"--extract-folder", L"C:\\a.zip", L"C:\\b.7z"});
        EXPECT(r.valid && r.action == LaunchAction::ExtractFolder && r.paths.size() == 2);
        r = ParseLaunchArgs({L"--add", L"C:\\x"});
        EXPECT(r.valid && r.action == LaunchAction::Add);
    }

    // New actions: formats, templates, each / encrypted / extract-to / merge / test / previews / open-content
    {
        auto r = ParseLaunchArgs({L"--compress", L"tar.gz", L"--template", L"Backup", L"C:\\a"});
        EXPECT(r.valid && r.action == LaunchAction::Compress && r.format == ArchiveType::TarGz && r.preset == L"Backup" && r.IsQuickAction());
        r = ParseLaunchArgs({L"--compress-each", L"7z", L"C:\\a", L"C:\\b"});
        EXPECT(r.valid && r.action == LaunchAction::CompressEach && r.format == ArchiveType::SevenZip && r.paths.size() == 2 && r.IsQuickAction());
        r = ParseLaunchArgs({L"--add-encrypted", L"C:\\a"});
        EXPECT(r.valid && r.action == LaunchAction::AddEncrypted && r.IsQuickAction() && !r.IsArchiveQuickAction());
        for (auto flag : {L"--extract-to", L"--extract-merge", L"--test"}) {
            r = ParseLaunchArgs({flag, L"C:\\a.zip", L"C:\\b.zip"});
            EXPECT(r.valid && r.paths.size() == 2 && r.IsQuickAction() && r.IsArchiveQuickAction());
        }
        r = ParseLaunchArgs({L"--preview-images", L"C:\\a.cbz"});
        EXPECT(r.valid && r.action == LaunchAction::PreviewImages && !r.IsQuickAction());
        r = ParseLaunchArgs({L"--preview-audio", L"C:\\a.zip"});
        EXPECT(r.valid && r.action == LaunchAction::PreviewAudio);
        r = ParseLaunchArgs({L"--open-content", L"C:\\setup.exe"});
        EXPECT(r.valid && r.action == LaunchAction::OpenContent && r.paths.size() == 1);
        EXPECT(!ParseLaunchArgs({L"--preview-images", L"C:\\a.zip", L"C:\\b.zip"}).valid);  // exactly one file
        EXPECT(!ParseLaunchArgs({L"--open-content"}).valid);
        r = ParseLaunchArgs({L"--hash", L"C:\\a.zip", L"C:\\b.txt"});  // several files, not a quick action (the hash dialog is shown in the main window)
        EXPECT(r.valid && r.action == LaunchAction::Hash && r.paths.size() == 2 && !r.IsQuickAction() && !r.IsPreviewOnly());
        // checksum files: --verify, or the files alone (one or several); a mix with other files is a normal add
        r = ParseLaunchArgs({L"--verify", L"C:\\a.sha256", L"C:\\b.md5"});
        EXPECT(r.valid && r.action == LaunchAction::Verify && r.paths.size() == 2 && !r.IsQuickAction());
        r = ParseLaunchArgs({L"C:\\a.SHA256"});
        EXPECT(r.valid && r.action == LaunchAction::Verify && r.paths.size() == 1);
        r = ParseLaunchArgs({L"C:\\a.sfv", L"C:\\SHA256SUMS"});
        EXPECT(r.valid && r.action == LaunchAction::Verify && r.paths.size() == 2);
        r = ParseLaunchArgs({L"C:\\a.sha256", L"C:\\b.txt"});
        EXPECT(r.valid && r.action == LaunchAction::Add);
        EXPECT(!ParseLaunchArgs({L"--verify"}).valid);
        r = ParseLaunchArgs({L"--hash-save", L"C:\\a", L"C:\\b.txt"});
        EXPECT(r.valid && r.action == LaunchAction::HashSave && r.paths.size() == 2 && !r.IsQuickAction() && !r.IsPreviewOnly());
        EXPECT(!ParseLaunchArgs({L"--hash-save"}).valid);
        r = ParseLaunchArgs({L"--compress", L"zip", L"C:\\a.sha256"});
        EXPECT(r.valid && r.action == LaunchAction::Compress);   // asking for a compress still compresses
        EXPECT(!ParseLaunchArgs({L"--hash"}).valid);
        EXPECT(!ParseLaunchArgs({L"--compress", L"zip", L"--template"}).valid);
        ArchiveType t;
        EXPECT(ParseFormatName(L"TAR.XZ", t) && t == ArchiveType::TarXz && !ParseFormatName(L"tar", t) && !ParseFormatName(L"", t));
        EXPECT(std::wstring(FormatName(ArchiveType::TarBz2)) == L"tar.bz2" && std::wstring(FormatName(ArchiveType::Zip)) == L"zip");
        // Serialization keeps the format and the preset (a preset name may contain spaces and ';')
        LaunchRequest in;
        in.action = LaunchAction::Compress;
        in.format = ArchiveType::TarXz;
        in.preset = L"My; backup";
        in.paths = {fs::path(L"C:\\x")};
        auto out = DeserializeLaunchRequest(SerializeLaunchRequest(in));
        EXPECT(out.valid && out.action == in.action && out.format == in.format && out.preset == in.preset && out.paths == in.paths);
        r = ParseLaunchArgs({L"--extract-to", L"--dest", L"D:\\Out Dir", L"C:\\a.zip"});
        EXPECT(r.valid && r.action == LaunchAction::ExtractTo && r.dest == fs::path(L"D:\\Out Dir") && r.paths.size() == 1);
        EXPECT(!ParseLaunchArgs({L"--extract-to", L"C:\\a.zip", L"--dest"}).valid);
        in.action = LaunchAction::ExtractTo;
        in.dest = L"D:\\Out; Dir";
        out = DeserializeLaunchRequest(SerializeLaunchRequest(in));
        EXPECT(out.valid && out.dest == in.dest && out.paths == in.paths);
    }

    // --list: UTF-8 (with BOM, CRLF, blank lines, Chinese and spaces)
    {
        fs::path list = fs::temp_directory_path() / (L"lycan_list_" + std::to_wstring(GetCurrentProcessId()) + L".txt");
        { std::ofstream f(list, std::ios::binary); f << "\xEF\xBB\xBF" "C:\\資料夾\\a b.txt\r\n\r\nD:\\x.bin\n"; }
        auto r = ParseLaunchArgs({L"--add", L"--list", list.wstring()});
        EXPECT(r.valid && r.paths.size() == 2 && r.paths[0] == fs::path(L"C:\\資料夾\\a b.txt") && r.paths[1] == fs::path(L"D:\\x.bin"));
        EXPECT(!ParseLaunchArgs({L"--add", L"--list", L"C:\\no\\such\\list.txt"}).valid);
        std::error_code ec;
        fs::remove(list, ec);
    }

    // Serialization round trip (with special characters other than line breaks)
    {
        LaunchRequest in;
        in.action = LaunchAction::Compress;
        in.paths = {fs::path(L"C:\\資料夾\\a b.txt"), fs::path(L"D:\\x (2).bin")};
        auto out = DeserializeLaunchRequest(SerializeLaunchRequest(in));
        EXPECT(out.valid && out.action == in.action && out.paths == in.paths);
        EXPECT(!DeserializeLaunchRequest(L"").valid);
        EXPECT(!DeserializeLaunchRequest(L"99\nC:\\a").valid);
        EXPECT(!DeserializeLaunchRequest(L"abc").valid);
    }

    // ---- the menu DLL: the version is in the file name; the DLLs of other versions are removed ----
    {
        EXPECT(ShellDllFileName() == L"LycanZipShell-" LYCAN_VERSION_WSTRING L".dll");
        fs::path d = fs::temp_directory_path() / (L"lycan_dlls_" + std::to_wstring(GetCurrentProcessId()));
        fs::create_directories(d);
        for (const wchar_t* n : {L"LycanZipShell.dll", L"LycanZipShell-0.8.0.dll", L"lycanzipshell-0.9.0.dll", L"LycanZipShell-9.9.9.dll", L"Other.dll", L"LycanZip.exe", L"LycanZipShell-notes.txt"})
            std::ofstream(d / n) << "x";
        std::ofstream(ShellDllPath(d)) << "x";
        EXPECT(ShellDllPath(d) == d / ShellDllFileName());
        // one of them is loaded (cannot be deleted): it stays, the others go
        HANDLE held = CreateFileW((d / L"LycanZipShell-9.9.9.dll").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        EXPECT(held != INVALID_HANDLE_VALUE);
        const int removed = RemoveOtherShellDlls(d, ShellDllPath(d));
        CloseHandle(held);
        EXPECT(removed == 3);   // LycanZipShell.dll, -0.8.0, lycanzipshell-0.9.0
        EXPECT(fs::exists(ShellDllPath(d)) && fs::exists(d / L"LycanZipShell-9.9.9.dll"));
        EXPECT(fs::exists(d / L"Other.dll") && fs::exists(d / L"LycanZip.exe") && fs::exists(d / L"LycanZipShell-notes.txt"));
        EXPECT(!fs::exists(d / L"LycanZipShell.dll") && !fs::exists(d / L"LycanZipShell-0.8.0.dll"));
        EXPECT(RemoveOtherShellDlls(d, ShellDllPath(d)) == 1);   // the next time the other one is free
        EXPECT(RemoveOtherShellDlls(d, ShellDllPath(d)) == 0);
        EXPECT(RemoveOtherShellDlls(d / L"nothing here", ShellDllPath(d)) == 0);
        fs::remove_all(d);
    }

    // ---- an installed copy (<root>\current with <root>\Update.exe) keeps the menu DLL and the Windows 11 package outside of current ----
    {
        fs::path root = fs::temp_directory_path() / (L"lycan_home_" + std::to_wstring(GetCurrentProcessId()));
        fs::remove_all(root);
        const fs::path cur = root / L"app" / L"current", data = root / L"data";
        fs::create_directories(cur);
        EXPECT(!IsInstalledLayout(cur));                                   // no Update.exe yet
        EXPECT(ShellHome(cur, data) == cur);
        { std::ofstream(root / L"app" / L"Update.exe") << "x"; }
        EXPECT(IsInstalledLayout(cur) && IsInstalledLayout(cur / L""));
        EXPECT(!IsInstalledLayout(root / L"app"));                         // the folder is not called current
        EXPECT(ShellHome(cur, data) == data / L"shell");
        { std::ofstream(ShellDllPath(cur)) << "dll one"; std::ofstream(cur / L"LycanZipShell.msix") << "package"; }
        const fs::path home = EnsureShellHome(cur, data);
        EXPECT(home == data / L"shell" && fs::exists(ShellDllPath(home)) && fs::exists(home / L"LycanZipShell.msix"));
        EXPECT(fs::file_size(ShellDllPath(home)) == 7);
        {   // the DLL finds the exe through this file (it has no LycanZip.exe beside it)
            std::ifstream in(home / L"LycanZip.exepath", std::ios::binary);
            std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            const auto u8 = (cur / L"LycanZip.exe").u8string();
            EXPECT(s == std::string(reinterpret_cast<const char*>(u8.data()), u8.size()));
        }
        // a newer DLL of the same name replaces the copy; an unchanged one is left alone
        { std::ofstream(ShellDllPath(cur)) << "a longer dll"; }
        EnsureShellHome(cur, data);
        EXPECT(fs::file_size(ShellDllPath(home)) == 12);
        // the copy that is loaded cannot be replaced: nothing throws, the old file stays
        HANDLE held = CreateFileW(ShellDllPath(home).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        { std::ofstream(ShellDllPath(cur)) << "yet another dll"; }
        EXPECT(EnsureShellHome(cur, data) == home);
        CloseHandle(held);
        EXPECT(fs::file_size(ShellDllPath(home)) == 12);
        // not installed: the folder of the exe, nothing copied
        const fs::path plain = root / L"plain";
        fs::create_directories(plain);
        EXPECT(EnsureShellHome(plain, data) == plain);
        fs::remove_all(root);
    }

    // ---- context-menu registration (a temporary key, never the real Classes) ----
    {
        std::wstring base = L"Software\\LycanZipTest_" + std::to_wstring(GetCurrentProcessId());
        fs::path tmp = fs::temp_directory_path() / (L"lycan_reg_" + std::to_wstring(GetCurrentProcessId()));
        fs::create_directories(tmp);
        { std::ofstream(ShellDllPath(tmp)) << "x"; }   // the menu DLL has the version in its name
        ShellRegOptions o;
        o.exe = tmp / L"LycanZip.exe";
        o.classesKey = base + L"\\Classes";
        const std::wstring c = o.classesKey;

        EXPECT(!IsShellMenuRegistered(o));
        std::wstring err;
        // an old-style registration (earlier versions) must be replaced, not left beside the new one
        {
            HKEY k;
            RegCreateKeyExW(HKEY_CURRENT_USER, (c + L"\\*\\shell\\LycanZip\\shell\\01add\\command").c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr);
            RegCloseKey(k);
        }
        EXPECT(RegisterShellMenu(o, &err));
        EXPECT(IsShellMenuRegistered(o));
        EXPECT(ReadReg(c + L"\\*\\shell\\LycanZip\\shell\\01add\\command", nullptr) == L"<missing>");  // the old one is gone

        // the verbs only point to the COM classes
        EXPECT(ReadReg(c + L"\\*\\shell\\LycanZipFiles", L"ExplorerCommandHandler") == kClassicFilesClsid);
        EXPECT(ReadReg(c + L"\\Directory\\shell\\LycanZipFiles", L"ExplorerCommandHandler") == kClassicFilesClsid);
        EXPECT(ReadReg(c + L"\\SystemFileAssociations\\.zip\\shell\\LycanZipArchive", L"ExplorerCommandHandler") == kClassicArchiveClsid);
        EXPECT(ReadReg(c + L"\\SystemFileAssociations\\.cbt\\shell\\LycanZipArchive", L"ExplorerCommandHandler") == kClassicArchiveClsid);
        EXPECT(ReadReg(c + L"\\SystemFileAssociations\\.txt\\shell\\LycanZipArchive", L"ExplorerCommandHandler") == L"<missing>");
        // the right-drag menu: a drag and drop handler on folders, drives and the desktop (a folder)
        for (const wchar_t* cls : {L"Directory", L"Drive", L"Folder"})
            EXPECT(ReadReg(c + L"\\" + cls + L"\\shellex\\DragDropHandlers\\LycanZip", nullptr) == kDragDropClsid);
        EXPECT(ReadReg(c + L"\\*\\shellex\\DragDropHandlers\\LycanZip", nullptr) == L"<missing>");
        // the checksum files: one verb each
        for (const wchar_t* ext : {L".sha256", L".sha1", L".sha384", L".sha512", L".md5", L".sfv"})
            EXPECT(ReadReg(c + L"\\SystemFileAssociations\\" + ext + L"\\shell\\LycanZipVerify", L"ExplorerCommandHandler") == kClassicVerifyClsid);
        EXPECT(ReadReg(c + L"\\SystemFileAssociations\\.txt\\shell\\LycanZipVerify", L"ExplorerCommandHandler") == L"<missing>");
        // ... and the classes point to the DLL
        const std::wstring dll = ShellDllPath(tmp).wstring();
        EXPECT(ReadReg(c + L"\\CLSID\\" + kDragDropClsid + L"\\InprocServer32", nullptr) == dll);
        EXPECT(ReadReg(c + L"\\CLSID\\" + kClassicFilesClsid + L"\\InprocServer32", nullptr) == dll);
        EXPECT(ReadReg(c + L"\\CLSID\\" + kClassicArchiveClsid + L"\\InprocServer32", nullptr) == dll);
        EXPECT(ReadReg(c + L"\\CLSID\\" + kClassicFilesClsid + L"\\InprocServer32", L"ThreadingModel") == L"Apartment");

        // what the registration points to can be read back (a moved portable folder is noticed by comparing it with the running exe)
        EXPECT(RegisteredShellDll(o) == ShellDllPath(tmp));
        EXPECT(RegisteredShellDll(ShellRegOptions{L"x.exe", {}, c + L"\\Nothing"}).empty());

        // registering again / moving: everything points to the new place
        { std::ofstream(tmp / L"Other.dll") << "x"; }
        o.dll = tmp / L"Other.dll";
        EXPECT(RegisterShellMenu(o, &err));
        EXPECT(ReadReg(c + L"\\CLSID\\" + kClassicFilesClsid + L"\\InprocServer32", nullptr) == (tmp / L"Other.dll").wstring());
        EXPECT(RegisteredShellDll(o) == tmp / L"Other.dll");

        // removal
        EXPECT(UnregisterShellMenu(o, &err));
        EXPECT(!IsShellMenuRegistered(o));
        EXPECT(RegisteredShellDll(o).empty());
        EXPECT(ReadReg(c + L"\\SystemFileAssociations\\.zip\\shell\\LycanZipArchive", L"ExplorerCommandHandler") == L"<missing>");
        EXPECT(ReadReg(c + L"\\CLSID\\" + kClassicFilesClsid + L"\\InprocServer32", nullptr) == L"<missing>");
        EXPECT(ReadReg(c + L"\\Directory\\shellex\\DragDropHandlers\\LycanZip", nullptr) == L"<missing>");
        EXPECT(ReadReg(c + L"\\SystemFileAssociations\\.sha256\\shell\\LycanZipVerify", L"ExplorerCommandHandler") == L"<missing>");
        EXPECT(ReadReg(c + L"\\CLSID\\" + kClassicVerifyClsid + L"\\InprocServer32", nullptr) == L"<missing>");
        EXPECT(ReadReg(c + L"\\CLSID\\" + kDragDropClsid + L"\\InprocServer32", nullptr) == L"<missing>");
        EXPECT(UnregisterShellMenu(o, &err));  // not there is fine

        // no exe path, or no DLL next to the exe: fails and says why
        EXPECT(!RegisterShellMenu(ShellRegOptions{}, &err) && !err.empty());
        ShellRegOptions lonely;
        lonely.exe = tmp / L"nowhere" / L"LycanZip.exe";
        lonely.classesKey = c;
        err.clear();
        EXPECT(!RegisterShellMenu(lonely, &err) && !err.empty() && !IsShellMenuRegistered(lonely));

        RegDeleteTreeW(HKEY_CURRENT_USER, base.c_str());  // the whole test key
        std::error_code ec;
        fs::remove_all(tmp, ec);
    }

    if (failures == 0) std::puts("test_launch_shell: all passed");
    return failures ? 1 : 0;
}
