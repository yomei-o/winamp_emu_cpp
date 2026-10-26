#include "files.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>
#include <sys/stat.h>
#include <sys/types.h>

#if defined(_WIN32)
#include <io.h>
#define x86emu_stat_struct struct _stat64
#else
#include <dirent.h>
#include <strings.h>
#include <unistd.h>
#define x86emu_stat_struct struct stat
#endif

#include "guest_printf.h"  // utf8_to_utf16, for wide host paths on Windows

namespace x86emu {
namespace {

// Host paths are UTF-8 strings everywhere in the emulator.  On Windows the
// narrow CRT interprets bytes in the ANSI code page, so a path with anything
// beyond ASCII in it - a Japanese %TMP%, say - has to go through the wide
// entry points instead.  On other hosts the bytes pass straight through.
#if defined(_WIN32)
std::wstring host_wide(const std::string& utf8) {
    std::u16string w = utf8_to_utf16(utf8);
    return std::wstring(w.begin(), w.end());
}
std::FILE* host_fopen(const std::string& path, const char* mode) {
    std::wstring wmode(mode, mode + std::strlen(mode));
    return _wfopen(host_wide(path).c_str(), wmode.c_str());
}
int x86emu_stat(const char* path, x86emu_stat_struct* st) {
    return _wstat64(host_wide(path).c_str(), st);
}
int host_remove(const std::string& path) { return _wremove(host_wide(path).c_str()); }
int host_rename(const std::string& from, const std::string& to) {
    return _wrename(host_wide(from).c_str(), host_wide(to).c_str());
}
#else
std::FILE* host_fopen(const std::string& path, const char* mode) {
    return std::fopen(path.c_str(), mode);
}
#define x86emu_stat stat
int host_remove(const std::string& path) { return std::remove(path.c_str()); }
int host_rename(const std::string& from, const std::string& to) {
    return std::rename(from.c_str(), to.c_str());
}
#endif

// errno-style negatives, using the Linux numbers.  The Win32 hooks translate
// them into their own error codes, and the Linux syscalls pass them straight
// through, which is why the table speaks in these terms.
constexpr int kENOENT = -2;
constexpr int kEBADF = -9;
constexpr int kEACCES = -13;
constexpr int kEEXIST = -17;
constexpr int kEINVAL = -22;
constexpr int kEMFILE = -24;
constexpr int kEPIPE = -32;

int from_errno(int e) {
    switch (e) {
        case ENOENT: return kENOENT;
        case EACCES: return kEACCES;
        case EEXIST: return kEEXIST;
        case EINVAL: return kEINVAL;
        default: return kEACCES;
    }
}

// The host's standard streams are never fclosed; everything else is closed when
// its last reference goes.
std::shared_ptr<std::FILE> own_stream(std::FILE* fp) {
    return std::shared_ptr<std::FILE>(fp, [](std::FILE* f) {
        if (f) std::fclose(f);
    });
}
std::shared_ptr<std::FILE> borrow_stream(std::FILE* fp) {
    return std::shared_ptr<std::FILE>(fp, [](std::FILE*) {});
}

std::string g_sysroot;
std::string g_drive_c;

}  // namespace

void FileTable::set_drive_c(std::string dir) {
    // Absolute, so it still names the same folder after the guest changes
    // directory.
#if defined(_WIN32)
    std::u16string w16 = utf8_to_utf16(dir);
    std::wstring w(w16.begin(), w16.end());
    if (wchar_t* full = _wfullpath(nullptr, w.c_str(), 0)) {
        std::string out;
        for (const wchar_t* q = full; *q; ++q) {
            uint32_t c = static_cast<uint16_t>(*q);
            if (c >= 0xD800 && c < 0xDC00 && q[1]) c = 0x10000 + ((c - 0xD800) << 10) + (static_cast<uint16_t>(*++q) - 0xDC00);
            if (c < 0x80) out += static_cast<char>(c);
            else if (c < 0x800) out += {static_cast<char>(0xC0 | c >> 6), static_cast<char>(0x80 | (c & 0x3F))};
            else if (c < 0x10000)
                out += {static_cast<char>(0xE0 | c >> 12), static_cast<char>(0x80 | ((c >> 6) & 0x3F)), static_cast<char>(0x80 | (c & 0x3F))};
            else
                out += {static_cast<char>(0xF0 | c >> 18), static_cast<char>(0x80 | ((c >> 12) & 0x3F)),
                        static_cast<char>(0x80 | ((c >> 6) & 0x3F)), static_cast<char>(0x80 | (c & 0x3F))};
        }
        std::free(full);
        dir = out;
    }
#else
    if (char* full = realpath(dir.c_str(), nullptr)) {
        dir = full;
        std::free(full);
    }
#endif
    for (char& c : dir)
        if (c == '\\') c = '/';
    while (!dir.empty() && dir.back() == '/') dir.pop_back();
    g_drive_c = std::move(dir);
}

const std::string& FileTable::drive_c() { return g_drive_c; }

void FileTable::set_sysroot(std::string dir) {
    for (char& c : dir)
        if (c == '\\') c = '/';
    while (!dir.empty() && dir.back() == '/') dir.pop_back();
    g_sysroot = std::move(dir);
}

FileTable::FileTable() {
    // The three standard streams are always present and always the host's.  Their
    // terminal-ness is whatever the host's really is, so that a guest asking
    // GetFileType or isatty gets the truth about where its output is going.
    auto standard = [](std::FILE* fp, const char* name, bool readable, bool writable) {
        Entry e;
        e.fp = borrow_stream(fp);
        e.path = name;
        e.readable = readable;
        e.writable = writable;
        e.standard_stream = true;
#if defined(_WIN32)
        e.is_tty = _isatty(_fileno(fp)) != 0;
#else
        e.is_tty = isatty(fileno(fp)) != 0;
#endif
        return e;
    };
    files_[0] = standard(stdin, "<stdin>", true, false);
    files_[1] = standard(stdout, "<stdout>", false, true);
    files_[2] = standard(stderr, "<stderr>", false, true);
}

std::string FileTable::host_path(const std::string& guest_path) {
    // Guests write paths in their own OS's convention and the host may be the
    // other one; backslashes work as separators on Windows and would be a
    // literal character on Unix, so normalise to forward slashes, which both
    // accept.
    std::string out;
    out.reserve(guest_path.size());
    for (char c : guest_path) out += (c == '\\') ? '/' : c;
    // A mapped C: drive takes the whole path; nothing below applies to it.
    if (!g_drive_c.empty() && out.size() >= 2 && (out[0] == 'C' || out[0] == 'c') && out[1] == ':') {
        out = g_drive_c + out.substr(2);
        while (out.size() > 1 && out.back() == '/') out.pop_back();
        return out;
    }
    // A Linux guest needs absolute paths that start with '/', and the emulator
    // hands it "/C:/dir/file" spellings for host paths on a Windows host (see
    // /proc/self/exe).  Undo that on the way back to the host filesystem.
    // (Only on a Windows host: on a Unix one "/C:/..." is where a Windows
    // guest's drive really lives, below, and translating twice must not undo it.)
    auto is_letter = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); };
