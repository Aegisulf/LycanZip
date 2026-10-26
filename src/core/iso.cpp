// ISO 9660 + Joliet writer. Layout of the image (2048-byte sectors):
//   0-15 system area (zeros) | 16 primary volume descriptor | 17 Joliet supplementary descriptor | 18 terminator
//   path tables (primary L/M, Joliet L/M) | directories of the primary tree | directories of the Joliet tree | file data
// Both trees point to the same file data. The primary tree has 8.3 style upper case names (for old readers); the Joliet tree has
// the real names. Directory records are sorted by identifier, as the standard asks.
#include "lycan/iso.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <memory>
#include <set>

#include "lycan/error.h"
#include "lycan/security.h"
#include "lycan/testhooks.h"

namespace lycan {
namespace {

constexpr uint32_t kSector = 2048;
constexpr uint64_t kMaxFile = 0xFFFFFFFFull;   // a directory record keeps the size in 32 bits (one extent)

struct Node {
    std::wstring name;
    bool isDir = false;
    uint64_t size = 0, mtime = 0;
    std::filesystem::path source;
    Node* parent = nullptr;
    std::vector<std::unique_ptr<Node>> kids;
    std::string id[2];   // [0] primary identifier ("NAME.EXT;1" / "DIR"), [1] Joliet identifier (UCS-2 big endian bytes)
    struct Lay { uint32_t extent = 0, dirSize = 0, dirNo = 0; } lay[2];
    std::vector<Node*> sorted[2];   // the kids in the order of each tree
    uint32_t fileExtent = 0;
};

std::wstring LowerW(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

uint32_t RoundUp(uint64_t v) { return static_cast<uint32_t>((v + kSector - 1) / kSector); }   // sectors

// ---- identifiers --------------------------------------------------------------------------------------------------

char DChar(wchar_t c) {
    if (c >= L'a' && c <= L'z') return static_cast<char>(c - L'a' + L'A');
    if ((c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'_') return static_cast<char>(c);
    return '_';
}

std::string PrimaryId(const std::wstring& name, bool isDir, std::set<std::string>& used) {
    std::wstring base = name, ext;
    if (!isDir) {
        const size_t dot = name.rfind(L'.');
        if (dot != std::wstring::npos && dot > 0) { base = name.substr(0, dot); ext = name.substr(dot + 1); }
    }
    std::string b, e;
    for (wchar_t c : base) if (b.size() < 8) b += DChar(c);
    for (wchar_t c : ext) if (e.size() < 3) e += DChar(c);
    if (b.empty()) b = "_";
    auto make = [&](const std::string& bb) { return isDir ? bb : (bb + (e.empty() ? std::string(".") : "." + e) + ";1"); };
    std::string id = make(b);
    for (int n = 1; used.count(id) && n < 10000000; ++n) {
        const std::string suf = "~" + std::to_string(n);
        id = make(b.substr(0, 8 > suf.size() ? 8 - suf.size() : 0) + suf);
    }
    used.insert(id);
    return id;
}

std::string JolietId(const std::wstring& name, std::set<std::wstring>& used) {
    std::wstring s;
    for (wchar_t c : name) s += (c < 0x20 || c == L'*' || c == L'/' || c == L':' || c == L';' || c == L'?' || c == L'\\') ? L'_' : c;
    auto fit = [&](const std::wstring& full, const std::wstring& suffixBeforeExt) {
        size_t dot = full.rfind(L'.');
        std::wstring base = full, ext;
        if (dot != std::wstring::npos && dot > 0 && full.size() - dot <= 10) { base = full.substr(0, dot); ext = full.substr(dot); }
        const size_t room = 64 - ext.size() - suffixBeforeExt.size();
        if (base.size() > room) base.resize(room);
        return base + suffixBeforeExt + ext;
    };
    std::wstring id = fit(s, L"");
    for (int n = 1; used.count(LowerW(id)); ++n) id = fit(s, L"~" + std::to_wstring(n));
    used.insert(LowerW(id));
    std::string bytes;
    for (wchar_t c : id) { bytes += static_cast<char>(c >> 8); bytes += static_cast<char>(c & 0xFF); }
    return bytes;
}

// ---- little helpers for the binary records ------------------------------------------------------------------------

void Le32(std::vector<uint8_t>& b, size_t o, uint32_t v) { for (int i = 0; i < 4; ++i) b[o + i] = static_cast<uint8_t>(v >> (8 * i)); }
void Be32(std::vector<uint8_t>& b, size_t o, uint32_t v) { for (int i = 0; i < 4; ++i) b[o + i] = static_cast<uint8_t>(v >> (24 - 8 * i)); }
void Le16(std::vector<uint8_t>& b, size_t o, uint32_t v) { b[o] = static_cast<uint8_t>(v); b[o + 1] = static_cast<uint8_t>(v >> 8); }
void Be16(std::vector<uint8_t>& b, size_t o, uint32_t v) { b[o] = static_cast<uint8_t>(v >> 8); b[o + 1] = static_cast<uint8_t>(v); }
void Both32(std::vector<uint8_t>& b, size_t o, uint32_t v) { Le32(b, o, v); Be32(b, o + 4, v); }
void Both16(std::vector<uint8_t>& b, size_t o, uint32_t v) { Le16(b, o, v); Be16(b, o + 2, v); }

void RecordDate(uint8_t* p, uint64_t ft) {
    FILETIME f;
    f.dwLowDateTime = static_cast<DWORD>(ft);
    f.dwHighDateTime = static_cast<DWORD>(ft >> 32);
    SYSTEMTIME st{};
    if (!FileTimeToSystemTime(&f, &st) || st.wYear < 1900 || st.wYear > 2155) { GetSystemTime(&st); }
    p[0] = static_cast<uint8_t>(st.wYear - 1900);
    p[1] = static_cast<uint8_t>(st.wMonth);
    p[2] = static_cast<uint8_t>(st.wDay);
    p[3] = static_cast<uint8_t>(st.wHour);
    p[4] = static_cast<uint8_t>(st.wMinute);
    p[5] = static_cast<uint8_t>(st.wSecond);
    p[6] = 0;   // UTC
}

void VolumeDate(uint8_t* p, const SYSTEMTIME& st) {   // 17 bytes: YYYYMMDDHHMMSS + hundredths + offset
    char t[20];
    sprintf_s(t, "%04u%02u%02u%02u%02u%02u00", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    memcpy(p, t, 16);
    p[16] = 0;
}

void NoDate(uint8_t* p) { memset(p, '0', 16); p[16] = 0; }

// One directory record (ECMA-119 9.1); never crosses a sector border
void AddRecord(std::vector<uint8_t>& dir, uint32_t extent, uint32_t size, uint64_t mtime, bool isDir, const std::string& id) {
    const size_t len = 33 + id.size() + (id.size() % 2 == 0 ? 1 : 0);
    if (dir.size() % kSector + len > kSector) dir.resize((dir.size() / kSector + 1) * kSector, 0);
    const size_t o = dir.size();
    dir.resize(o + len, 0);
    dir[o] = static_cast<uint8_t>(len);
    Both32(dir, o + 2, extent);
    Both32(dir, o + 10, size);
    RecordDate(&dir[o + 18], mtime);
    dir[o + 25] = isDir ? 2 : 0;
    Both16(dir, o + 28, 1);
    dir[o + 32] = static_cast<uint8_t>(id.size());
    memcpy(&dir[o + 33], id.data(), id.size());
}

size_t RecordLen(size_t idSize) { return 33 + idSize + (idSize % 2 == 0 ? 1 : 0); }

class Out {
public:
    Out(HANDLE h, const ProgressFn& progress) : h_(h), progress_(progress) {}
    void Write(const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        if (TestWriteShouldFail(n)) throw Error(HRESULT_FROM_WIN32(ERROR_DISK_FULL), Message::Make(L"core.disk_full"));
        while (n) {
            const DWORD chunk = static_cast<DWORD>((std::min<size_t>)(n, 1u << 20));
            DWORD w = 0;
            if (!WriteFile(h_, b, chunk, &w, nullptr) || w == 0) throw Error(HRESULT_FROM_WIN32(GetLastError()), Message::Make(L"core.cannot_write_output"));
            b += w;
            n -= w;
            pos_ += w;
        }
    }
    void Zeros(uint64_t n) {
        static const std::vector<uint8_t> z(1u << 16, 0);
        while (n) { const size_t c = static_cast<size_t>((std::min<uint64_t>)(n, z.size())); Write(z.data(), c); n -= c; }
    }
    void PadToSector() { if (pos_ % kSector) Zeros(kSector - pos_ % kSector); }
    uint64_t pos() const { return pos_; }
private:
    HANDLE h_;
    const ProgressFn& progress_;
    uint64_t pos_ = 0;
};

void Descriptor(std::vector<uint8_t>& d, uint8_t type, bool joliet, const std::string& labelBytes, uint32_t blocks,
                uint32_t pathSize, uint32_t lPath, uint32_t mPath, const std::vector<uint8_t>& root, const SYSTEMTIME& now) {
    d.assign(kSector, 0);
    d[0] = type;
    memcpy(&d[1], "CD001", 5);
    d[6] = 1;
    // fields of text: spaces (UCS-2 spaces in the Joliet descriptor)
    auto fill = [&](size_t from, size_t to) {
        for (size_t i = from; i < to; ++i) d[i] = joliet ? ((i - from) % 2 == 0 ? 0 : ' ') : ' ';
    };
    fill(8, 40);                // system id
    fill(40, 72);               // volume id, replaced below
    memcpy(&d[40], labelBytes.data(), (std::min<size_t>)(labelBytes.size(), 32));
    Both32(d, 80, blocks);
    if (joliet) { d[88] = 0x25; d[89] = 0x2F; d[90] = 0x45; }   // escape sequence: UCS-2 level 3
    Both16(d, 120, 1);
    Both16(d, 124, 1);
    Both16(d, 128, kSector);
    Both32(d, 132, pathSize);
    Le32(d, 140, lPath);
    Be32(d, 148, mPath);
    memcpy(&d[156], root.data(), 34);
    fill(190, 190 + 128);       // volume set id
    fill(318, 318 + 128);       // publisher
    fill(446, 446 + 128);       // data preparer
    fill(574, 574 + 128);       // application
    fill(702, 702 + 37);        // copyright / abstract / bibliographic files
    fill(739, 739 + 37);
    fill(776, 776 + 37);
    VolumeDate(&d[813], now);
    VolumeDate(&d[830], now);
    NoDate(&d[847]);
    VolumeDate(&d[864], now);
    d[881] = 1;
}

}  // namespace

IsoResult WriteIso(void* outHandle, const std::vector<IsoItem>& items, const IsoOptions& options) {
    IsoResult result;
    SYSTEMTIME now;
    GetSystemTime(&now);
    FILETIME nowFt;
    SystemTimeToFileTime(&now, &nowFt);
    const uint64_t nowTime = (static_cast<uint64_t>(nowFt.dwHighDateTime) << 32) | nowFt.dwLowDateTime;

    // ---- the tree ----
    Node root;
    root.isDir = true;
    root.mtime = nowTime;
    for (const IsoItem& it : items) {
        Node* cur = &root;
        std::vector<std::wstring> parts;
        size_t from = 0;
        for (size_t i = 0; i <= it.path.size(); ++i)
            if (i == it.path.size() || it.path[i] == L'\\' || it.path[i] == L'/') {
                if (i > from) parts.push_back(it.path.substr(from, i - from));
                from = i + 1;
            }
        for (size_t i = 0; i < parts.size(); ++i) {
            const bool last = i + 1 == parts.size();
            Node* next = nullptr;
            for (auto& k : cur->kids) if (LowerW(k->name) == LowerW(parts[i])) { next = k.get(); break; }
            if (!next) {
                cur->kids.push_back(std::make_unique<Node>());
                next = cur->kids.back().get();
                next->name = parts[i];
                next->parent = cur;
                next->isDir = !last || it.isDir;
                next->mtime = nowTime;
            }
            cur = next;
            if (last) {
                cur->mtime = options.keepTimes && it.mtime ? it.mtime : nowTime;
                if (!it.isDir) { cur->size = it.size; cur->source = it.source; cur->isDir = false; }
            }
        }
    }
    if (root.kids.empty()) throw Error(E_INVALIDARG, Message::Make(L"core.iso_nothing"));

    // ---- identifiers and the order of the kids ----
    std::vector<Node*> dirs;   // every folder, in the order of discovery
    std::vector<Node*> files;
    {
        std::vector<Node*> stack{&root};
        while (!stack.empty()) {
            Node* d = stack.back();
            stack.pop_back();
            dirs.push_back(d);
            std::set<std::string> usedP;
            std::set<std::wstring> usedJ;
            for (auto& k : d->kids) {
                k->id[0] = PrimaryId(k->name, k->isDir, usedP);
                k->id[1] = JolietId(k->name, usedJ);
                if (k->isDir) stack.push_back(k.get());
                else {
                    if (k->size > kMaxFile) throw Error(E_INVALIDARG, Message::Make(L"core.iso_file_too_big", k->name));
                    files.push_back(k.get());
                }
            }
            for (int t = 0; t < 2; ++t) {
                for (auto& k : d->kids) d->sorted[t].push_back(k.get());
                std::sort(d->sorted[t].begin(), d->sorted[t].end(), [t](const Node* a, const Node* b) { return a->id[t] < b->id[t]; });
            }
        }
    }
    root.id[0] = std::string(1, '\0');
    root.id[1] = std::string(1, '\0');

    // ---- sizes and numbering of the folders (breadth first, per tree) ----
    std::vector<Node*> order[2];
    uint32_t pathSize[2] = {0, 0};
    for (int t = 0; t < 2; ++t) {
        order[t].push_back(&root);
        for (size_t i = 0; i < order[t].size(); ++i) {
            Node* d = order[t][i];
            d->lay[t].dirNo = static_cast<uint32_t>(i + 1);
            for (Node* k : d->sorted[t]) if (k->isDir) order[t].push_back(k);
            size_t cur = 0;
            auto add = [&](size_t idSize) {
                const size_t len = RecordLen(idSize);
                if (cur % kSector + len > kSector) cur = (cur / kSector + 1) * kSector;
                cur += len;
            };
            add(1);
            add(1);
            for (Node* k : d->sorted[t]) add(k->id[t].size());
            d->lay[t].dirSize = RoundUp(cur) * kSector;
            const size_t idLen = d == &root ? 1 : d->id[t].size();
            pathSize[t] += static_cast<uint32_t>(8 + idLen + (idLen % 2));
        }
    }

    // ---- sector numbers ----
    uint32_t cur = 19;
    uint32_t lPath[2], mPath[2];
    for (int t = 0; t < 2; ++t) {
        lPath[t] = cur; cur += RoundUp(pathSize[t]);
        mPath[t] = cur; cur += RoundUp(pathSize[t]);
    }
    for (int t = 0; t < 2; ++t)
        for (Node* d : order[t]) { d->lay[t].extent = cur; cur += d->lay[t].dirSize / kSector; }
    std::sort(files.begin(), files.end(), [](const Node* a, const Node* b) { return a->id[0] < b->id[0] || (a->id[0] == b->id[0] && a->name < b->name); });
    uint64_t totalFileBytes = 0;
    for (Node* f : files) {
        f->fileExtent = cur;
        cur += RoundUp(f->size);
        totalFileBytes += f->size;
    }
    const uint32_t blocks = cur;

    // ---- volume label ----
    std::wstring label = options.label;
    std::string primaryLabel;
    for (wchar_t c : label) if (primaryLabel.size() < 32) primaryLabel += (c == L' ') ? '_' : DChar(c);
    if (primaryLabel.empty()) primaryLabel = "CDROM";
    std::string jolietLabel;
    {
        std::wstring jl;
        for (wchar_t c : label) if (jl.size() < 16) jl += (c < 0x20 || c == L'*' || c == L'/' || c == L':' || c == L';' || c == L'?' || c == L'\\') ? L'_' : c;
        if (jl.empty()) jl = L"CDROM";
        for (wchar_t c : jl) { jolietLabel += static_cast<char>(c >> 8); jolietLabel += static_cast<char>(c & 0xFF); }
    }

    // ---- write ----
    Out out(static_cast<HANDLE>(outHandle), options.progress);
    out.Zeros(16 * kSector);
    for (int t = 0; t < 2; ++t) {
        std::vector<uint8_t> rootRec;
        AddRecord(rootRec, root.lay[t].extent, root.lay[t].dirSize, nowTime, true, std::string(1, '\0'));
        std::vector<uint8_t> d;
        Descriptor(d, static_cast<uint8_t>(t == 0 ? 1 : 2), t == 1, t == 0 ? primaryLabel : jolietLabel, blocks, pathSize[t], lPath[t], mPath[t], rootRec, now);
        out.Write(d.data(), d.size());
    }
    {
        std::vector<uint8_t> term(kSector, 0);
        term[0] = 255;
        memcpy(&term[1], "CD001", 5);
        term[6] = 1;
        out.Write(term.data(), term.size());
    }
    for (int t = 0; t < 2; ++t) {
        for (int big = 0; big < 2; ++big) {
            std::vector<uint8_t> pt;
            for (Node* d : order[t]) {
                const std::string id = d == &root ? std::string(1, '\0') : d->id[t];
                const size_t o = pt.size();
                pt.resize(o + 8 + id.size() + (id.size() % 2), 0);
                pt[o] = static_cast<uint8_t>(id.size());
                const uint32_t parentNo = d == &root ? 1 : d->parent->lay[t].dirNo;
                if (big) { Be32(pt, o + 2, d->lay[t].extent); Be16(pt, o + 6, parentNo); }
                else { Le32(pt, o + 2, d->lay[t].extent); Le16(pt, o + 6, parentNo); }
                memcpy(&pt[o + 8], id.data(), id.size());
            }
            out.Write(pt.data(), pt.size());
            out.PadToSector();
        }
    }
    for (int t = 0; t < 2; ++t)
        for (Node* d : order[t]) {
            std::vector<uint8_t> dir;
            const Node* par = d == &root ? &root : d->parent;
            AddRecord(dir, d->lay[t].extent, d->lay[t].dirSize, d->mtime, true, std::string(1, '\0'));
            AddRecord(dir, par->lay[t].extent, par->lay[t].dirSize, par->mtime, true, std::string(1, '\1'));
            for (Node* k : d->sorted[t]) {
                if (k->isDir) AddRecord(dir, k->lay[t].extent, k->lay[t].dirSize, k->mtime, true, k->id[t]);
                else AddRecord(dir, k->fileExtent, static_cast<uint32_t>(k->size), k->mtime, false, k->id[t]);
            }
            dir.resize(d->lay[t].dirSize, 0);
            out.Write(dir.data(), dir.size());
        }

    uint64_t done = 0;
    std::vector<uint8_t> buf(1u << 20);
    for (Node* f : files) {
        uint64_t left = f->size;
        HANDLE h = INVALID_HANDLE_VALUE;
        if (f->size) {
            h = CreateFileW(ToLongPath(f->source).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                            FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (h == INVALID_HANDLE_VALUE) result.errors.push_back(Message::Make(L"core.iso_cannot_read", f->source.wstring()));
        }
        bool reportedShort = false;
        while (left) {
            const DWORD want = static_cast<DWORD>((std::min<uint64_t>)(left, buf.size()));
            DWORD got = 0;
            if (h != INVALID_HANDLE_VALUE && !ReadFile(h, buf.data(), want, &got, nullptr)) got = 0;
            if (got == 0) {   // unreadable, or the file became shorter: zeros keep the layout
                if (h != INVALID_HANDLE_VALUE && !reportedShort) result.errors.push_back(Message::Make(L"core.iso_short_read", f->source.wstring()));
                reportedShort = true;
                out.Zeros(left);
                done += left;
                left = 0;
            } else {
                out.Write(buf.data(), got);
                left -= got;
                done += got;
            }
            if (options.progress && !options.progress(done, totalFileBytes)) {
                if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
                throw Error(E_ABORT, Message::Make(L"core.cancelled"));
            }
        }
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        out.PadToSector();
    }
    result.bytes = out.pos();
    return result;
}

}  // namespace lycan
