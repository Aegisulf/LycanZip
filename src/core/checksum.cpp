#include "lycan/checksum.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

#include "lycan/error.h"

namespace lycan {
namespace fs = std::filesystem;
namespace {

constexpr uint64_t kMaxSumFile = 256ull << 20;

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// A line as UTF-8: it is when it already is, otherwise it was written in the system's ANSI code page
std::string LineAsUtf8(const std::string& line) {
    if (line.empty() || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, line.data(), static_cast<int>(line.size()), nullptr, 0) > 0) return line;
    const int n = MultiByteToWideChar(CP_ACP, 0, line.data(), static_cast<int>(line.size()), nullptr, 0);
    if (n <= 0) return line;
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_ACP, 0, line.data(), static_cast<int>(line.size()), w.data(), n);
    return WideToUtf8(w);
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

bool IsHex(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; });
}

// ".sfv": "name 1A2B3C4D" (the CRC is the last word; the name may have spaces); lines that start with ';' are comments
bool ParseSfvLine(const std::string& line, SumEntry& out) {
    std::string l = line;
    while (!l.empty() && (l.back() == ' ' || l.back() == '\t')) l.pop_back();
    const size_t sp = l.find_last_of(" \t");
    if (sp == std::string::npos || sp == 0) return false;
    const std::string crc = l.substr(sp + 1);
    if (crc.size() != 8 || !IsHex(crc)) return false;
    std::string name = l.substr(0, sp);
    while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.pop_back();
    if (name.empty()) return false;
    out.name = Utf8ToWide(name);
    out.hex = crc;
    for (auto& c : out.hex) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    out.algo = kHashCrc32;
    return true;
}

std::wstring Normalize(const std::wstring& name) {
    std::wstring s = name;
    std::replace(s.begin(), s.end(), L'\\', L'/');
    size_t start = 0;
    for (;;) {
        if (s.compare(start, 2, L"./") == 0) start += 2;
        else if (start < s.size() && s[start] == L'/') ++start;
        else break;
    }
    return Lower(s.substr(start));
}

// Without the first `count` folders ("" when there is not that much path)
std::wstring DropSegments(const std::wstring& s, int count) {
    size_t pos = 0;
    for (int i = 0; i < count; ++i) {
        const size_t slash = s.find(L'/', pos);
        if (slash == std::wstring::npos) return {};
        pos = slash + 1;
    }
    return s.substr(pos);
}

std::wstring RelativeName(const fs::path& file, const fs::path& base) {
    std::wstring name = file.wstring();
    if (!base.empty()) {
        const fs::path rel = file.lexically_relative(base);
        if (!rel.empty() && rel.wstring().rfind(L"..", 0) != 0) name = rel.wstring();
    }
    std::replace(name.begin(), name.end(), L'\\', L'/');
    return name;
}

size_t AlgoSlot(uint32_t mask, uint32_t algo) {
    const auto list = HashAlgoList(mask);
    for (size_t i = 0; i < list.size(); ++i) if (list[i] == algo) return i;
    return 0;
}

void WriteUtf8File(const fs::path& out, const std::string& text) {
    const fs::path tmp = out.wstring() + L".lycan-tmp";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        o.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!o.good()) {
            o.close();
            std::error_code ec;
            fs::remove(tmp, ec);
            throw Error(static_cast<long>(0x80070005L), "cannot write the checksum file");
        }
    }
    std::error_code ec;
    fs::remove(out, ec);
    fs::rename(tmp, out, ec);
    if (ec) {
        std::error_code ec2;
        fs::remove(tmp, ec2);
        throw Error(static_cast<long>(0x80070005L), "cannot write the checksum file");
    }
}

// What a check has to hash: one entry of the checksum file → a position in a list of files
struct Pending {
    size_t sumIndex;
    size_t slot;      // position in the list that is hashed
};