#if defined(_WIN32)
    const bool drive_root = out.size() >= 3 && out[0] == '/' && is_letter(out[1]) && out[2] == ':';
    if (drive_root) out.erase(0, 1);
#else
    // On a Unix host (and in the browser) a Windows guest's drive letters are
    // directories at the root: "C:/jww/x" lives at "/C:/jww/x", so it means
    // the same thing whatever the working directory is.
    const bool drive_root = out.size() >= 2 && is_letter(out[0]) && out[1] == ':';
    if (drive_root) out.insert(out.begin(), '/');
    const bool already_drive = out.size() >= 3 && out[0] == '/' && is_letter(out[1]) && out[2] == ':';
#endif
    // Any other absolute Unix path lives inside the sysroot, when one is set.
#if defined(_WIN32)
    if (!drive_root && !g_sysroot.empty() && !out.empty() && out[0] == '/')
#else
    if (!drive_root && !already_drive && !g_sysroot.empty() && !out.empty() && out[0] == '/')
#endif
        out = g_sysroot + out;
    // A trailing separator is how a guest spells a directory, and how the NT
    // path form arrives, but stat() rejects it on Windows.  Drop it unless the
    // path is a root ("/" or "C:/"), which needs it to mean anything.
    while (out.size() > 1 && out.back() == '/' &&
           !(out.size() == 3 && out[1] == ':'))
        out.pop_back();
