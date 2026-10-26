#pragma once
// Reading one entry of an archive into memory for the previews (images, audio).
// A Reader owns its own ArchiveSource (never shared between threads: a mutex guarantees one user at a time), opens it
// once and reuses it; the progress callback lets the caller abort when the request was superseded.
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "lycan/archive_source.h"
#include "lycan/error.h"
#include "lycan/extract.h"
#include "lycan/i18n.h"

namespace lycan_app {

class VecSink final : public lycan::ByteSink {
public:
    VecSink(std::vector<uint8_t>& v, uint64_t cap) : v_(v), cap_(cap) {}
    void Write(const void* data, size_t size) override {
        if (v_.size() + size > cap_) throw lycan::Error(E_OUTOFMEMORY, winrt::to_string(lycan::Tr(L"preview.file_too_large")));
        auto* p = static_cast<const uint8_t*>(data);
        v_.insert(v_.end(), p, p + size);
    }
private:
    std::vector<uint8_t>& v_;
    uint64_t cap_;
};

// A dedicated ArchiveSource: only one thread uses it at a time (guaranteed with a mutex); opened once, read many times
class Reader {
public:
    void Reset(const std::filesystem::path& archive, const lycan::OpenOptions& open) {
        std::lock_guard lk(m_);
        src_.reset();
        archive_ = archive;
        open_ = open;
    }
    // Reads a whole item. Returns false on failure or cancel (err empty = cancelled)
    bool Read(uint32_t entry, uint64_t cap, std::vector<uint8_t>& out, const std::function<bool()>& keepGoing, std::wstring& err) {
        std::lock_guard lk(m_);
        try {
            if (!src_) src_ = lycan::ArchiveSource::Open(archive_, open_);
            VecSink sink(out, cap);
            src_->ReadEntry(entry, sink, [&](uint64_t, uint64_t) { return keepGoing(); });
            return true;
        } catch (lycan::Error const& e) {
            if (e.cancelled()) { err.clear(); return false; }
            err = e.wrongPassword() ? lycan::Tr(L"preview.wrong_password_file_cannot_be_read") : lycan::Tf(L"preview.failed_read_file", winrt::hstring{e.message().Text()});
        } catch (...) {
            err = lycan::Tr(L"preview.failed_read_file_plain");
        }
        return false;
    }
private:
    std::mutex m_;
    std::unique_ptr<lycan::ArchiveSource> src_;
    std::filesystem::path archive_;
    lycan::OpenOptions open_;
};

}  // namespace lycan_app
