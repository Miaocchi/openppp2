#include "NetworkRecoveryJournal.h"

#include <ppp/Filesystem.h>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace ppp::app::client {
namespace {
namespace fs = ppp::filesystem::fs;
using ppp::filesystem::error_code;
constexpr std::size_t kMaxBytes = 4u * 1024u * 1024u;
constexpr std::size_t kMaxFieldBytes = 1024u * 1024u;

void Error(std::string* out, const char* value) { if (out) *out = value; }

uint64_t Checksum(const std::string& value) {
    uint64_t hash = 14695981039346656037ull;
    for (unsigned char ch : value) { hash ^= ch; hash *= 1099511628211ull; }
    return hash;
}

std::string Hex(uint64_t value) {
    static const char digits[] = "0123456789abcdef";
    std::string result(16, '0');
    for (int i = 15; i >= 0; --i) { result[static_cast<std::size_t>(i)] = digits[value & 15u]; value >>= 4; }
    return result;
}

std::string Encode(const std::string& value) {
    static const char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(value.size() * 2);
    for (unsigned char ch : value) { result.push_back(digits[ch >> 4]); result.push_back(digits[ch & 15u]); }
    return result;
}

bool Decode(const std::string& value, std::string& result) {
    if ((value.size() & 1u) != 0 || value.size() > kMaxFieldBytes * 2) return false;
    auto digit = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        return -1;
    };
    result.clear(); result.reserve(value.size() / 2);
    for (std::size_t i = 0; i < value.size(); i += 2) {
        const int high = digit(value[i]), low = digit(value[i + 1]);
        if (high < 0 || low < 0) return false;
        result.push_back(static_cast<char>((high << 4) | low));
    }
    return true;
}

bool Read(const fs::path& path, std::string& bytes) {
    error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec || size > kMaxBytes) return false;
    std::ifstream stream(ppp::filesystem::StreamPath(path), std::ios::binary);
    if (!stream) return false;
    bytes.resize(static_cast<std::size_t>(size));
    if (!bytes.empty()) stream.read(&bytes[0], static_cast<std::streamsize>(bytes.size()));
    return stream.good() || (stream.eof() && static_cast<std::size_t>(stream.gcount()) == bytes.size());
}

bool FlushDirectory(const fs::path& path) {
#if defined(_WIN32)
    (void)path;
    return true;
#else
    const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) return false;
    const bool ok = ::fsync(fd) == 0;
    ::close(fd);
    return ok;
#endif
}

bool Split(const std::string& line, std::vector<std::string>& fields) {
    std::size_t start = 0;
    for (;;) {
        const auto bar = line.find('|', start);
        fields.emplace_back(line.substr(start, bar == std::string::npos ? bar : bar - start));
        if (bar == std::string::npos) return true;
        start = bar + 1;
    }
}

std::string Body(const std::vector<NetworkRecoveryJournal::Entry>& entries) {
    std::string body;
    for (const auto& entry : entries) {
        body += "E|" + Encode(entry.resource) + "|" + Encode(entry.target) + "|" +
            Encode(entry.before) + "|" + Encode(entry.after) + "|" +
            (entry.confirmed ? "confirmed" : "intent") + "\n";
    }
    return body;
}

bool Parse(const std::string& bytes, const std::string& owner,
           std::vector<NetworkRecoveryJournal::Entry>& entries) {
    const auto newline = bytes.find('\n');
    if (newline == std::string::npos) return false;
    std::vector<std::string> header;
    Split(bytes.substr(0, newline), header);
    if (header.size() != 4 || header[0] != "NWRJ" || header[1] != "1" ||
        header[2] != Encode(owner) || header[3] != Hex(Checksum(bytes.substr(newline + 1)))) return false;
    std::size_t start = newline + 1;
    while (start < bytes.size()) {
        const auto end = bytes.find('\n', start);
        if (end == std::string::npos) return false;
        std::vector<std::string> fields;
        Split(bytes.substr(start, end - start), fields);
        if (fields.size() != 6 || fields[0] != "E" ||
            (fields[5] != "intent" && fields[5] != "confirmed")) return false;
        NetworkRecoveryJournal::Entry entry;
        if (!Decode(fields[1], entry.resource) || !Decode(fields[2], entry.target) ||
            !Decode(fields[3], entry.before) || !Decode(fields[4], entry.after) ||
            entry.resource.empty() || entry.target.empty() || entry.before.size() > kMaxFieldBytes ||
            entry.after.size() > kMaxFieldBytes) return false;
        entry.confirmed = fields[5] == "confirmed";
        const auto duplicate = std::find_if(entries.begin(), entries.end(), [&](const auto& item) {
            return item.resource == entry.resource && item.target == entry.target;
        });
        if (duplicate != entries.end()) return false;
        entries.emplace_back(std::move(entry));
        start = end + 1;
    }
    return true;
}