#if !defined(_WIN32)
    // A Windows guest's drive is case-insensitive, and the guest relies on it:
    // the loader asks for "common_lib_ap202.dll" when the file is
    // "common_lib_AP202.dll".  When the exact path is not there, match each
    // component without regard to ASCII case.
    if (out.size() >= 3 && out[0] == '/' && out[2] == ':') {
        struct stat st;
        if (stat(out.c_str(), &st) != 0) {
            std::string fixed;
            size_t pos = 1;
            bool ok = true;
            fixed = "";
            while (pos <= out.size()) {
                size_t next = out.find('/', pos);
                if (next == std::string::npos) next = out.size();
                std::string part = out.substr(pos, next - pos);
                std::string dir = fixed.empty() ? "/" : fixed;
                std::string candidate = fixed + "/" + part;
                if (ok && stat(candidate.c_str(), &st) != 0) {
                    ok = false;
                    if (DIR* d = opendir(dir.c_str())) {
                        while (dirent* ent = readdir(d)) {
                            if (strcasecmp(ent->d_name, part.c_str()) == 0) {
                                candidate = fixed + "/" + ent->d_name;
                                ok = true;
                                break;
                            }
                        }
                        closedir(d);
                    }
                }
                fixed = candidate;
                pos = next + 1;
            }
            if (ok) out = fixed;
        }
    }
#endif
    return out;
}

int FileTable::alloc_slot() {
    // Guests, and especially libcs, expect the lowest free descriptor.
    for (int fd = 3; fd < 4096; ++fd)
        if (files_.find(fd) == files_.end()) return fd;
    return -1;
}

// Putting a descriptor back where it was, for a restored state.
//
// Unlike open() this does not choose the number - the guest already has it
// written down all over its memory - and it does not create or truncate: the
// file is being *rejoined*, not made.  A file that has moved or been deleted
// since the state was saved cannot be rejoined, and saying so is better than
// handing back a descriptor that reads zeros.
bool FileTable::restore(int fd, const std::string& path, const OpenFlags& flags,
                        uint64_t at) {
    // The three standard streams are the host's own and were never ours to
    // close; whatever the front end has them pointing at now is what a resumed
    // guest should write to.
    if (fd >= 0 && fd <= 2 && files_.count(fd)) return true;

    if (flags.create || flags.truncate) {
        // Saved as write-created, restored as write-existing: the file is there
        // now, and remaking it would throw away what the guest wrote before the
        // snapshot.
    }
    std::string mode = flags.append ? (flags.read ? "a+" : "a")
                     : (flags.write && flags.read) ? "r+"
                     : flags.write ? "r+"
                     : "r";
    if (flags.binary) mode += "b";

    std::FILE* fp = host_fopen(host_path(path), mode.c_str());
    if (!fp) return false;
    if (at) std::fseek(fp, (long)at, SEEK_SET);

    Entry e;
    e.fp = own_stream(fp);
    e.path = path;
    e.readable = flags.read;
    e.writable = flags.write || flags.append;
    e.append = flags.append;
    e.text_mode = translate_newlines_ && !flags.binary;
    e.wide_io = flags.wide_io;
    files_[fd] = e;
    return true;
}