void Compare(const SumFile& sums, const std::vector<Pending>& pending, const HashReport& rep, SumVerifyReport& out) {
    for (const auto& p : pending) {
        const SumEntry& e = sums.entries[p.sumIndex];
        const HashFileResult& r = rep.files[p.slot];
        SumProblem prob;
        prob.name = e.name;
        if (!r.ok()) {
            prob.status = SumStatus::Error;
            prob.error = r.error;
            ++out.errors;
            out.problems.push_back(std::move(prob));
            continue;
        }
        const std::string& actual = r.hex[AlgoSlot(rep.algos, e.algo)];
        if (actual == e.hex) { ++out.matched; continue; }
        prob.status = SumStatus::Mismatch;
        prob.expected = e.hex;
        prob.actual = actual;
        ++out.mismatched;
        out.problems.push_back(std::move(prob));
    }
}

uint32_t UnionAlgos(const SumFile& sums, const std::vector<Pending>& pending) {
    uint32_t mask = 0;
    for (const auto& p : pending) mask |= sums.entries[p.sumIndex].algo;
    return mask ? mask : kHashSha256;
}

}  // namespace

uint32_t SumAlgoForPath(const fs::path& file) {
    const std::wstring ext = Lower(file.extension().wstring());
    if (ext == L".sha256") return kHashSha256;
    if (ext == L".sha1") return kHashSha1;
    if (ext == L".sha384") return kHashSha384;
    if (ext == L".sha512") return kHashSha512;
    if (ext == L".md5") return kHashMd5;
    if (ext == L".sfv") return kHashCrc32;
    const std::wstring name = Lower(file.filename().wstring());
    if (name == L"sha256sums" || name == L"sha256sum") return kHashSha256;
    if (name == L"sha1sums" || name == L"sha1sum") return kHashSha1;
    if (name == L"sha512sums" || name == L"sha512sum") return kHashSha512;
    if (name == L"md5sums" || name == L"md5sum") return kHashMd5;
    return 0;
}

const wchar_t* SumExtensionFor(uint32_t algo) {
    switch (algo) {
        case kHashCrc32: return L".sfv";
        case kHashMd5: return L".md5";
        case kHashSha1: return L".sha1";
        case kHashSha384: return L".sha384";
        case kHashSha512: return L".sha512";
        default: return L".sha256";
    }
}

SumFile ReadSumFile(const fs::path& file) {
    SumFile sf;
    sf.path = file;
    std::string data;
    {
        std::ifstream in(file, std::ios::binary);
        if (!in) { sf.error = SumFileError::Unreadable; return sf; }
        in.seekg(0, std::ios::end);
        const auto size = static_cast<uint64_t>(in.tellg());
        if (size > kMaxSumFile) { sf.error = SumFileError::TooBig; return sf; }
        in.seekg(0);
        data.resize(static_cast<size_t>(size));
        in.read(data.data(), static_cast<std::streamsize>(size));
        if (!in && !in.eof()) { sf.error = SumFileError::Unreadable; return sf; }
    }
    if (data.size() >= 3 && data.compare(0, 3, "\xEF\xBB\xBF") == 0) data.erase(0, 3);
    const bool sfv = Lower(file.extension().wstring()) == L".sfv";
    size_t pos = 0;
    while (pos < data.size()) {
        size_t end = data.find('\n', pos);
        if (end == std::string::npos) end = data.size();
        std::string line = data.substr(pos, end - pos);
        pos = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        line = LineAsUtf8(line);
        SumEntry e;
        if (sfv && ParseSfvLine(line, e)) { sf.entries.push_back(std::move(e)); continue; }
        SumLine sl;
        if (ParseSumLine(line, sl)) {
            e.name = sl.name;
            e.hex = sl.hex;
            e.algo = sl.algo ? sl.algo : (HashAlgosForLength(sl.hex.size()) & (~HashAlgosForLength(sl.hex.size()) + 1));  // the lowest bit
            if (e.algo) { sf.entries.push_back(std::move(e)); continue; }
        }
        if (!sfv && ParseSfvLine(line, e)) { sf.entries.push_back(std::move(e)); continue; }  // an .sfv style line in a file with another name
        ++sf.badLines;
    }
    if (sf.entries.empty()) sf.error = SumFileError::NoLines;
    return sf;
}