bool WriteAtomic(const fs::path& directory, const fs::path& target, const std::string& bytes,
                 const NetworkRecoveryJournal::Options& options, std::string* error) {
    if (options.allow_stage && !options.allow_stage(NetworkRecoveryJournal::Stage::Write)) { Error(error, "journal write failed"); return false; }
    const fs::path temporary = target.string() + ".tmp";
#if defined(_WIN32)
    HANDLE handle = CreateFileW(temporary.wstring().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) { Error(error, "journal open failed"); return false; }
    DWORD written = 0;
    const bool wrote = bytes.empty() || (WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size());
    bool flushed = wrote;
    if (options.allow_stage && !options.allow_stage(NetworkRecoveryJournal::Stage::Flush)) flushed = false;
    if (flushed) flushed = FlushFileBuffers(handle) != 0;
    CloseHandle(handle);
    if (!flushed) { Error(error, "journal flush failed"); return false; }
    if (options.allow_stage && !options.allow_stage(NetworkRecoveryJournal::Stage::Rename)) { Error(error, "journal rename failed"); return false; }
    if (!MoveFileExW(temporary.wstring().c_str(), target.wstring().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { Error(error, "journal rename failed"); return false; }
#else
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { Error(error, "journal open failed"); return false; }
    bool wrote = true;
    const char* data = bytes.data();
    std::size_t left = bytes.size();
    while (left) { const ssize_t count = ::write(fd, data, left); if (count < 0 && errno == EINTR) continue; if (count <= 0) { wrote = false; break; } data += count; left -= static_cast<std::size_t>(count); }
    if (options.allow_stage && !options.allow_stage(NetworkRecoveryJournal::Stage::Flush)) wrote = false;
    if (wrote && ::fsync(fd) != 0) wrote = false;
    if (::close(fd) != 0) wrote = false;
    if (!wrote) { Error(error, "journal flush failed"); return false; }
    if (options.allow_stage && !options.allow_stage(NetworkRecoveryJournal::Stage::Rename)) { Error(error, "journal rename failed"); return false; }
    if (::rename(temporary.c_str(), target.c_str()) != 0) { Error(error, "journal rename failed"); return false; }
#endif
    if (options.allow_stage && !options.allow_stage(NetworkRecoveryJournal::Stage::DirectoryFlush)) { Error(error, "journal directory flush failed"); return false; }
    if (!FlushDirectory(directory)) { Error(error, "journal directory flush failed"); return false; }
    return true;
}
} // namespace

NetworkRecoveryJournal::~NetworkRecoveryJournal() { Close(); }

bool NetworkRecoveryJournal::StageAllowed(Stage stage) const { return !options_.allow_stage || options_.allow_stage(stage); }

bool NetworkRecoveryJournal::Open(const std::string& directory, const std::string& owner,
                                  const Options& options, std::string* error) {
    Close();
    if (directory.empty() || owner.empty() || owner.size() > kMaxFieldBytes) { Error(error, "invalid journal identity"); return false; }
    options_ = options; directory_ = directory; owner_ = owner;
    const fs::path path(directory_);
    error_code ec;
    if (!fs::exists(path, ec)) {
        if (!StageAllowed(Stage::DirectoryCreate) || !fs::create_directories(path, ec) || ec) { Error(error, "journal directory unavailable"); Close(); return false; }
#if !defined(_WIN32)
        ::chmod(path.c_str(), 0700);
#endif
    }
    if (ec || !fs::is_directory(path, ec) || ec) { Error(error, "journal directory unavailable"); Close(); return false; }
    const fs::path lock = path / "network-recovery.lock";
#if defined(_WIN32)
    if (!StageAllowed(Stage::Lock)) { Error(error, "journal lock failed"); Close(); return false; }
    HANDLE handle = CreateFileW(lock.wstring().c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (handle == INVALID_HANDLE_VALUE) { Error(error, "journal lock failed"); Close(); return false; }
    lock_handle_ = reinterpret_cast<intptr_t>(handle);
#else
    if (!StageAllowed(Stage::Lock)) { Error(error, "journal lock failed"); Close(); return false; }
    const int fd = ::open(lock.c_str(), O_RDWR | O_CREAT, 0600);
    if (fd < 0 || ::flock(fd, LOCK_EX | LOCK_NB) != 0) { if (fd >= 0) ::close(fd); Error(error, "journal lock failed"); Close(); return false; }
    ::fchmod(fd, 0600); lock_handle_ = fd;
#endif
    const fs::path journal = path / "network-recovery.journal";
    if (fs::exists(journal, ec)) {
        std::string bytes;
        if (ec || !Read(journal, bytes) || !Parse(bytes, owner_, entries_)) { Error(error, "journal is corrupt or has a different owner"); Close(); return false; }
#if !defined(_WIN32)
        // Recovery state can contain interface configuration; do not leave it
        // readable by unrelated local users after an existing file is reused.
        ::chmod(journal.c_str(), 0600);
#endif
    } else if (ec) { Error(error, "journal metadata unavailable"); Close(); return false; }
    open_ = true;
    return true;
}

void NetworkRecoveryJournal::CloseWithLock() noexcept {
#if defined(_WIN32)
    if (lock_handle_ != -1) CloseHandle(reinterpret_cast<HANDLE>(lock_handle_));
#else
    if (lock_handle_ != -1) { flock(static_cast<int>(lock_handle_), LOCK_UN); ::close(static_cast<int>(lock_handle_)); }
#endif
    lock_handle_ = -1;
}

void NetworkRecoveryJournal::Close() noexcept { open_ = false; entries_.clear(); CloseWithLock(); directory_.clear(); owner_.clear(); }

bool NetworkRecoveryJournal::Rewrite(std::string* error) {
    if (!open_) { Error(error, "journal is not open"); return false; }
    const std::string body = Body(entries_);
    const std::string bytes = "NWRJ|1|" + Encode(owner_) + "|" + Hex(Checksum(body)) + "\n" + body;
    return WriteAtomic(fs::path(directory_), fs::path(directory_) / "network-recovery.journal", bytes, options_, error);
}

bool NetworkRecoveryJournal::RecordIntent(const std::string& resource, const std::string& target,
                                          const std::string& before, const std::string& after,
                                          std::string* error) {
    if (resource.empty() || target.empty() || before.size() > kMaxFieldBytes || after.size() > kMaxFieldBytes) { Error(error, "invalid journal entry"); return false; }
    auto it = std::find_if(entries_.begin(), entries_.end(), [&](const auto& item) { return item.resource == resource && item.target == target; });
    const bool inserted = it == entries_.end();
    if (inserted) entries_.push_back({resource, target, before, after, false});
    else { it->before = before; it->after = after; it->confirmed = false; }
    if (!Rewrite(error)) { if (inserted) entries_.pop_back(); return false; }
    return true;
}

bool NetworkRecoveryJournal::Confirm(const std::string& resource, const std::string& target, std::string* error) {
    auto it = std::find_if(entries_.begin(), entries_.end(), [&](const auto& item) { return item.resource == resource && item.target == target; });
    if (it == entries_.end()) { Error(error, "journal entry not found"); return false; }
    const bool previous = it->confirmed; it->confirmed = true;
    if (!Rewrite(error)) { it->confirmed = previous; return false; }
    return true;
}

bool NetworkRecoveryJournal::Remove(const std::string& resource, const std::string& target, std::string* error) {
    const auto it = std::find_if(entries_.begin(), entries_.end(), [&](const auto& item) { return item.resource == resource && item.target == target; });
    if (it == entries_.end()) { Error(error, "journal entry not found"); return false; }
    const Entry backup = *it; entries_.erase(it);
    if (!Rewrite(error)) { entries_.push_back(backup); return false; }
    return true;
}

std::vector<NetworkRecoveryJournal::Entry> NetworkRecoveryJournal::Pending() const { return entries_; }
bool NetworkRecoveryJournal::HasPending() const { return !entries_.empty(); }

} // namespace ppp::app::client