int FileTable::open(const std::string& path, const OpenFlags& flags) {
    if (std::getenv("X86EMU_TRACE_OPEN"))
        std::fprintf(stderr, "x86emu: open(%s)\n", path.c_str());
    std::string mode;
    if (flags.append)
        mode = flags.read ? "a+" : "a";
    else if (flags.write && flags.read)
        mode = flags.truncate ? "w+" : (flags.create ? "w+" : "r+");
    else if (flags.write)
        mode = "w";
    else
        mode = "r";
    if (flags.binary) mode += "b";

    std::string host = host_path(path);

    // O_EXCL means "fail if it already exists", which fopen has no mode for.
    if (flags.exclusive && flags.create) {
        Stat probe;
        if (stat_path(path, probe) == 0) return kEEXIST;
    }
    // Opening for update without O_CREAT must not create the file, and "r+"
    // already behaves that way; the reverse case needs no special handling.
    std::FILE* fp = host_fopen(host, mode.c_str());
    if (!fp && flags.write && flags.read && flags.create && !flags.truncate) {
        // "r+" failed because the file does not exist yet and we may create it.
        fp = host_fopen(host, flags.binary ? "w+b" : "w+");
    }
    if (!fp) return from_errno(errno);

    int fd = alloc_slot();
    if (fd < 0) {
        std::fclose(fp);
        return kEMFILE;
    }
    Entry e;
    e.fp = own_stream(fp);
    e.path = path;
    e.readable = flags.read || (flags.write && flags.read);
    e.writable = flags.write || flags.append;
    e.append = flags.append;
    e.text_mode = translate_newlines_ && !flags.binary;
    e.wide_io = flags.wide_io;
    files_[fd] = e;
    return fd;
}

int FileTable::open_directory(const std::string& path) {
    Stat st;
    if (stat_path(path, st) != 0) return kENOENT;
    if (!st.is_dir) return kEINVAL;
    int fd = alloc_slot();
    if (fd < 0) return kEMFILE;
    Entry e;
    e.path = path;
    e.readable = true;
    e.is_directory = true;
    files_[fd] = e;
    return fd;
}

int FileTable::make_pipe(int fds[2]) {
    int rd = alloc_slot();
    if (rd < 0) return kEMFILE;
    auto pipe = std::make_shared<Pipe>();
    Entry read_end;
    read_end.pipe_end = std::make_shared<PipeEnd>(pipe, false);
    read_end.path = "<pipe:r>";
    read_end.readable = true;
    files_[rd] = read_end;

    int wr = alloc_slot();
    if (wr < 0) {
        files_.erase(rd);
        return kEMFILE;
    }
    Entry write_end;
    write_end.pipe_end = std::make_shared<PipeEnd>(pipe, true);
    write_end.path = "<pipe:w>";
    write_end.writable = true;
    files_[wr] = write_end;

    fds[0] = rd;
    fds[1] = wr;
    return 0;
}

void FileTable::install(int fd, const Entry& entry) {
    auto it = files_.find(fd);
    if (it != files_.end()) close(fd);
    Entry copy = entry;
    // The inherited descriptor is the child's now; whether it may be closed is
    // the child's own affair, and close-on-exec does not survive inheritance.
    copy.cloexec = false;
    files_[fd] = copy;
}

FileTable FileTable::clone() const {
    FileTable out;
    out.translate_newlines_ = translate_newlines_;
    out.files_.clear();
    // Entry copies share the stream and the pipe end, which is what fork()
    // means: one file description, two tables.
    for (const auto& [fd, e] : files_) out.files_[fd] = e;
    return out;
}

void FileTable::close_cloexec() {
    std::vector<int> doomed;
    for (const auto& [fd, e] : files_)
        if (e.cloexec) doomed.push_back(fd);
    for (int fd : doomed) close(fd);
}

bool FileTable::valid(int fd) const {
    auto it = files_.find(fd);
    return it != files_.end() && !it->second.closed &&
           (it->second.fp != nullptr || it->second.is_directory || it->second.pipe_end);
}

std::string FileTable::path_of(int fd) const {
    auto it = files_.find(fd);
    return it == files_.end() ? std::string() : it->second.path;
}