SumVerifyReport VerifySumFile(const SumFile& sums, const SumVerifyOptions& options) {
    SumVerifyReport rep;
    rep.sumPath = sums.path;
    rep.error = sums.error;
    if (rep.error != SumFileError::None) return rep;
    rep.total = static_cast<uint32_t>(sums.entries.size());
    const fs::path dir = sums.path.parent_path();
    std::vector<fs::path> files;
    std::vector<Pending> pending;
    for (size_t i = 0; i < sums.entries.size(); ++i) {
        std::wstring rel = sums.entries[i].name;
        std::replace(rel.begin(), rel.end(), L'/', L'\\');
        const fs::path file = dir / rel;
        std::error_code ec;
        if (!fs::exists(file, ec)) {
            SumProblem p;
            p.name = sums.entries[i].name;
            p.status = SumStatus::Missing;
            rep.problems.push_back(std::move(p));
            ++rep.missing;
            continue;
        }
        pending.push_back({i, files.size()});
        files.push_back(file);
    }
    if (!files.empty()) {
        HashOptions ho;
        ho.algos = UnionAlgos(sums, pending);
        ho.progress = options.progress;
        const HashReport hr = HashFiles(files, ho);
        rep.bytes = hr.bytes;
        Compare(sums, pending, hr, rep);
    }
    return rep;
}

ArchiveSumMatch MatchSumToArchive(const SumFile& sums, const std::vector<Entry>& entries, const ArchiveSumOptions& options) {
    ArchiveSumMatch m;
    m.entryOf.assign(sums.entries.size(), -1);
    std::vector<std::wstring> entryKey(entries.size());
    for (size_t i = 0; i < entries.size(); ++i) entryKey[i] = Normalize(entries[i].path);

    auto attempt = [&](int sumSkip, int entrySkip, std::vector<int64_t>& result) {
        std::unordered_map<std::wstring, int64_t> byKey;
        for (size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].isDir) continue;
            const std::wstring k = entrySkip ? DropSegments(entryKey[i], entrySkip) : entryKey[i];
            if (!k.empty()) byKey.emplace(k, static_cast<int64_t>(entries[i].index));
        }
        result.assign(sums.entries.size(), -1);
        uint32_t found = 0;
        for (size_t i = 0; i < sums.entries.size(); ++i) {
            std::wstring k = Normalize(sums.entries[i].name);
            if (options.manual) {
                const std::wstring strip = Normalize(options.stripPrefix);
                if (!strip.empty() && k.rfind(strip, 0) == 0) k.erase(0, strip.size());
                while (!k.empty() && k[0] == L'/') k.erase(0, 1);
                k = Normalize(options.addPrefix) + k;
            } else if (sumSkip) {
                k = DropSegments(k, sumSkip);
            }
            if (k.empty()) continue;
            const auto it = byKey.find(k);
            if (it != byKey.end()) { result[i] = it->second; ++found; }
        }
        return found;
    };

    if (options.manual) {
        m.found = attempt(0, 0, m.entryOf);
    } else {
        static const int kTries[][2] = {{0, 0}, {1, 0}, {0, 1}, {2, 0}, {0, 2}, {1, 1}};
        std::vector<int64_t> cur;
        uint32_t best = 0;
        for (const auto& t : kTries) {
            const uint32_t f = attempt(t[0], t[1], cur);
            if (f > best) { best = f; m.entryOf = cur; m.sumSkip = t[0]; m.entrySkip = t[1]; }
            if (f == sums.entries.size()) break;   // everything found: no need to look further
        }
        m.found = best;
    }
    m.confident = m.found > 0 && static_cast<size_t>(m.found) * 2 >= sums.entries.size();
    return m;
}

