// Context-menu extension (IExplorerCommand) tested in process: load the DLL, call the methods with real IShellItemArray
// objects, and check the command lines that are actually sent with a stand-in exe (echo_args). Nothing is registered with
// Explorer and the registry is not touched. Settings come from a temporary folder (LYCAN_SETTINGS_DIR).
#include "no_dialogs.h"
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "lycan/launch.h"
#include "lycan/shellitems.h"

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

static int failures = 0;
#define EXPECT(cond)                                                     \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); \
            ++failures;                                                  \
        }                                                                \
    } while (0)

// {5E7C1F3A-8B44-4D6E-9A21-3C0F7D2B6A10/11} = Windows 11 package, ...20/21 = classic registration (shellext.cpp, AppxManifest, shellreg.cpp)
static const GUID kFiles = {0x5E7C1F3A, 0x8B44, 0x4D6E, {0x9A, 0x21, 0x3C, 0x0F, 0x7D, 0x2B, 0x6A, 0x10}};
static const GUID kArchive = {0x5E7C1F3A, 0x8B44, 0x4D6E, {0x9A, 0x21, 0x3C, 0x0F, 0x7D, 0x2B, 0x6A, 0x11}};
static const GUID kClassicFiles = {0x5E7C1F3A, 0x8B44, 0x4D6E, {0x9A, 0x21, 0x3C, 0x0F, 0x7D, 0x2B, 0x6A, 0x20}};
static const GUID kClassicArchive = {0x5E7C1F3A, 0x8B44, 0x4D6E, {0x9A, 0x21, 0x3C, 0x0F, 0x7D, 0x2B, 0x6A, 0x21}};
// ...24 = the classic menu of the Store package (IContextMenu), ...27 = the one command of the Store package (shellext/shellext.cpp and the manifest of the Store package)
static const GUID kPackagedClassic = {0x5E7C1F3A, 0x8B44, 0x4D6E, {0x9A, 0x21, 0x3C, 0x0F, 0x7D, 0x2B, 0x6A, 0x24}};
static const GUID kAuto = {0x5E7C1F3A, 0x8B44, 0x4D6E, {0x9A, 0x21, 0x3C, 0x0F, 0x7D, 0x2B, 0x6A, 0x27}};

static ComPtr<IShellItemArray> MakeArray(const std::vector<fs::path>& paths) {
    std::vector<PIDLIST_ABSOLUTE> pidls;
    for (auto& p : paths) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (FAILED(SHParseDisplayName(p.c_str(), nullptr, &pidl, 0, nullptr))) return nullptr;
        pidls.push_back(pidl);
    }
    ComPtr<IShellItemArray> arr;
    SHCreateShellItemArrayFromIDLists(static_cast<UINT>(pidls.size()), const_cast<PCIDLIST_ABSOLUTE_ARRAY>(pidls.data()), &arr);
    for (auto p : pidls) CoTaskMemFree(p);
    return arr;
}

static std::wstring Str(PWSTR s) {
    std::wstring r = s ? s : L"";
    CoTaskMemFree(s);
    return r;
}

static std::wstring ReadEcho(const fs::path& f) {
    std::ifstream in(f, std::ios::binary);
    std::string raw((std::istreambuf_iterator<char>(in)), {});
    return std::wstring(reinterpret_cast<const wchar_t*>(raw.data()), raw.size() / sizeof(wchar_t));
}

static bool WaitForEcho(const fs::path& f, size_t lines, int ms = 8000) {
    ULONGLONG end = GetTickCount64() + ms;
    while (GetTickCount64() < end) {
        std::wstring s = ReadEcho(f);
        if (static_cast<size_t>(std::count(s.begin(), s.end(), L'\n')) >= lines) return true;
        Sleep(50);
    }
    return false;
}

using Entry = std::pair<std::wstring, ComPtr<IExplorerCommand>>;

// The sub menu of a command, in order (title, object)
static std::vector<Entry> Subs(ComPtr<IExplorerCommand>& cmd) {
    std::vector<Entry> v;
    ComPtr<IEnumExplorerCommand> en;
    if (FAILED(cmd->EnumSubCommands(&en)) || !en) return v;
    ComPtr<IExplorerCommand> c;
    ULONG got = 0;
    while (en->Next(1, &c, &got) == S_OK && got == 1) {
        PWSTR t = nullptr;
        c->GetTitle(nullptr, &t);
        v.emplace_back(Str(t), c);
        c.Reset();
    }
    return v;
}

static ComPtr<IExplorerCommand> Find(std::vector<Entry>& v, const std::wstring& title) {
    for (auto& e : v) if (e.first == title) return e.second;
    return nullptr;
}

// "exe,-id" of the icon of an entry (the test's stand-in exe is echo_args.exe: only the end of the string is compared)
static std::wstring IconOf(const ComPtr<IExplorerCommand>& c) {
    PWSTR p = nullptr;
    if (!c || FAILED(c->GetIcon(nullptr, &p))) return L"<none>";
    return Str(p);
}
static bool IconIs(const ComPtr<IExplorerCommand>& c, const wchar_t* suffix) {
    const std::wstring s = IconOf(c), e = suffix;
    return s.size() > e.size() && s.compare(s.size() - e.size(), e.size(), e) == 0;
}

static std::vector<std::wstring> Titles(const std::vector<Entry>& v) {
    std::vector<std::wstring> t;
    for (auto& e : v) t.push_back(e.first);
    return t;
}