FileTable::Entry* FileTable::get(int fd) {
    auto it = files_.find(fd);
    if (it == files_.end() || it->second.closed) return nullptr;
    return &it->second;
}

int FileTable::close(int fd) {
    Entry* e = get(fd);
    if (!e) return kEBADF;
    // The shared_ptr closes the stream when the last duplicate goes, and the
    // pipe end adjusts its pipe's reader/writer count the same way.
    files_.erase(fd);
    return 0;
}

int64_t FileTable::read(int fd, void* dst, uint64_t len) {
    Entry* e = get(fd);
    if (!e) return kEBADF;
    if (!e->readable || e->is_directory) return kEBADF;
    if (len == 0) return 0;
    if (e->is_pipe()) {
        Pipe& p = *e->pipe_end->pipe;
        if (p.buffer.empty())
            return p.writers > 0 ? kEAGAINPipe : 0;  // block-and-retry, or EOF
        size_t n = std::min<size_t>(static_cast<size_t>(len), p.buffer.size());
        auto* out = static_cast<uint8_t*>(dst);
        for (size_t i = 0; i < n; ++i) {
            out[i] = p.buffer.front();
            p.buffer.pop_front();
        }
        return static_cast<int64_t>(n);
    }
    if (e->last_was_write) {
        std::fflush(e->fp.get());
        e->last_was_write = false;
    }
    size_t got = std::fread(dst, 1, static_cast<size_t>(len), e->fp.get());
    if (e->text_mode && got) {
        // Text mode collapses CRLF to LF, so the guest sees fewer bytes than
        // were on disk - which is exactly what a real CRT reports.
        auto* p = static_cast<uint8_t*>(dst);
        size_t out = 0;
        for (size_t i = 0; i < got; ++i) {
            if (p[i] == '\r' && i + 1 < got && p[i + 1] == '\n') continue;
            p[out++] = p[i];
        }
        got = out;
    }
    return static_cast<int64_t>(got);
}

int64_t FileTable::write(int fd, const void* src, uint64_t len) {
    Entry* e = get(fd);
    if (!e) return kEBADF;
    if (!e->writable || e->is_directory) return kEBADF;
    if (len == 0) return 0;
    if (e->is_pipe()) {
        Pipe& p = *e->pipe_end->pipe;
        if (p.readers <= 0) return kEPIPE;  // nobody will ever read this
        const auto* in = static_cast<const uint8_t*>(src);
        p.buffer.insert(p.buffer.end(), in, in + len);
        return static_cast<int64_t>(len);
    }
    if (!e->last_was_write && !e->standard_stream) {
        // Seeking to the current position is the standard way to switch a stream
        // from reading to writing.
        std::fseek(e->fp.get(), 0, SEEK_CUR);
    }
    e->last_was_write = true;
    if (e->text_mode) {
        const auto* p = static_cast<const uint8_t*>(src);
        std::vector<uint8_t> expanded;
        expanded.reserve(static_cast<size_t>(len) + 16);
        for (uint64_t i = 0; i < len; ++i) {
            if (p[i] == '\n') expanded.push_back('\r');
            expanded.push_back(p[i]);
        }
        if (std::fwrite(expanded.data(), 1, expanded.size(), e->fp.get()) != expanded.size())
            return 0;
        if (e->standard_stream) std::fflush(e->fp.get());
        // Report the count the guest asked about, not the expanded one.
        return static_cast<int64_t>(len);
    }
    size_t put = std::fwrite(src, 1, static_cast<size_t>(len), e->fp.get());
    // A guest write to a standard stream has to leave the emulator, not sit in
    // the emulator's own stdio buffer.  The guest flushing its buffer only gets
    // the bytes as far as here, and when stdout is a pipe rather than a
    // terminal that buffer is 4 KB and nobody empties it - so a guest that
    // writes an answer and waits for the next request, and a host that wrote a
    // request and waits for the answer, wait for each other forever.  It also
    // means a long run's progress can be watched through a redirect, which it
    // could not before.
    if (e->standard_stream) std::fflush(e->fp.get());
    return static_cast<int64_t>(put);
}