SumVerifyReport VerifySumAgainstArchive(const SumFile& sums, ArchiveSource& archive, const SumVerifyOptions& options, const ArchiveSumOptions& matchOptions) {
    SumVerifyReport rep;
    rep.sumPath = sums.path;
    rep.error = sums.error;
    if (rep.error != SumFileError::None) return rep;
    rep.total = static_cast<uint32_t>(sums.entries.size());
    const auto& entries = archive.Entries();
    const ArchiveSumMatch match = MatchSumToArchive(sums, entries, matchOptions);
    rep.sumSkip = match.sumSkip;
    rep.entrySkip = match.entrySkip;
    rep.confident = match.confident;

    std::vector<uint32_t> indices;
    std::vector<Pending> pending;
    std::unordered_map<int64_t, size_t> slotOfEntry;   // an entry listed twice is hashed once
    std::unordered_set<int64_t> used;
    for (size_t i = 0; i < sums.entries.size(); ++i) {
        const int64_t idx = match.entryOf[i];
        if (idx < 0 || static_cast<size_t>(idx) >= entries.size() || entries[static_cast<size_t>(idx)].isDir) {
            SumProblem p;
            p.name = sums.entries[i].name;
            p.status = SumStatus::Missing;
            rep.problems.push_back(std::move(p));
            ++rep.missing;
            continue;
        }
        used.insert(idx);
        auto it = slotOfEntry.find(idx);
        if (it == slotOfEntry.end()) {
            it = slotOfEntry.emplace(idx, indices.size()).first;
            indices.push_back(static_cast<uint32_t>(idx));
        }
        pending.push_back({i, it->second});
    }
    if (!indices.empty()) {
        HashOptions ho;
        ho.algos = UnionAlgos(sums, pending);
        ho.progress = options.progress;
        ho.password = options.password;
        const HashReport hr = HashEntries(archive, indices, ho);   // one result per entry, in the order of `indices` (no folders in it)
        rep.bytes = hr.bytes;
        Compare(sums, pending, hr, rep);
    }
    const std::wstring sumName = Lower(sums.path.filename().wstring());
    for (const auto& e : entries) {
        if (e.isDir || used.count(static_cast<int64_t>(e.index))) continue;
        const size_t slash = e.path.find_last_of(L'/');
        if (Lower(slash == std::wstring::npos ? e.path : e.path.substr(slash + 1)) == sumName) continue;   // the checksum file itself, when it is inside
        rep.extra.push_back(e.path);
    }
    return rep;
}

ArchiveSidecars FindArchiveSidecars(const fs::path& archive) {
    static const wchar_t* const kExts[] = {L".sha256", L".sha512", L".sha384", L".sha1", L".md5", L".sfv"};
    ArchiveSidecars s;
    std::error_code ec;
    const std::wstring stem = ArchiveBaseName(archive);
    for (const wchar_t* ext : kExts) {
        if (s.archiveSum.empty()) {
            const fs::path p = archive.wstring() + ext;
            if (fs::is_regular_file(p, ec)) s.archiveSum = p;
        }
        if (s.contentsSum.empty() && !stem.empty()) {
            const fs::path p = archive.parent_path() / (stem + ext);
            if (fs::is_regular_file(p, ec)) s.contentsSum = p;
        }
    }
    return s;
}

std::vector<fs::path> ExpandForHash(const std::vector<fs::path>& paths) {
    std::vector<fs::path> out;
    for (const auto& p : paths) {
        std::error_code ec;
        if (!fs::is_directory(p, ec)) { out.push_back(p); continue; }
        std::vector<fs::path> found;
        fs::recursive_directory_iterator it(p, fs::directory_options::skip_permission_denied, ec), end;
        for (; !ec && it != end; it.increment(ec)) {
            std::error_code e2;
            const auto st = it->symlink_status(e2);
            if (!e2 && st.type() == fs::file_type::regular) found.push_back(it->path());
        }
        std::sort(found.begin(), found.end());
        out.insert(out.end(), found.begin(), found.end());
    }
    return out;
}