int main(int argc, char** argv) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (argc < 3) { std::fprintf(stderr, "usage: test_shellext <LycanZipShell.dll> <echo_args.exe>\n"); return 2; }

    fs::path dllPath = fs::absolute(argv[1]), echoExe = fs::absolute(argv[2]);
    fs::path root = fs::temp_directory_path() / (L"lycan_se_" + std::to_wstring(GetCurrentProcessId()));
    fs::create_directories(root);
    auto touch = [&](const wchar_t* n) { fs::path p = root / n; std::ofstream(p) << "x"; return p; };
    fs::path a = touch(L"a b.txt"), b = touch(L"資料.bin"), z = touch(L"x.ZIP"), z2 = touch(L"y.7z"), tgz = touch(L"data.tar.gz");
    fs::path exe = touch(L"setup.exe");
    fs::path dir = root / L"子資料夾";
    fs::create_directories(dir);
    const std::wstring rootName = root.filename().wstring();  // the name of the folder holding the files

    // settings (a folder of its own) and the stand-in exe
    fs::path settingsDir = root / L"settings";
    fs::create_directories(settingsDir);
    // the file differs in size every time: the extension re-reads settings when the time or the size changes, and two writes can get the same time
    int writes = 0;
    // The DLL re-reads the file when its time or size changes. Two writes can fall in one tick of the clock (a fast disk) and still have
    // the same size ("false" is one letter longer than "true", the padding one shorter), so each write also gets a later time of its own
    auto writeSettings = [&](const std::string& text) {
        const fs::path p = settingsDir / L"settings.ini";
        std::ofstream(p, std::ios::binary | std::ios::trunc) << text << "# " << std::string(++writes, 'x') << "\n";
        std::error_code ec;
        fs::last_write_time(p, fs::file_time_type::clock::now() + std::chrono::seconds(writes), ec);
    };
    writeSettings("language=zh-TW\n");
    fs::path echoOut = root / "echo.txt";
    SetEnvironmentVariableW(L"LYCAN_EXE", echoExe.c_str());
    SetEnvironmentVariableW(L"LYCAN_ECHO_OUT", echoOut.c_str());
    SetEnvironmentVariableW(L"LYCAN_SETTINGS_DIR", settingsDir.c_str());

    HMODULE dll = LoadLibraryW(dllPath.c_str());
    EXPECT(dll != nullptr);
    if (!dll) return 1;
    using GetClassObjectFn = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
    auto getClass = reinterpret_cast<GetClassObjectFn>(GetProcAddress(dll, "DllGetClassObject"));
    auto canUnload = reinterpret_cast<HRESULT(STDAPICALLTYPE*)()>(GetProcAddress(dll, "DllCanUnloadNow"));
    EXPECT(getClass && canUnload);

    auto create = [&](const GUID& clsid) {
        ComPtr<IClassFactory> f;
        ComPtr<IExplorerCommand> cmd;
        if (getClass && SUCCEEDED(getClass(clsid, IID_PPV_ARGS(&f)))) f->CreateInstance(nullptr, IID_PPV_ARGS(&cmd));
        return cmd;
    };
    // a fresh object per selection, like Explorer: the menu is built from the selection given to GetState
    auto state = [](ComPtr<IExplorerCommand>& c, ComPtr<IShellItemArray> items) {
        EXPCMDSTATE s = ECS_ENABLED;
        c->GetState(items.Get(), FALSE, &s);
        return s;
    };

    {   // the four CLSIDs all exist
        EXPECT(create(kFiles) && create(kArchive) && create(kClassicFiles) && create(kClassicArchive));
        PWSTR t = nullptr;
        auto c = create(kFiles);
        EXPECT(c && SUCCEEDED(c->GetTitle(nullptr, &t)) && Str(t) == L"LycanZip");
        EXPCMDFLAGS fl = ECF_DEFAULT;
        c->GetFlags(&fl);
        EXPECT((fl & ECF_HASSUBCOMMANDS) != 0);
    }

    auto txtItems = MakeArray({a, b});
    auto oneTxt = MakeArray({a});
    auto zipItems = MakeArray({z, z2});
    auto oneZip = MakeArray({z});
    auto mixedItems = MakeArray({a, z});
    auto dirItems = MakeArray({dir});
    auto exeItem = MakeArray({exe});
    auto tgzItem = MakeArray({tgz});
    EXPECT(txtItems && oneTxt && zipItems && oneZip && mixedItems && dirItems && exeItem && tgzItem);

    // ---- when each class shows ----
    {
        auto f = create(kFiles), ar = create(kArchive);
        EXPECT(state(f, txtItems) == ECS_ENABLED && state(ar, txtItems) == ECS_HIDDEN);
        f = create(kFiles); ar = create(kArchive);
        EXPECT(state(f, zipItems) == ECS_HIDDEN && state(ar, zipItems) == ECS_ENABLED);  // all archives (extension case does not matter)
        f = create(kFiles); ar = create(kArchive);
        EXPECT(state(f, mixedItems) == ECS_ENABLED && state(ar, mixedItems) == ECS_HIDDEN);  // a mix: only the files' entries
        f = create(kFiles); ar = create(kArchive);
        EXPECT(state(f, dirItems) == ECS_ENABLED && state(ar, dirItems) == ECS_HIDDEN);
        f = create(kFiles);
        EXPCMDSTATE none = ECS_ENABLED;
        f->GetState(nullptr, FALSE, &none);
        EXPECT(none == ECS_HIDDEN);  // nothing selected
    }

    // ---- the menu for files and folders ----
    auto menuOf = [&](const GUID& clsid, ComPtr<IShellItemArray> items) {
        auto c = create(clsid);
        EXPCMDSTATE s = ECS_ENABLED;
        c->GetState(items.Get(), FALSE, &s);
        return std::make_pair(c, Subs(c));
    };
    auto contains = [](const std::vector<std::wstring>& v, const std::wstring& t) { return std::find(v.begin(), v.end(), t) != v.end(); };

    {
        // several files: Add, two Compress-to, Compress each (several only), presets
        auto [c, subs] = menuOf(kFiles, txtItems);
        auto t = Titles(subs);
        EXPECT(t.size() == 7 && t[0] == L"加入壓縮檔…" && t[1] == L"壓縮成 \"" + rootName + L".zip\"" && t[2] == L"壓縮成 \"" + rootName + L".7z\"" &&
               t[3] == L"分別壓縮成 ZIP" && t[4] == L"用範本壓縮" && t[5] == L"計算雜湊值…" && t[6] == L"計算並儲存為 \"" + rootName + L".sha256\"");   // several items: the name of the folder they are in
    }
    {
        // the icons: the format's file-type icon for compressing to it, a badge icon for what the entry does (lycan/fileicons.h, shellext.cpp)
        auto [c, subs] = menuOf(kFiles, txtItems);
        EXPECT(IconIs(subs[0].second, L",0"));           // add: the LycanZip icon
        EXPECT(IconIs(subs[1].second, L",-101"));        // compress to ZIP
        EXPECT(IconIs(subs[2].second, L",-102"));        // compress to 7z
        EXPECT(IconIs(subs[3].second, L",-101"));        // compress each as ZIP
        EXPECT(IconIs(subs[4].second, L",-117"));        // templates
        EXPECT(IconIs(subs[5].second, L",-116"));        // hash values
    }
    {
        // one file: named after the file; no "each"; a folder: named after the folder
        auto [c, subs] = menuOf(kFiles, oneTxt);
        auto t = Titles(subs);
        EXPECT(t.size() == 6 && t[1] == L"壓縮成 \"a b.zip\"" && t[2] == L"壓縮成 \"a b.7z\"" && !contains(t, L"分別壓縮成 ZIP") && contains(t, L"計算雜湊值…"));
        auto [c2, subs2] = menuOf(kFiles, dirItems);
        EXPECT(subs2.size() == 6 && subs2[1].first == L"壓縮成 \"子資料夾.zip\"" && contains(Titles(subs2), L"計算雜湊值…"));  // folders too: every file in them
    }
    {
        // the preset sub menu: the built-in presets that are switched on (by default smallest, backup, store only), the user's
        // presets after them
        {
            std::ofstream f(settingsDir / L"presets.ini", std::ios::binary | std::ios::trunc);
            f << "\xEF\xBB\xBF[我的範本]\nformat=7z\n\n[Backup]\nformat=zip\n\n[Bad [name]\nformat=zip\n";  // "Backup" is a built-in: not listed twice
        }
        auto [c, subs] = menuOf(kFiles, oneTxt);
        auto tmpl = Find(subs, L"用範本壓縮");
        EXPECT(tmpl != nullptr);
        EXPCMDFLAGS fl = ECF_DEFAULT;
        if (tmpl) tmpl->GetFlags(&fl);
        EXPECT((fl & ECF_HASSUBCOMMANDS) != 0);
        auto presets = tmpl ? Subs(tmpl) : std::vector<Entry>{};
        auto t = Titles(presets);
        EXPECT(t == (std::vector<std::wstring>{L"最小體積", L"備份", L"僅儲存不壓縮", L"我的範本"}));
        // choosing one passes its name (quoted) to --template
        DeleteFileW(echoOut.c_str());
        if (!presets.empty()) EXPECT(SUCCEEDED(presets.back().second->Invoke(oneTxt.Get(), nullptr)));
        EXPECT(WaitForEcho(echoOut, 1));
        EXPECT(ReadEcho(echoOut).find(L"--compress zip --template \"我的範本\" \"" + a.wstring() + L"\"") != std::wstring::npos);
        // the built-in ones by the setting; a preset of the user with in_menu=false is not listed; no preset at all: no sub menu
        writeSettings("language=zh-TW\nshell_presets=zip,fast\n");
        {
            std::ofstream f(settingsDir / L"presets.ini", std::ios::binary | std::ios::trunc);
            f << "[顯示]\nformat=zip\nin_menu=true\n\n[隱藏]\nformat=zip\nin_menu=false\nlevel=5\n\n[隱藏2]\nin_menu=false\nformat=7z\n\n[預設]\nformat=zip\n";
        }
        auto [c2, subs2] = menuOf(kFiles, oneTxt);
        auto tmpl2 = Find(subs2, L"用範本壓縮");
        EXPECT(tmpl2 != nullptr);
        EXPECT(tmpl2 && Titles(Subs(tmpl2)) == (std::vector<std::wstring>{L"一般 ZIP", L"快速", L"顯示", L"預設"}));
        writeSettings("language=zh-TW\nshell_presets=-\n");
        {
            std::ofstream f(settingsDir / L"presets.ini", std::ios::binary | std::ios::trunc);
            f << "[隱藏]\nformat=zip\nin_menu=false\n";
        }
        auto [c3, subs3] = menuOf(kFiles, oneTxt);
        EXPECT(Find(subs3, L"用範本壓縮") == nullptr);
        writeSettings("language=zh-TW\n");
        DeleteFileW((settingsDir / L"presets.ini").c_str());
    }
    {
        // single .exe: "Open contents"; a plain file: not
        auto [c, subs] = menuOf(kFiles, exeItem);
        auto t = Titles(subs);
        EXPECT(contains(t, L"以 LycanZip 開啟內容"));
        auto [c2, subs2] = menuOf(kFiles, oneTxt);
        EXPECT(!contains(Titles(subs2), L"以 LycanZip 開啟內容"));
    }

    // ---- the menu for archives ----
    {
        auto [c, subs] = menuOf(kArchive, oneZip);
        auto t = Titles(subs);
        EXPECT(t.size() == 8 && t[0] == L"開啟" && t[1] == L"解壓縮到此處" && t[2] == L"解壓縮到 \"x\\\"" && t[3] == L"解壓縮到…" && t[4] == L"測試完整性" &&
               t[5] == L"圖形預覽" && t[6] == L"計算雜湊值…" && t[7] == L"計算並儲存為 \"x.ZIP.sha256\"");   // one item: its own name
        auto [c2, subs2] = menuOf(kArchive, zipItems);  // several: the generic folder entry, no previews
        auto t2 = Titles(subs2);
        EXPECT(t2.size() == 7 && t2[2] == L"解壓縮到同名資料夾" && !contains(t2, L"圖形預覽") && contains(t2, L"計算雜湊值…"));
        auto [c3, subs3] = menuOf(kArchive, tgzItem);  // "data.tar.gz" -> "data"
        EXPECT(subs3.size() == 8 && subs3[2].first == L"解壓縮到 \"data\\\"");
        // icons: open shows the type of the archive; extract, test, images and hash have their own
        EXPECT(IconIs(subs[0].second, L",-101") && IconIs(subs3[0].second, L",-104"));  // x.ZIP -> ZIP, data.tar.gz -> TAR
        EXPECT(IconIs(subs2[0].second, L",0"));                                             // x.ZIP and y.7z: mixed, the LycanZip icon
        EXPECT(IconIs(subs[1].second, L",-111") && IconIs(subs[3].second, L",-111") && IconIs(subs[4].second, L",-112") &&
               IconIs(subs[5].second, L",-113") && IconIs(subs[6].second, L",-116"));
    }

    // ---- settings change the menu (no re-registration) ----
    {
        writeSettings("language=zh-TW\nshell_items=add,audio,merge,images,test,encrypt,more\nshell_format_1=tar.xz\nshell_format_2=7z\n");
        Sleep(20);
        auto [c, subs] = menuOf(kFiles, oneTxt);
        auto t = Titles(subs);
        EXPECT(t.size() == 3 && t[0] == L"加入壓縮檔…" && t[1] == L"其他格式" && t[2] == L"加密壓縮…");  // order of the menu, not of the setting
        auto more = Find(subs, L"其他格式");
        auto kids = more ? Subs(more) : std::vector<Entry>{};
        EXPECT(Titles(kids) == (std::vector<std::wstring>{L"tar.gz", L"tar.xz", L"tar.bz2"}));
        auto [c2, subs2] = menuOf(kArchive, oneZip);
        EXPECT(Titles(subs2) == (std::vector<std::wstring>{L"測試完整性", L"圖形預覽", L"音訊預覽"}));
        auto [c3, subs3] = menuOf(kArchive, zipItems);
        EXPECT(Titles(subs3) == (std::vector<std::wstring>{L"多個壓縮檔合併解壓", L"測試完整性"}));  // merge only for several archives
        writeSettings("language=zh-TW\nshell_format_1=tar.xz\nshell_format_2=7z\n");
        auto [c4, subs4] = menuOf(kFiles, oneTxt);
        EXPECT(subs4.size() == 6 && subs4[1].first == L"壓縮成 \"a b.tar.xz\"" && subs4[2].first == L"壓縮成 \"a b.7z\"");

        // everything off: the menu disappears
        writeSettings("language=zh-TW\nshell_items=-\n");
        auto f = create(kFiles);
        EXPECT(state(f, oneTxt) == ECS_HIDDEN);
        auto ar = create(kArchive);
        EXPECT(state(ar, oneZip) == ECS_HIDDEN);
        // a bad value falls back to the defaults
        writeSettings("language=zh-TW\nshell_format_1=rar\nshell_items=bogus\n");
        auto f2 = create(kFiles);
        EXPECT(state(f2, oneTxt) == ECS_HIDDEN);  // only unknown ids = none, like "-"
        writeSettings("language=zh-TW\nshell_format_1=rar\n");
        auto [c5, subs5] = menuOf(kFiles, oneTxt);
        EXPECT(subs5.size() == 6 && subs5[1].first == L"壓縮成 \"a b.zip\"");
    }

    // ---- recent folders ----
    {
        writeSettings("language=zh-TW\nshell_items=recent\nrecent_extract_dirs=D:\\Out One|E:\\中文 資料夾|relative\\no\n");
        auto [c, subs] = menuOf(kArchive, oneZip);
        auto recent = Find(subs, L"解壓縮到最近用過的位置");
        EXPECT(subs.size() == 1 && recent != nullptr);
        auto kids = recent ? Subs(recent) : std::vector<Entry>{};
        EXPECT(kids.size() == 2 && kids[0].first == L"D:\\Out One" && kids[1].first == L"E:\\中文 資料夾");  // the relative path is dropped
        DeleteFileW(echoOut.c_str());
        if (kids.size() == 2) EXPECT(SUCCEEDED(kids[0].second->Invoke(oneZip.Get(), nullptr)));
        EXPECT(WaitForEcho(echoOut, 1));
        auto line = ReadEcho(echoOut);
        EXPECT(line.find(L"--extract-to --dest \"D:\\Out One\" \"" + z.wstring() + L"\"") != std::wstring::npos);
        auto req = lycan::ParseLaunchArgs({L"--extract-to", L"--dest", L"D:\\Out One", z.wstring()});
        EXPECT(req.valid && req.action == lycan::LaunchAction::ExtractTo && req.dest == fs::path(L"D:\\Out One"));
    }

    // ---- languages ----
    {
        writeSettings("language=en-US\n");
        auto [c, subs] = menuOf(kFiles, oneTxt);
        EXPECT(subs.size() == 6 && subs[0].first == L"Add to archive…" && subs[1].first == L"Compress to \"a b.zip\"" && subs[4].first == L"Calculate hash values…");
        auto [c2, subs2] = menuOf(kArchive, oneZip);
        EXPECT(subs2.size() == 8 && subs2[1].first == L"Extract here" && subs2[2].first == L"Extract to \"x\\\"");
        writeSettings("language=zh-CN\n");
        auto [c3, subs3] = menuOf(kFiles, oneTxt);
        EXPECT(subs3.size() == 6 && subs3[0].first == L"添加到压缩文件…" && subs3[4].first == L"计算哈希值…");
        writeSettings("language=zh-TW\n");
    }

    // ---- long names are cut in the middle ----
    {
        fs::path longFile = touch(L"這是一個非常非常長的檔案名稱用來測試選單標題會不會太長.txt");
        auto items = MakeArray({longFile});
        auto [c, subs] = menuOf(kFiles, items);
        EXPECT(subs.size() == 6 && subs[1].first.find(L"…") != std::wstring::npos && subs[1].first.size() < 40);
    }

    // ---- invoking: the command lines that are sent ----
    writeSettings("language=zh-TW\nshell_items=add,fmt1,fmt2,each,encrypt,opencontent,open,here,folder,to,test,images,audio,merge,hashsave\n");
    DeleteFileW(echoOut.c_str());
    size_t expectLines = 0;
    auto invoke = [&](const GUID& clsid, ComPtr<IShellItemArray> items, const std::wstring& title) {
        auto [c, subs] = menuOf(clsid, items);
        auto cmd = Find(subs, title);
        EXPECT(cmd != nullptr);
        if (!cmd) return;
        EXPECT(SUCCEEDED(cmd->Invoke(items.Get(), nullptr)));
        ++expectLines;
    };
    invoke(kFiles, txtItems, L"加入壓縮檔…");
    invoke(kFiles, oneTxt, L"計算並儲存為 \"a b.txt.sha256\"");
    invoke(kFiles, txtItems, L"壓縮成 \"" + rootName + L".zip\"");
    invoke(kFiles, dirItems, L"壓縮成 \"子資料夾.7z\"");
    invoke(kFiles, txtItems, L"分別壓縮成 ZIP");
    invoke(kFiles, oneTxt, L"加密壓縮…");
    invoke(kFiles, exeItem, L"以 LycanZip 開啟內容");
    invoke(kArchive, zipItems, L"解壓縮到此處");
    invoke(kArchive, zipItems, L"解壓縮到同名資料夾");
    invoke(kArchive, zipItems, L"解壓縮到…");
    invoke(kArchive, zipItems, L"測試完整性");
    invoke(kArchive, zipItems, L"多個壓縮檔合併解壓");
    invoke(kArchive, oneZip, L"圖形預覽");
    invoke(kArchive, oneZip, L"音訊預覽");
    invoke(kClassicArchive, oneZip, L"解壓縮到此處");  // the classic registration behaves the same
    invoke(kClassicFiles, oneTxt, L"加入壓縮檔…");
    invoke(kArchive, zipItems, L"開啟");  // two archives: one process each
    expectLines += 1;
    EXPECT(WaitForEcho(echoOut, expectLines));
    std::wstring echo = ReadEcho(echoOut);
    auto has = [&](const std::wstring& s) { return echo.find(s) != std::wstring::npos; };
    auto q = [](const fs::path& p) { return L"\"" + p.wstring() + L"\""; };
    // the processes write their lines at their own pace (a loaded CI runner): the last single-archive lines may come a moment after the count is reached
    for (int i = 0; i < 100 && !(has(L"--extract-here " + q(z) + L"\n") && has(q(z2) + L"\n")); ++i) { Sleep(100); echo = ReadEcho(echoOut); }
    EXPECT(has(L"--add " + q(a) + L" " + q(b)));
    EXPECT(has(L"--hash-save " + q(a)));
    EXPECT(has(L"--compress zip " + q(a) + L" " + q(b)));
    EXPECT(has(L"--compress 7z " + q(dir)));
    EXPECT(has(L"--compress-each zip " + q(a) + L" " + q(b)));
    EXPECT(has(L"--add-encrypted " + q(a)));
    EXPECT(has(L"--open-content " + q(exe)));
    EXPECT(has(L"--extract-here " + q(z) + L" " + q(z2)));
    EXPECT(has(L"--extract-folder " + q(z) + L" " + q(z2)));
    EXPECT(has(L"--extract-to " + q(z) + L" " + q(z2)));
    EXPECT(has(L"--test " + q(z) + L" " + q(z2)));
    EXPECT(has(L"--extract-merge " + q(z) + L" " + q(z2)));
    EXPECT(has(L"--preview-images " + q(z)));
    EXPECT(has(L"--preview-audio " + q(z)));
    EXPECT(has(L"--extract-here " + q(z) + L"\n"));
    EXPECT(has(q(z) + L"\n") && has(q(z2) + L"\n"));  // Open: the path alone, one process per archive

    // every command line the menu sends must be understood by LycanZip's own parser
    for (const wchar_t* line : {L"--compress tar.xz \"x\"", L"--compress zip --template \"我的 範本\" \"x\"", L"--compress-each 7z \"x\" \"y\"",
                                L"--add-encrypted \"x\"", L"--extract-to --dest \"D:\\Out One\" \"x.zip\"", L"--extract-merge \"x.zip\" \"y.zip\"",
                                L"--test \"x.zip\"", L"--preview-images \"x.zip\"", L"--preview-audio \"x.zip\"", L"--open-content \"x.exe\""}) {
        const std::wstring full = std::wstring(L"\"x.exe\" ") + line;
        int n = 0;
        LPWSTR* av = CommandLineToArgvW(full.c_str(), &n);
        std::vector<std::wstring> args(av + 1, av + n);
        LocalFree(av);
        auto req = lycan::ParseLaunchArgs(args);
        EXPECT(req.valid && req.action != lycan::LaunchAction::None);
    }

    // ---- many paths: a list file, deleted by LycanZip after reading ----
    {
        std::vector<fs::path> many;
        for (int i = 0; i < 700; ++i) {
            std::wstring name = L"長檔名測試檔案_" + std::to_wstring(i) + std::wstring(40, L'x') + L".txt";
            many.push_back(touch(name.c_str()));
        }
        auto big = MakeArray(many);
        EXPECT(big != nullptr);
        DeleteFileW(echoOut.c_str());
        auto [c, subs] = menuOf(kFiles, big);
        auto cmd = Find(subs, L"壓縮成 \"" + rootName + L".zip\"");
        EXPECT(cmd != nullptr);
        if (cmd) EXPECT(SUCCEEDED(cmd->Invoke(big.Get(), nullptr)));
        EXPECT(WaitForEcho(echoOut, 1));
        std::wstring line = ReadEcho(echoOut);
        EXPECT(line.size() < 600);  // a short command line
        auto pos = line.find(L"--list \"");
        EXPECT(pos != std::wstring::npos);
        if (pos != std::wstring::npos) {
            std::wstring listFile = line.substr(pos + 8);
            listFile = listFile.substr(0, listFile.find(L'"'));
            EXPECT(fs::exists(listFile));
            auto req = lycan::ParseLaunchArgs({L"--compress", L"zip", L"--list", listFile});
            EXPECT(req.valid && req.action == lycan::LaunchAction::Compress && req.paths.size() == 700);
            EXPECT(req.paths.size() == 700 && req.paths[699] == many[699]);
            EXPECT(!fs::exists(listFile));  // deleted after reading
        }
    }

    // ---- the right-button drag menu (IShellExtInit + IContextMenu): files dropped on a folder ----
    {
        writeSettings("language=zh-TW\nshell_items=add,fmt1,fmt2,each,encrypt,opencontent,open,here,folder,to,test,images,audio,merge,hashsave\n");
        const GUID kDrag = {0x5E7C1F3A, 0x8B44, 0x4D6E, {0x9A, 0x21, 0x3C, 0x0F, 0x7D, 0x2B, 0x6A, 0x22}};
        // the dropped items as Explorer hands them over: a data object (shell data object: CF_HDROP too) and the target folder
        auto dataOf = [&](const std::vector<fs::path>& paths) {
            std::vector<PIDLIST_ABSOLUTE> pidls;
            for (auto& p : paths) { PIDLIST_ABSOLUTE pidl = nullptr; SHParseDisplayName(p.c_str(), nullptr, &pidl, 0, nullptr); pidls.push_back(pidl); }
            std::vector<PCUITEMID_CHILD> kids;
            for (auto p : pidls) kids.push_back(ILFindLastID(p));
            PIDLIST_ABSOLUTE parent = ILClone(pidls[0]);
            ILRemoveLastID(parent);
            ComPtr<IDataObject> data;
            SHCreateDataObject(parent, static_cast<UINT>(kids.size()), kids.data(), nullptr, IID_PPV_ARGS(&data));
            CoTaskMemFree(parent);
            for (auto p : pidls) CoTaskMemFree(p);
            return data;
        };
        // creates the handler, initializes it with the drop folder and returns the titles of its sub menu (and the object)
        auto dragMenu = [&](const std::vector<fs::path>& paths, const fs::path& target, ComPtr<IContextMenu>& cm, HMENU& menu) {
            ComPtr<IClassFactory> f;
            ComPtr<IShellExtInit> init;
            if (!getClass || FAILED(getClass(kDrag, IID_PPV_ARGS(&f))) || FAILED(f->CreateInstance(nullptr, IID_PPV_ARGS(&init)))) return std::vector<std::wstring>{};
            PIDLIST_ABSOLUTE tp = nullptr;
            SHParseDisplayName(target.c_str(), nullptr, &tp, 0, nullptr);
            auto data = dataOf(paths);
            const HRESULT hr = init->Initialize(tp, data.Get(), nullptr);
            CoTaskMemFree(tp);
            if (FAILED(hr) || FAILED(init.As(&cm))) return std::vector<std::wstring>{};
            menu = CreatePopupMenu();
            const HRESULT q = cm->QueryContextMenu(menu, 0, 1, 0x7FFF, CMF_NORMAL);
            std::vector<std::wstring> titles;
            if (FAILED(q) || GetMenuItemCount(menu) < 1) return titles;
            MENUITEMINFOW mi{sizeof(mi)};
            mi.fMask = MIIM_SUBMENU;
            GetMenuItemInfoW(menu, 0, TRUE, &mi);
            wchar_t top[64] = {};
            GetMenuStringW(menu, 0, top, 64, MF_BYPOSITION);
            if (std::wstring(top) != L"LycanZip" || !mi.hSubMenu) return titles;
            for (int i = 0; i < GetMenuItemCount(mi.hSubMenu); ++i) {
                wchar_t t[256] = {};
                GetMenuStringW(mi.hSubMenu, i, t, 256, MF_BYPOSITION);
                titles.push_back(t);
            }
            return titles;
        };
        auto run = [&](ComPtr<IContextMenu>& cm, UINT index) {
            CMINVOKECOMMANDINFO ci{sizeof(ci)};
            ci.lpVerb = MAKEINTRESOURCEA(index);
            return cm->InvokeCommand(&ci);
        };
        const fs::path drop = root / L"放開的資料夾";
        fs::create_directories(drop);

        {   // files: Compress to "name.zip" / "name.7z" go into the drop folder
            ComPtr<IContextMenu> cm;
            HMENU menu = nullptr;
            auto titles = dragMenu({a, b}, drop, cm, menu);
            EXPECT(titles == (std::vector<std::wstring>{L"壓縮成 \"" + rootName + L".zip\"", L"壓縮成 \"" + rootName + L".7z\"", L"分別壓縮成 ZIP"}));
            if (cm) {
                DeleteFileW(echoOut.c_str());
                EXPECT(SUCCEEDED(run(cm, 0)));
                EXPECT(WaitForEcho(echoOut, 1));
                const std::wstring line = ReadEcho(echoOut);
                EXPECT(line.find(L"--compress zip --dest \"" + drop.wstring() + L"\" \"" + a.wstring() + L"\" \"" + b.wstring() + L"\"") != std::wstring::npos);
                auto req = lycan::ParseLaunchArgs({L"--compress", L"zip", L"--dest", drop.wstring(), a.wstring(), b.wstring()});
                EXPECT(req.valid && req.action == lycan::LaunchAction::Compress && req.dest == drop);
                CMINVOKECOMMANDINFO bad{sizeof(bad)};
                bad.lpVerb = MAKEINTRESOURCEA(99);
                EXPECT(FAILED(cm->InvokeCommand(&bad)));  // not one of ours
            }
            if (menu) DestroyMenu(menu);
        }
        {   // archives: extract into the drop folder
            ComPtr<IContextMenu> cm;
            HMENU menu = nullptr;
            auto titles = dragMenu({z}, drop, cm, menu);
            EXPECT(titles == (std::vector<std::wstring>{L"解壓縮到此處", L"解壓縮到 \"x\\\""}));
            if (cm) {
                DeleteFileW(echoOut.c_str());
                EXPECT(SUCCEEDED(run(cm, 1)));
                EXPECT(WaitForEcho(echoOut, 1));
                EXPECT(ReadEcho(echoOut).find(L"--extract-folder --dest \"" + drop.wstring() + L"\" \"" + z.wstring() + L"\"") != std::wstring::npos);
                auto req = lycan::ParseLaunchArgs({L"--extract-folder", L"--dest", drop.wstring(), z.wstring()});
                EXPECT(req.valid && req.action == lycan::LaunchAction::ExtractFolder && req.dest == drop);
            }
            if (menu) DestroyMenu(menu);
        }
        {   // the settings decide here too: only the entries that are switched on
            writeSettings("language=zh-TW\nshell_items=fmt1\n");
            ComPtr<IContextMenu> cm;
            HMENU menu = nullptr;
            auto titles = dragMenu({a}, drop, cm, menu);
            EXPECT(titles == (std::vector<std::wstring>{L"壓縮成 \"a b.zip\""}));
            if (menu) DestroyMenu(menu);
            writeSettings("language=zh-TW\nshell_items=add,fmt1,fmt2,each,encrypt,opencontent,open,here,folder,to,test,images,audio,merge,hashsave\n");  // as before this block
        }
        {   // a drop on something that is not a folder of the file system: no menu
            ComPtr<IClassFactory> f;
            ComPtr<IShellExtInit> init;
            EXPECT(getClass && SUCCEEDED(getClass(kDrag, IID_PPV_ARGS(&f))) && SUCCEEDED(f->CreateInstance(nullptr, IID_PPV_ARGS(&init))));
            if (init) {
                auto data = dataOf({a});
                EXPECT(FAILED(init->Initialize(nullptr, data.Get(), nullptr)));
                EXPECT(FAILED(init->Initialize(nullptr, nullptr, nullptr)));
            }
        }
    }

    // ---- checksum files: "Verify the checksum file", one window for all the selected files ----
    {
        const GUID kVerifyW11 = {0x5E7C1F3A, 0x8B44, 0x4D6E, {0x9A, 0x21, 0x3C, 0x0F, 0x7D, 0x2B, 0x6A, 0x12}};
        const GUID kVerifyClassic = {0x5E7C1F3A, 0x8B44, 0x4D6E, {0x9A, 0x21, 0x3C, 0x0F, 0x7D, 0x2B, 0x6A, 0x23}};
        fs::path s1 = touch(L"a.sha256"), s2 = touch(L"b.md5");
        auto both = MakeArray({s1, s2});
        EXPECT(both != nullptr);
        for (const GUID& id : {kVerifyW11, kVerifyClassic}) {
            auto v = create(id);
            EXPECT(v != nullptr);
            if (!v) continue;
            PWSTR t = nullptr;
            EXPECT(SUCCEEDED(v->GetTitle(both.Get(), &t)) && Str(t) == L"用 LycanZip 驗證校驗檔");
            EXPCMDSTATE st = ECS_HIDDEN;
            EXPECT(SUCCEEDED(v->GetState(both.Get(), FALSE, &st)) && st == ECS_ENABLED);
            EXPECT(IconIs(v, L",-116"));   // the hash icon
            DeleteFileW(echoOut.c_str());
            EXPECT(SUCCEEDED(v->Invoke(both.Get(), nullptr)));
            EXPECT(WaitForEcho(echoOut, 1));
            const std::wstring line = ReadEcho(echoOut);
            EXPECT(line.find(L"--verify \"" + s1.wstring() + L"\" \"" + s2.wstring() + L"\"") != std::wstring::npos);
            auto req = lycan::ParseLaunchArgs({L"--verify", s1.wstring(), s2.wstring()});
            EXPECT(req.valid && req.action == lycan::LaunchAction::Verify && req.paths.size() == 2);
        }
        writeSettings("language=en-US\nshell_items=add,fmt1,fmt2,each,encrypt,opencontent,open,here,folder,to,test,images,audio,merge,hashsave\n");
        {
            auto v = create(kVerifyClassic);
            PWSTR t = nullptr;
            EXPECT(v && SUCCEEDED(v->GetTitle(nullptr, &t)) && Str(t) == L"Verify the checksum file with LycanZip");
        }
        writeSettings("language=zh-TW\nshell_items=add,fmt1,fmt2,each,encrypt,opencontent,open,here,folder,to,test,images,audio,merge,hashsave\n");
    }

    // ---- the Store package: ONE command for every selection, and the classic menu made of it ----
    {
        const GUID kVerifyClassic = {0x5E7C1F3A, 0x8B44, 0x4D6E, {0x9A, 0x21, 0x3C, 0x0F, 0x7D, 0x2B, 0x6A, 0x23}};   // the checksum file command of the classic registration: its title is the one to expect
        const fs::path sum = touch(L"c.sha256");
        auto sumItem = MakeArray({sum});
        auto sumAndZip = MakeArray({sum, z});
        EXPECT(sumItem && sumAndZip);
        // the entries are those of the separate commands, by what is selected, but with no sub menu (the Windows 11 menu opens a
        // sub menu inside the LycanZip menu empty): the entries of a sub menu are listed in place as "sub menu：entry", between lines ("")
        writeSettings("language=zh-TW\nshell_items=add,fmt1,template,more,hash\n");
        auto flatOf = [&](const GUID& clsid, ComPtr<IShellItemArray> items) {
            auto subs = menuOf(clsid, items).second;
            std::vector<std::wstring> v;
            auto line = [&] { if (!v.empty() && !v.back().empty()) v.push_back(L""); };
            for (auto& e : subs) {
                EXPCMDFLAGS fl = ECF_DEFAULT;
                e.second->GetFlags(&fl);
                if (!(fl & ECF_HASSUBCOMMANDS)) { v.push_back(e.first); continue; }
                line();
                for (auto& k : Subs(e.second)) v.push_back(e.first + L"：" + k.first);
                line();
            }
            if (!v.empty() && v.back().empty()) v.pop_back();
            return v;
        };
        EXPECT(Titles(menuOf(kAuto, txtItems).second) == flatOf(kFiles, txtItems));
        EXPECT(Titles(menuOf(kAuto, oneTxt).second) == flatOf(kFiles, oneTxt));
        EXPECT(Titles(menuOf(kAuto, dirItems).second) == flatOf(kFiles, dirItems));
        EXPECT(Titles(menuOf(kAuto, zipItems).second) == flatOf(kArchive, zipItems));
        EXPECT(Titles(menuOf(kAuto, oneZip).second) == flatOf(kArchive, oneZip));
        EXPECT(Titles(menuOf(kAuto, mixedItems).second) == flatOf(kFiles, mixedItems));        // a mix of files and archives: the files' entries
        EXPECT(Titles(menuOf(kAuto, sumAndZip).second) == flatOf(kFiles, sumAndZip));          // not only checksum files: the files' entries
        EXPECT(!Titles(menuOf(kAuto, txtItems).second).empty() && !Titles(menuOf(kAuto, zipItems).second).empty());
        {   // the files' menu in full: the presets (by default smallest, backup, store only) and the other formats in place, each with the icon of its command
            auto subs = menuOf(kAuto, oneTxt).second;
            EXPECT(Titles(subs) == (std::vector<std::wstring>{L"加入壓縮檔…", L"壓縮成 \"a b.zip\"", L"", L"用範本壓縮：最小體積", L"用範本壓縮：備份",
                                                              L"用範本壓縮：僅儲存不壓縮", L"", L"其他格式：tar.gz", L"其他格式：tar.xz", L"其他格式：tar.bz2", L"",
                                                              L"計算雜湊值…"}));
            for (auto& e : subs) {
                EXPCMDFLAGS fl = ECF_DEFAULT;
                e.second->GetFlags(&fl);
                EXPECT((fl & ECF_HASSUBCOMMANDS) == 0);
                EXPECT(e.first.empty() == ((fl & ECF_ISSEPARATOR) != 0));
            }
            if (subs.size() == 12) {
                EXPECT(IconIs(subs[3].second, L",-117") && IconIs(subs[7].second, L",-104"));   // a preset: the preset icon; tar.gz: the TAR icon
                EXPECT(FAILED(subs[2].second->Invoke(oneTxt.Get(), nullptr)));                  // a line does nothing
                DeleteFileW(echoOut.c_str());
                EXPECT(SUCCEEDED(subs[4].second->Invoke(oneTxt.Get(), nullptr)));
                EXPECT(WaitForEcho(echoOut, 1));
                EXPECT(ReadEcho(echoOut).find(L"--compress zip --template \"Backup\" \"" + a.wstring() + L"\"") != std::wstring::npos);
            }
        }
        {   // the recent folders of the archives, and no line at the end
            writeSettings("language=zh-TW\nshell_items=here,recent\nrecent_extract_dirs=D:\\Out One\n");
            EXPECT(Titles(menuOf(kAuto, oneZip).second) == (std::vector<std::wstring>{L"解壓縮到此處", L"", L"解壓縮到最近用過的位置：D:\\Out One"}));
            writeSettings("language=en-US\nshell_items=template\nshell_presets=zip\n");   // only a sub menu: no line at all
            EXPECT(Titles(menuOf(kAuto, oneTxt).second) == (std::vector<std::wstring>{L"Compress with a template: General ZIP"}));
        }
        writeSettings("language=zh-TW\nshell_items=add,fmt1,fmt2,each,encrypt,opencontent,open,here,folder,to,test,images,audio,merge,hashsave\n");  // as before this block
        {   // shown for any selection, hidden for none; a flyout for files and archives
            auto c = create(kAuto);
            EXPECT(state(c, txtItems) == ECS_ENABLED);
            EXPCMDFLAGS fl = ECF_DEFAULT;
            c->GetFlags(&fl);
            EXPECT((fl & ECF_HASSUBCOMMANDS) != 0);
            PWSTR t = nullptr;
            EXPECT(SUCCEEDED(c->GetTitle(txtItems.Get(), &t)) && Str(t) == L"LycanZip");
            c = create(kAuto);
            EXPECT(state(c, zipItems) == ECS_ENABLED && IconIs(c, L",0"));
            c = create(kAuto);
            EXPCMDSTATE none = ECS_ENABLED;
            c->GetState(nullptr, FALSE, &none);
            EXPECT(none == ECS_HIDDEN);
        }
        {   // "Show more options" (the classic menu hands the selection over with SetSelection first): hidden there, the classic handler is the entry;
            // the new menu (GetState first) keeps it, also when it hands the selection over later
            ComPtr<IObjectWithSelection> ows;
            auto c = create(kAuto);
            EXPECT(c && SUCCEEDED(c.As(&ows)) && SUCCEEDED(ows->SetSelection(txtItems.Get())));
            EXPECT(state(c, txtItems) == ECS_HIDDEN);
            c = create(kAuto);
            ComPtr<IObjectWithSelection> ows2;
            EXPECT(state(c, txtItems) == ECS_ENABLED && SUCCEEDED(c.As(&ows2)) && SUCCEEDED(ows2->SetSelection(txtItems.Get())));
            EXPECT(state(c, txtItems) == ECS_ENABLED && !Subs(c).empty());
            c = create(kAuto);
            ComPtr<IObjectWithSelection> ows3;
            EXPECT(SUCCEEDED(c.As(&ows3)) && SUCCEEDED(ows3->SetSelection(sumItem.Get())) && state(c, sumItem) == ECS_HIDDEN);   // the checksum entry too
        }
        {   // Windows 10: no desktop9 handlers, this command is the classic menu itself: it stays when the selection comes first, and keeps its sub menus
            SetEnvironmentVariableW(L"LYCAN_TEST_WIN_BUILD", L"19045");
            writeSettings("language=zh-TW\nshell_items=add,template\n");
            ComPtr<IObjectWithSelection> ows;
            auto c = create(kAuto);
            EXPECT(c && SUCCEEDED(c.As(&ows)) && SUCCEEDED(ows->SetSelection(oneTxt.Get())));
            EXPECT(state(c, oneTxt) == ECS_ENABLED);
            const auto subs = Subs(c);
            EXPECT(Titles(subs) == (std::vector<std::wstring>{L"加入壓縮檔…", L"用範本壓縮"}));
            auto tmpl = Find(const_cast<std::vector<Entry>&>(subs), L"用範本壓縮");
            EXPECT(tmpl && Titles(Subs(tmpl)) == (std::vector<std::wstring>{L"最小體積", L"備份", L"僅儲存不壓縮"}));
            SetEnvironmentVariableW(L"LYCAN_TEST_WIN_BUILD", nullptr);
        }
        {   // shell_menu=false (the switch of the Store version): no entry anywhere, not even for checksum files; the classic handler adds nothing
            writeSettings("language=zh-TW\nshell_menu=false\n");
            auto c = create(kAuto);
            EXPECT(state(c, txtItems) == ECS_HIDDEN);
            c = create(kAuto);
            EXPECT(state(c, zipItems) == ECS_HIDDEN);
            c = create(kAuto);
            EXPECT(state(c, sumItem) == ECS_HIDDEN);
            writeSettings("language=zh-TW\nshell_menu=true\n");
            c = create(kAuto);
            EXPECT(state(c, txtItems) == ECS_ENABLED);
            writeSettings("language=zh-TW\nshell_items=add,fmt1,fmt2,each,encrypt,opencontent,open,here,folder,to,test,images,audio,merge,hashsave\n");  // as before this block
        }
        {   // checksum files: the command itself is "Verify the checksum file", no sub menu
            auto c = create(kAuto);
            EXPECT(state(c, sumItem) == ECS_ENABLED);
            EXPCMDFLAGS fl = ECF_DEFAULT;
            c->GetFlags(&fl);
            EXPECT((fl & ECF_HASSUBCOMMANDS) == 0);
            auto v = create(kVerifyClassic);
            PWSTR t = nullptr, t2 = nullptr;
            EXPECT(v && SUCCEEDED(v->GetTitle(nullptr, &t2)));
            EXPECT(SUCCEEDED(c->GetTitle(sumItem.Get(), &t)) && Str(t) == Str(t2) && IconIs(c, L",-116"));
            ComPtr<IEnumExplorerCommand> e;
            EXPECT(c->EnumSubCommands(&e) == E_NOTIMPL);
            DeleteFileW(echoOut.c_str());
            EXPECT(SUCCEEDED(c->Invoke(sumItem.Get(), nullptr)));
            EXPECT(WaitForEcho(echoOut, 1));
            EXPECT(ReadEcho(echoOut).find(L"--verify \"" + sum.wstring() + L"\"") != std::wstring::npos);
        }

        // ---- the classic menu of the package (IContextMenu): the same entries, as HMENUs ----
        auto dataOf = [&](const std::vector<fs::path>& paths) {
            std::vector<PIDLIST_ABSOLUTE> pidls;
            for (auto& p : paths) { PIDLIST_ABSOLUTE pidl = nullptr; SHParseDisplayName(p.c_str(), nullptr, &pidl, 0, nullptr); pidls.push_back(pidl); }
            std::vector<PCUITEMID_CHILD> kids;
            for (auto p : pidls) kids.push_back(ILFindLastID(p));
            PIDLIST_ABSOLUTE parent = ILClone(pidls[0]);
            ILRemoveLastID(parent);
            ComPtr<IDataObject> data;
            SHCreateDataObject(parent, static_cast<UINT>(kids.size()), kids.data(), nullptr, IID_PPV_ARGS(&data));
            CoTaskMemFree(parent);
            for (auto p : pidls) CoTaskMemFree(p);
            return data;
        };
        auto classicMenu = [&](const std::vector<fs::path>& paths, ComPtr<IContextMenu>& cm, HMENU& menu, UINT& count) {
            ComPtr<IClassFactory> f;
            ComPtr<IShellExtInit> init;
            if (!getClass || FAILED(getClass(kPackagedClassic, IID_PPV_ARGS(&f))) || FAILED(f->CreateInstance(nullptr, IID_PPV_ARGS(&init)))) return false;
            auto data = dataOf(paths);
            if (FAILED(init->Initialize(nullptr, data.Get(), nullptr)) || FAILED(init.As(&cm))) return false;
            menu = CreatePopupMenu();
            const HRESULT q = cm->QueryContextMenu(menu, 0, 1, 0x7FFF, CMF_NORMAL);
            count = HRESULT_CODE(q);
            return SUCCEEDED(q);
        };
        auto titlesOfMenu = [](HMENU m) {
            std::vector<std::wstring> v;
            for (int i = 0; i < GetMenuItemCount(m); ++i) {
                wchar_t t[256] = {};
                GetMenuStringW(m, i, t, 256, MF_BYPOSITION);
                v.push_back(t);
            }
            return v;
        };
        auto strip = [](std::vector<std::wstring> v) { for (auto& t : v) t.erase(std::remove(t.begin(), t.end(), L'&'), t.end()); return v; };   // (no names with '&' here: nothing to undo)
        for (auto& pr : std::vector<std::pair<std::vector<fs::path>, ComPtr<IShellItemArray>>>{{{a, b}, txtItems}, {{z, z2}, zipItems}, {{dir}, dirItems}, {{a, z}, mixedItems}}) {
            ComPtr<IContextMenu> cm;
            HMENU menu = nullptr;
            UINT count = 0;
            EXPECT(classicMenu(pr.first, cm, menu, count) && count > 1);
            EXPECT(menu && GetMenuItemCount(menu) == 1);
            if (menu && GetMenuItemCount(menu) == 1) {
                EXPECT(titlesOfMenu(menu) == (std::vector<std::wstring>{L"LycanZip"}));
                MENUITEMINFOW mi{sizeof(mi)};
                mi.fMask = MIIM_SUBMENU;
                GetMenuItemInfoW(menu, 0, TRUE, &mi);
                EXPECT(mi.hSubMenu != nullptr);
                const auto expected = Titles(menuOf(pr.second == zipItems ? kArchive : kFiles, pr.second).second);   // the classic menu keeps its sub menus
                if (mi.hSubMenu) EXPECT(titlesOfMenu(mi.hSubMenu) == expected);
                (void)strip;
            }
            if (menu) DestroyMenu(menu);
        }
        {   // choosing an entry starts LycanZip with the same command line (the first entry of the files: add)
            ComPtr<IContextMenu> cm;
            HMENU menu = nullptr;
            UINT count = 0;
            EXPECT(classicMenu({a, b}, cm, menu, count));
            if (cm) {
                DeleteFileW(echoOut.c_str());
                CMINVOKECOMMANDINFO ci{sizeof(ci)};
                ci.lpVerb = MAKEINTRESOURCEA(0);
                EXPECT(SUCCEEDED(cm->InvokeCommand(&ci)));
                EXPECT(WaitForEcho(echoOut, 1));
                EXPECT(ReadEcho(echoOut).find(L"--add \"" + a.wstring() + L"\" \"" + b.wstring() + L"\"") != std::wstring::npos);
                CMINVOKECOMMANDINFO bad{sizeof(bad)};
                bad.lpVerb = MAKEINTRESOURCEA(999);
                EXPECT(FAILED(cm->InvokeCommand(&bad)));
            }
            if (menu) DestroyMenu(menu);
        }
        {   // a checksum file: the entry itself (no sub menu), and it verifies
            ComPtr<IContextMenu> cm;
            HMENU menu = nullptr;
            UINT count = 0;
            EXPECT(classicMenu({sum}, cm, menu, count) && count == 1 && menu && GetMenuItemCount(menu) == 1);
            if (cm && menu) {
                auto v = create(kVerifyClassic);
                PWSTR t2 = nullptr;
                EXPECT(v && SUCCEEDED(v->GetTitle(nullptr, &t2)));
                EXPECT(titlesOfMenu(menu) == (std::vector<std::wstring>{Str(t2)}));
                DeleteFileW(echoOut.c_str());
                CMINVOKECOMMANDINFO ci{sizeof(ci)};
                ci.lpVerb = MAKEINTRESOURCEA(0);
                EXPECT(SUCCEEDED(cm->InvokeCommand(&ci)));
                EXPECT(WaitForEcho(echoOut, 1));
                EXPECT(ReadEcho(echoOut).find(L"--verify \"" + sum.wstring() + L"\"") != std::wstring::npos);
            }
            if (menu) DestroyMenu(menu);
        }
        {   // the switch off: the classic handler of the package adds nothing either
            writeSettings("language=zh-TW\nshell_menu=false\n");
            ComPtr<IContextMenu> cm;
            HMENU menu = nullptr;
            UINT count = 99;
            EXPECT(classicMenu({a, b}, cm, menu, count) && count == 0 && menu && GetMenuItemCount(menu) == 0);
            if (menu) DestroyMenu(menu);
            writeSettings("language=zh-TW\nshell_items=add,fmt1,fmt2,each,encrypt,opencontent,open,here,folder,to,test,images,audio,merge,hashsave\n");
        }
        {   // no menu for the default-verb query, and nothing without a selection
            ComPtr<IClassFactory> f;
            ComPtr<IShellExtInit> init;
            EXPECT(getClass && SUCCEEDED(getClass(kPackagedClassic, IID_PPV_ARGS(&f))) && SUCCEEDED(f->CreateInstance(nullptr, IID_PPV_ARGS(&init))));
            EXPECT(init && FAILED(init->Initialize(nullptr, nullptr, nullptr)));
            ComPtr<IContextMenu> cm;
            HMENU menu = CreatePopupMenu();
            ComPtr<IContextMenu> cm2;
            ComPtr<IClassFactory> f2;
            if (getClass && SUCCEEDED(getClass(kPackagedClassic, IID_PPV_ARGS(&f2)))) {
                ComPtr<IShellExtInit> i2;
                f2->CreateInstance(nullptr, IID_PPV_ARGS(&i2));
                auto data = dataOf({a});
                if (i2 && SUCCEEDED(i2->Initialize(nullptr, data.Get(), nullptr)) && SUCCEEDED(i2.As(&cm2)))
                    EXPECT(HRESULT_CODE(cm2->QueryContextMenu(menu, 0, 1, 0x7FFF, CMF_DEFAULTONLY)) == 0 && GetMenuItemCount(menu) == 0);
            }
            DestroyMenu(menu);
        }
    }

    // ---- odd input ----
    {
        auto [c, subs] = menuOf(kFiles, oneTxt);
        EXPECT(!subs.empty() && FAILED(subs[0].second->Invoke(nullptr, nullptr)));  // no selection
        ComPtr<IEnumExplorerCommand> e;
        EXPECT(!subs.empty() && subs[0].second->EnumSubCommands(&e) == E_NOTIMPL);  // a leaf has no sub menu
        // Reset / Clone / Skip of the enumerator
        ComPtr<IEnumExplorerCommand> en, en2;
        c->EnumSubCommands(&en);
        ComPtr<IExplorerCommand> one;
        ULONG got = 0;
        en->Next(1, &one, &got);
        EXPECT(SUCCEEDED(en->Clone(&en2)));
        en->Skip(1);
        one.Reset();
        EXPECT(en->Next(1, &one, &got) == S_OK);
        en2->Reset();
        EXPECT(en2->Next(1, &one, &got) == S_OK);  // the clone is independent
        // a menu that was never given a selection stays empty and hidden
        auto fresh = create(kFiles);
        EXPECT(Subs(fresh).empty());
        // the classic menu enumerates on an object that only got IObjectWithSelection::SetSelection (no GetState)
        ComPtr<IObjectWithSelection> sel;
        auto viaSel = create(kClassicFiles);
        EXPECT(viaSel && SUCCEEDED(viaSel.As(&sel)));
        if (sel) {
            EXPECT(SUCCEEDED(sel->SetSelection(oneTxt.Get())));
            EXPECT(Subs(viaSel).size() == 5);
        }
    }

    // The DLL can be unloaded once every object is released
    canUnload();  // (objects of the lambdas above are already gone)
    EXPECT(canUnload() == S_OK);
    FreeLibrary(dll);

    std::error_code ec;
    fs::remove_all(root, ec);
    if (failures == 0) std::puts("test_shellext: all passed");
    CoUninitialize();
    return failures ? 1 : 0;
}