int64_t FileTable::seek(int fd, int64_t offset, int whence) {
    Entry* e = get(fd);
    if (!e) return kEBADF;
    if (e->standard_stream || e->is_directory) return kEINVAL;
    if (e->is_pipe()) return -29;  // ESPIPE
    e->last_was_write = false;
    int origin = whence == 1 ? SEEK_CUR : whence == 2 ? SEEK_END : SEEK_SET;
#if defined(_WIN32)
    if (_fseeki64(e->fp.get(), offset, origin) != 0) return kEINVAL;
    return _ftelli64(e->fp.get());
#else
    if (fseeko(e->fp.get(), static_cast<off_t>(offset), origin) != 0) return kEINVAL;
    return static_cast<int64_t>(ftello(e->fp.get()));
#endif
}

int64_t FileTable::tell(int fd) {
    Entry* e = get(fd);
    if (!e) return kEBADF;
    if (e->is_pipe() || !e->fp) return -29;  // ESPIPE
#if defined(_WIN32)
    return _ftelli64(e->fp.get());
#else
    return static_cast<int64_t>(ftello(e->fp.get()));
#endif
}

int64_t FileTable::size(int fd) {
    Stat s;
    int r = stat_fd(fd, s);
    return r < 0 ? r : static_cast<int64_t>(s.size);
}

int FileTable::truncate(int fd, int64_t length) {
    Entry* e = get(fd);
    if (!e || e->is_directory || e->is_pipe()) return kEBADF;
    if (std::fflush(e->fp.get()) != 0) return kEINVAL;
#if defined(_WIN32)
    int r = _chsize_s(_fileno(e->fp.get()), length);
#else
    int r = ftruncate(fileno(e->fp.get()), length);
#endif
    return r == 0 ? 0 : kEINVAL;
}

int FileTable::flush(int fd) {
    Entry* e = get(fd);
    if (!e) return kEBADF;
    if (e->is_directory || e->is_pipe()) return 0;
    return std::fflush(e->fp.get()) == 0 ? 0 : kEINVAL;
}

bool FileTable::eof(int fd) {
    Entry* e = get(fd);
    if (!e) return true;
    if (e->is_pipe()) {
        Pipe& p = *e->pipe_end->pipe;
        return p.buffer.empty() && p.writers <= 0;
    }
    return e->fp ? std::feof(e->fp.get()) != 0 : true;
}

int FileTable::dup(int fd, int to) {
    Entry* e = get(fd);
    if (!e) return kEBADF;
    int target = to;
    if (target < 0) {
        target = alloc_slot();
        if (target < 0) return kEMFILE;
    } else if (target == fd) {
        return target;
    }
    // Both descriptors share one file description: stream, offset, pipe end.
    Entry copy = *e;
    copy.cloexec = false;  // dup() clears close-on-exec on the new descriptor
    if (files_.find(target) != files_.end()) close(target);
    files_[target] = copy;
    return target;
}

int FileTable::remove_file(const std::string& path) {
    // X86EMU_KEEP_TEMP dumps a file's contents as the guest deletes it.  A
    // toolchain that talks to itself through response files destroys the evidence
    // on the way out, and this is the only way to read what one process actually
    // handed the next.  Printing rather than keeping the file matters: a guest
    // that recreates the same name would overwrite whatever was kept.
    if (std::getenv("X86EMU_KEEP_TEMP")) {
        std::fprintf(stderr, "x86emu: deleting %s:\n", path.c_str());
        if (FILE* f = host_fopen(host_path(path), "rb")) {
            unsigned char buf[64];
            size_t got;
            size_t total = 0;
            while ((got = std::fread(buf, 1, sizeof buf, f)) > 0 && total < 512) {
                for (size_t i = 0; i < got; ++i) std::fprintf(stderr, "%02X ", buf[i]);
                std::fprintf(stderr, "| ");
                for (size_t i = 0; i < got; ++i)
                    std::fputc(buf[i] >= 0x20 && buf[i] < 0x7F ? buf[i] : '.', stderr);
                std::fputc('\n', stderr);
                total += got;
            }
            std::fclose(f);
        }
    }

    if (host_remove(host_path(path)) != 0) return from_errno(errno);
    return 0;
}