namespace {

// One line per file that has a value: "<hex> *<name>" (".sfv": "<name> <crc>")
std::string SumLines(const HashReport& rep, const std::vector<fs::path>& bases, uint32_t algo) {
    std::string text;
    const size_t slot = AlgoSlot(rep.algos, algo);
    for (size_t i = 0; i < rep.files.size(); ++i) {
        const auto& f = rep.files[i];
        if (!f.ok()) continue;
        const std::wstring name = RelativeName(f.path, bases[i]);
        if (algo == kHashCrc32) text += WideToUtf8(name) + " " + f.hex[slot] + "\n";
        else text += FormatSumLine(f.hex[slot], name) + "\n";
    }
    return text;
}

// Hashes `files` as a part of a bigger job: `offset` bytes were done before, `grand` is the size of the whole job
HashReport HashPart(const std::vector<fs::path>& files, uint32_t algo, const ProgressFn& progress, uint64_t offset, uint64_t grand) {
    HashOptions ho;
    ho.algos = algo;
    if (progress) ho.progress = [&](uint64_t done, uint64_t) { return progress(offset + done, grand); };
    return HashFiles(files, ho);
}

uint64_t SizeOf(const std::vector<fs::path>& files) {
    uint64_t total = 0;
    for (const auto& f : files) {
        std::error_code ec;
        const auto s = fs::file_size(f, ec);
        if (!ec) total += s;
    }
    return total;
}

}  // namespace

SumWriteResult WriteSumFile(const fs::path& out, const std::vector<fs::path>& paths, const fs::path& base, const SumWriteOptions& options) {
    SumWriteResult res;
    const std::vector<fs::path> files = ExpandForHash(paths);
    const uint32_t algo = NormalizeHashAlgos(options.algo) & (~NormalizeHashAlgos(options.algo) + 1);
    const HashReport rep = HashPart(files, algo, options.progress, 0, SizeOf(files));
    res.skipped = rep.failed;
    WriteUtf8File(out, SumLines(rep, std::vector<fs::path>(rep.files.size(), base), algo));
    res.written.push_back(out);
    return res;
}

SumWriteResult WriteArchiveSums(const fs::path& archive, const std::vector<fs::path>& sources, bool archiveSum, bool filesSum, const SumWriteOptions& options,
                                const std::vector<fs::path>& parts) {
    SumWriteResult res;
    const uint32_t algo = NormalizeHashAlgos(options.algo) & (~NormalizeHashAlgos(options.algo) + 1);
    const wchar_t* ext = SumExtensionFor(algo);
    std::vector<fs::path> files, bases;
    if (filesSum)
        for (const auto& s : sources) {
            const auto part = ExpandForHash({s});
            for (const auto& f : part) { files.push_back(f); bases.push_back(s.parent_path()); }
        }
    const std::vector<fs::path> archiveFiles = parts.empty() ? std::vector<fs::path>{archive} : parts;
    const uint64_t archiveSize = archiveSum ? SizeOf(archiveFiles) : 0;
    const uint64_t grand = archiveSize + SizeOf(files);
    try {
        if (archiveSum) {
            const HashReport rep = HashPart(archiveFiles, algo, options.progress, 0, grand);
            if (rep.failed) throw Error(static_cast<long>(0x80070005L), "cannot read the archive");
            const fs::path out = archive.wstring() + ext;
            WriteUtf8File(out, SumLines(rep, std::vector<fs::path>(archiveFiles.size(), archive.parent_path()), algo));
            res.written.push_back(out);
        }
        if (filesSum && !files.empty()) {
            const HashReport rep = HashPart(files, algo, options.progress, archiveSize, grand);
            res.skipped = rep.failed;
            const std::wstring stem = ArchiveBaseName(archive);
            const fs::path out = archive.parent_path() / ((stem.empty() ? archive.stem().wstring() : stem) + ext);
            WriteUtf8File(out, SumLines(rep, bases, algo));
            res.written.push_back(out);
        }
    } catch (...) {
        for (const auto& w : res.written) { std::error_code ec; fs::remove(w, ec); }
        throw;
    }
    return res;
}

}  // namespace lycan