int FileTable::rename_file(const std::string& from, const std::string& to) {
    if (host_rename(host_path(from), host_path(to)) != 0)
        return from_errno(errno);
    return 0;
}

int FileTable::stat_path(const std::string& path, Stat& out) { return stat_host(host_path(path), out); }

int FileTable::stat_host(const std::string& host, Stat& out) {
    x86emu_stat_struct st;
    if (x86emu_stat(host.c_str(), &st) != 0) return from_errno(errno);
    out.size = static_cast<uint64_t>(st.st_size);
    out.is_dir = (st.st_mode & S_IFMT) == S_IFDIR;
    out.is_char_device = (st.st_mode & S_IFMT) == S_IFCHR;
    out.mtime = static_cast<int64_t>(st.st_mtime);
    out.ino = inode_for(host);
    return 0;
}

uint64_t FileTable::inode_for(const std::string& host_path_in) {
    // Windows' _stat64 reports st_ino as 0, so the number is invented here -
    // stably, from the path, since callers compare inodes between two stats of
    // the same file.  Case is folded because Windows paths are case-insensitive
    // and "the same file" must hash the same either way.
#if !defined(_WIN32)
    x86emu_stat_struct st;
    if (x86emu_stat(host_path_in.c_str(), &st) == 0 && st.st_ino)
        return static_cast<uint64_t>(st.st_ino);
#endif
    uint64_t h = 1469598103934665603ull;  // FNV-1a
    for (char c : host_path_in) {
        char lower = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
        h ^= static_cast<unsigned char>(lower);
        h *= 1099511628211ull;
    }
    return h ? h : 1;  // 0 means "no inode" to some callers
}

int FileTable::stat_fd(int fd, Stat& out) {
    Entry* e = get(fd);
    if (!e) return kEBADF;
    if (e->is_pipe()) {
        out.is_fifo = true;
        out.size = e->pipe_end->pipe->buffer.size();
        // A pipe's identity is the pipe object, so that two descriptors on the
        // same pipe stat alike and two different pipes do not.
        out.ino = reinterpret_cast<uintptr_t>(e->pipe_end->pipe.get());
        out.dev = 2;
        return 0;
    }
    if (e->standard_stream) {
        out.is_char_device = true;
        out.size = 0;
        out.ino = static_cast<uint64_t>(fd) + 1;
        out.dev = 3;
        return 0;
    }
    // A directory handle has no stream to measure; the path is the whole answer.
    if (e->is_directory) return stat_path(e->path, out);
    // Start from the path so mode and timestamps are right, then take the size
    // from the stream itself: Windows does not update a file's directory entry
    // until the handle is closed, so stat() on an open file can report a stale
    // size where the guest expects the value its own write() just produced.
    int r = stat_path(e->path, out);
    if (r != 0) return r;
    if (e->last_was_write) {
        std::fflush(e->fp.get());
        e->last_was_write = false;
    }
#if defined(_WIN32)
    long long here = _ftelli64(e->fp.get());
    if (here >= 0 && _fseeki64(e->fp.get(), 0, SEEK_END) == 0) {
        long long end = _ftelli64(e->fp.get());
        if (end >= 0) out.size = static_cast<uint64_t>(end);
        _fseeki64(e->fp.get(), here, SEEK_SET);
    }
#else
    off_t here = ftello(e->fp.get());
    if (here >= 0 && fseeko(e->fp.get(), 0, SEEK_END) == 0) {
        off_t end = ftello(e->fp.get());
        if (end >= 0) out.size = static_cast<uint64_t>(end);
        fseeko(e->fp.get(), here, SEEK_SET);
    }
#endif
    return 0;
}

}  // namespace x86emu
