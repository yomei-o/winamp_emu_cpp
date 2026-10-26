// Host implementations of the library functions the guest imports.
//
// Every hook reads its arguments through Emulator::Args (which knows the active
// calling convention), does the work with real host C++, and leaves the result
// in RAX/EAX.  The emulator performs the return, so a hook never touches RIP.
#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "emulator.h"
#include "guest_printf.h"

namespace x86emu {
namespace {

// ---------------------------------------------------------------------------
// printf-family formatting
// ---------------------------------------------------------------------------

void write_out(Emulator& e, int fd, const std::string& s) {
    // The standard streams go through the emulator's output path (and its
    // newline translation); anything else is a real file.
    if (fd >= 0 && fd <= 2)
        e.write_text(fd, s);
    else if (fd > 2)
        e.files.write(fd, s.data(), s.size());
}

// Resolves a guest FILE* to a host stream, defaulting to stdout when the guest
// obtained it from something we do not model (e.g. an imported _iob array).
int stream_fd(Emulator& e, uint64_t guest_file) {
    int fd = e.host_fd(guest_file);
    return fd < 0 ? 1 : fd;
}

}  // namespace

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void Emulator::install_library_hooks() {
    // cdecl / caller-cleanup: the C runtime.
    auto libc = [this](const char* name, std::function<void(Emulator&)> fn) {
        add_hook(name, 0, std::move(fn));
    };
    // ---- stdio ------------------------------------------------------------
    libc("printf", [](Emulator& e) {
        Args va(e, 1);
        std::string s = format_guest(e, e.arg_slot(0), va);
        write_out(e, 1, s);
        e.set_result(s.size());
    });
    libc("puts", [](Emulator& e) {
        std::string s = e.mem.read_cstring(e.arg_slot(0));
        write_out(e, 1, s + "\n");
        e.set_result(static_cast<uint64_t>(s.size() + 1));
    });
    libc("putchar", [](Emulator& e) {
        char c = static_cast<char>(e.arg_slot(0));
        write_out(e, 1, std::string(1, c));
        e.set_result(static_cast<uint8_t>(c));
    });
    libc("fprintf", [](Emulator& e) {
        int fd = stream_fd(e, e.arg_slot(0));
        Args va(e, 2);
        std::string s = format_guest(e, e.arg_slot(1), va);
        write_out(e, fd, s);
        e.set_result(s.size());
    });
    libc("vfprintf", [](Emulator& e) {
        int fd = stream_fd(e, e.arg_slot(0));
        Args va = Args::va_list_at(e, e.arg_slot(2));
        std::string s = format_guest(e, e.arg_slot(1), va);
        write_out(e, fd, s);
        e.set_result(s.size());
    });
    libc("vprintf", [](Emulator& e) {
        Args va = Args::va_list_at(e, e.arg_slot(1));
        std::string s = format_guest(e, e.arg_slot(0), va);
        write_out(e, 1, s);
        e.set_result(s.size());
    });
    libc("fputs", [](Emulator& e) {
        std::string s = e.mem.read_cstring(e.arg_slot(0));
        write_out(e, stream_fd(e, e.arg_slot(1)), s);
        e.set_result(static_cast<uint64_t>(s.size()));
    });
    libc("fputc", [](Emulator& e) {
        char c = static_cast<char>(e.arg_slot(0));
        write_out(e, stream_fd(e, e.arg_slot(1)), std::string(1, c));
        e.set_result(static_cast<uint8_t>(c));
    });
    libc("putc", [](Emulator& e) {
        char c = static_cast<char>(e.arg_slot(0));
        write_out(e, stream_fd(e, e.arg_slot(1)), std::string(1, c));
        e.set_result(static_cast<uint8_t>(c));
    });
    libc("sprintf", [](Emulator& e) {
        Args va(e, 2);
        std::string s = format_guest(e, e.arg_slot(1), va);
        e.mem.write_cstring(e.arg_slot(0), s);
        e.set_result(s.size());
    });
    libc("snprintf", [](Emulator& e) {
        uint64_t buf = e.arg_slot(0);
        uint64_t n = e.arg_slot(1);
        Args va(e, 3);
        std::string s = format_guest(e, e.arg_slot(2), va);
        if (n > 0) {
            std::string t = s.substr(0, static_cast<size_t>(n - 1));
            e.mem.write_cstring(buf, t);
        }
        e.set_result(s.size());  // the length it *would* have taken
    });
    libc("_snprintf", [](Emulator& e) {
        uint64_t buf = e.arg_slot(0);
        uint64_t n = e.arg_slot(1);
        Args va(e, 3);
        std::string s = format_guest(e, e.arg_slot(2), va);
        size_t take = s.size() < n ? s.size() : static_cast<size_t>(n);
        e.mem.write(buf, s.data(), take);
        if (take < n) e.mem.write8(buf + take, 0);
        e.set_result(s.size() < n ? s.size() : 0xFFFFFFFFull);
    });
    // The "_nolock" variants exist because a caller has already taken the
    // stream's lock; with one guest thread at a time the two are the same
    // function, and registering both is what keeps a guest from finding one
    // missing at the worst moment.
    auto do_fwrite = [](Emulator& e) {
        uint64_t ptr = e.arg_slot(0), size = e.arg_slot(1), count = e.arg_slot(2);
        int fd = stream_fd(e, e.arg_slot(3));
        uint64_t total = size * count;
        std::string data(static_cast<size_t>(total), '\0');
        if (total) e.mem.read(ptr, data.data(), total);
        write_out(e, fd, data);
        e.set_result(count);
    };
    libc("fwrite", do_fwrite);
    libc("fflush", [](Emulator& e) {
        // The guest's own buffered stdout has to go out too, not just the host's
        // streams: a guest that flushes before writing to another descriptor is
        // ordering its output deliberately, and holding the bytes back reorders
        // it.  fflush(NULL) means every stream.
        int fd = e.host_fd(e.arg_slot(0));
        if (fd == 1 || e.arg_slot(0) == 0) e.flush_guest_output();
        if (fd > 2) e.files.flush(fd);
        std::fflush(stdout);
        std::fflush(stderr);
        e.set_result(0);
    });
    libc("__acrt_iob_func", [](Emulator& e) { e.set_result(e.guest_file(static_cast<int>(e.arg_slot(0)))); });
    libc("__iob_func", [](Emulator& e) { e.set_result(e.guest_file(0)); });
    libc("_iob", [](Emulator& e) { e.set_result(e.guest_file(0)); });

    // ---- floating-point control -------------------------------------------
    // _controlfp speaks the CRT's own bit layout, not the x87 one: translate
    // both ways so a later FSTCW sees what the guest asked for.
    auto controlfp = [](Emulator& e) {
        uint16_t& cw = e.cpu().fpu_control;
        auto to_crt = [](uint16_t x) {
            uint32_t c = 0;
            if (x & 0x01) c |= 0x10;      // _EM_INVALID
            if (x & 0x02) c |= 0x80000;   // _EM_DENORMAL
            if (x & 0x04) c |= 0x08;      // _EM_ZERODIVIDE
            if (x & 0x08) c |= 0x04;      // _EM_OVERFLOW
            if (x & 0x10) c |= 0x02;      // _EM_UNDERFLOW
            if (x & 0x20) c |= 0x01;      // _EM_INEXACT
            c |= ((x >> 10) & 3) << 8;    // _MCW_RC: same order as RC
            static const uint32_t pc[4] = {0x20000, 0x20000, 0x10000, 0};  // 24/-/53/64
            return c | pc[(x >> 8) & 3];
        };
        uint32_t nv = static_cast<uint32_t>(e.arg_slot(0));
        uint32_t mask = static_cast<uint32_t>(e.arg_slot(1));
        uint32_t c = (to_crt(cw) & ~mask) | (nv & mask);
        uint16_t x = cw & ~0x0F3F;
        if (c & 0x10) x |= 0x01;
        if (c & 0x80000) x |= 0x02;
        if (c & 0x08) x |= 0x04;
        if (c & 0x04) x |= 0x08;
        if (c & 0x02) x |= 0x10;
        if (c & 0x01) x |= 0x20;
        x |= ((c >> 8) & 3) << 10;
        uint32_t p = c & 0x30000;
        x |= (p == 0x20000 ? 0 : p == 0x10000 ? 2 : 3) << 8;
        cw = x;
        e.set_result(c);
    };
    libc("_controlfp", controlfp);
    libc("_control87", controlfp);
    libc("_fpreset", [](Emulator& e) { e.cpu().fpu_control = 0x027F; e.set_result(0); });

    // ---- MSVCRT.DLL of the VC6 era -----------------------------------------
    // The x87 helpers take their operands on the FPU stack, not the call stack.
    libc("_ftol", [](Emulator& e) {
        double v = e.cpu().fpu_pop();
        int64_t r = std::isfinite(v) && std::fabs(v) < 9.2e18 ? static_cast<int64_t>(v)
                                                                : INT64_MIN;
        e.cpu().regs[RDX] = static_cast<uint32_t>(static_cast<uint64_t>(r) >> 32);
        e.set_result(static_cast<uint32_t>(r));
    });
    libc("_CIpow", [](Emulator& e) {
        double y = e.cpu().fpu_pop(), x = e.cpu().fpu_pop();
        e.cpu().fpu_push(std::pow(x, y));
    });
    libc("??2@YAPAXI@Z", [](Emulator& e) { e.set_result(e.heap_alloc(e.arg_slot(0) ? e.arg_slot(0) : 1)); });
    libc("??3@YAXPAX@Z", [](Emulator& e) { if (e.arg_slot(0)) e.heap_free(e.arg_slot(0)); });
    libc("??1type_info@@UAE@XZ", [](Emulator& e) {});
    libc("?terminate@@YAXXZ", [](Emulator& e) {
        std::fprintf(stderr, "[guest] terminate()\n");
        e.exit_process(3);
    });
    libc("_XcptFilter", [](Emulator& e) { e.set_result(0); });  // EXCEPTION_CONTINUE_SEARCH
    libc("_itoa", [](Emulator& e) {
        int radix = static_cast<int>(e.arg_slot(2));
        if (radix < 2 || radix > 36) radix = 10;
        int32_t v = static_cast<int32_t>(e.arg_slot(0));
        uint64_t m = radix == 10 && v < 0 ? static_cast<uint64_t>(-static_cast<int64_t>(v))
                                          : static_cast<uint32_t>(v);
        std::string t;
        do {
            int d = static_cast<int>(m % radix);
            t += static_cast<char>(d < 10 ? '0' + d : 'a' + d - 10);
            m /= radix;
        } while (m);
        if (radix == 10 && v < 0) t += '-';
        std::reverse(t.begin(), t.end());
        e.mem.write_cstring(e.arg_slot(1), t);
        e.set_result(e.arg_slot(1));
    });
    libc("_mbsstr", [](Emulator& e) {
        std::string h = e.mem.read_cstring(e.arg_slot(0)), n = e.mem.read_cstring(e.arg_slot(1));
        size_t p = h.find(n);
        e.set_result(p == std::string::npos ? 0 : e.arg_slot(0) + p);
    });
    libc("_isctype", [](Emulator& e) {
        uint64_t table = e.mem.read32(e.data_import("_pctype"));
        uint32_t c = static_cast<uint32_t>(e.arg_slot(0));
        e.set_result(c < 256 ? e.mem.read16(table + c * 2) & e.arg_slot(1) : 0);
    });

    // _except_handler3, the VC6 __try/__except frame handler.  Its frame is
    //   [ebp-18] saved esp  [ebp-14] EXCEPTION_POINTERS*  [ebp-10] next
    //   [ebp-0C] this handler  [ebp-08] scope table  [ebp-04] try level
    // and the filters and __finally blocks are pieces of the function itself,
    // entered with that function's ebp.
    libc("_except_handler3", [](Emulator& e) {
        uint64_t record = e.arg_slot(0), reg = e.arg_slot(1), ctx = e.arg_slot(2);
        uint64_t ebp = reg + 0x10;
        uint64_t table = e.mem.read32(reg + 8);
        auto call_in_frame = [&](uint64_t code) {
            uint64_t saved = e.cpu().regs[RBP];
            e.cpu().regs[RBP] = ebp;
            uint64_t r = e.call_guest(code, {});
            e.cpu().regs[RBP] = saved;
            return static_cast<int32_t>(r);
        };
        auto local_unwind = [&](uint32_t stop) {
            for (int guard = 0; guard < 256; ++guard) {
                uint32_t level = e.mem.read32(reg + 12);
                if (level == 0xFFFFFFFFu || level == stop) break;
                uint64_t ent = table + level * 12ull;
                e.mem.write32(reg + 12, e.mem.read32(ent));
                if (!e.mem.read32(ent + 4)) call_in_frame(e.mem.read32(ent + 8));  // __finally
            }
        };
        if (e.mem.read32(record + 4) & 6) {  // unwinding
            local_unwind(0xFFFFFFFFu);
            return e.set_result(1);
        }
        uint64_t xp = e.exception_scratch(8);
        e.mem.write32(xp, static_cast<uint32_t>(record));
        e.mem.write32(xp + 4, static_cast<uint32_t>(ctx));
        e.mem.write32(reg - 4, static_cast<uint32_t>(xp));
        uint32_t level = e.mem.read32(reg + 12);
        for (int guard = 0; guard < 256 && level != 0xFFFFFFFFu; ++guard) {
            uint64_t ent = table + level * 12ull;
            uint64_t filter = e.mem.read32(ent + 4);
            if (filter) {
                int32_t r = call_in_frame(filter);
                if (r < 0) return e.set_result(0);  // EXCEPTION_CONTINUE_EXECUTION
                if (r > 0) {
                    e.unwind_chain32(reg, record);
                    local_unwind(level);
                    e.mem.write32(reg + 12, e.mem.read32(ent));
                    e.cpu().regs[RBP] = ebp;
                    e.cpu().regs[RSP] = e.mem.read32(reg - 8);
                    e.cpu().rip = e.mem.read32(ent + 8);
                    e.retry_current_call();
                    return;
                }
            }
            level = e.mem.read32(ent);
        }
        e.set_result(1);  // ExceptionContinueSearch
    });

    // ---- process control ---------------------------------------------------
    // exit() runs the atexit handlers and _exit() does not - that is the whole
    // difference between them, and this registration shadows the one in
    // hooks_win32.cpp (add_hook keeps the first), so it must do the job itself.
    libc("exit", [](Emulator& e) {
        e.run_atexit();
        e.exit_process(static_cast<int>(e.arg_slot(0)));
    });
    libc("_exit", [](Emulator& e) { e.exit_process(static_cast<int>(e.arg_slot(0))); });
    libc("abort", [](Emulator& e) {
        std::fprintf(stderr, "[guest] abort()\n");
        e.exit_process(3);
    });
    // atexit must keep the function, not merely succeed: a /MD C++ program's
    // static *destructors* arrive here (the /MT path goes through _crt_atexit),
    // and a hook that returns 0 while dropping the argument cancels every one of
    // them silently.
    libc("atexit", [](Emulator& e) {
        if (uint64_t fn = e.arg_slot(0)) e.add_atexit(fn);
        e.set_result(0);
    });
    // msvcrt's internal CRT locks: one guest thread runs at a time, so there is
    // nothing to take.
    libc("_lock", [](Emulator& e) { e.set_result(0); });
    libc("_unlock", [](Emulator& e) { e.set_result(0); });
    libc("___lc_codepage_func", [](Emulator& e) { e.set_result(1252); });
    libc("___mb_cur_max_func", [](Emulator& e) { e.set_result(1); });
    // (argc*, argv*, env*, doWildCard, startupInfo*) - mingw's msvcrt startup.
    libc("__getmainargs", [](Emulator& e) {
        int ps = e.pointer_size();
        std::vector<uint64_t> ptrs;
        for (const auto& a : e.args()) ptrs.push_back(e.alloc_guest_string(a));
        std::vector<uint8_t> table((ptrs.size() + 1) * ps, 0);
        uint64_t argv = e.alloc_guest_data(table.data(), table.size());
        for (size_t i = 0; i < ptrs.size(); ++i)
            e.mem.write_sized(argv + i * ps, ps, ptrs[i]);
        if (e.arg_slot(0)) e.mem.write32(e.arg_slot(0), static_cast<uint32_t>(e.args().size()));
        if (e.arg_slot(1)) e.mem.write_sized(e.arg_slot(1), ps, argv);
        if (e.arg_slot(2)) e.mem.write_sized(e.arg_slot(2), ps, e.environment_vector());
        e.set_result(0);
    });
    // _initterm deliberately has no stub here: the real implementation in
    // hooks_win32.cpp walks the table and calls the initialisers, and a no-op
    // registered first would shadow it - mingw's startup sets argv inside one
    // of those initialisers, so the no-op cost every guest its argument list.
    libc("__set_app_type", [](Emulator& e) { e.set_result(0); });
    libc("_amsg_exit", [](Emulator& e) { e.exit_process(static_cast<int>(e.arg_slot(0))); });
    libc("signal", [](Emulator& e) { e.set_result(0); });

    // ---- memory ------------------------------------------------------------
    libc("malloc", [](Emulator& e) { e.set_result(e.heap_alloc(e.arg_slot(0))); });
    libc("calloc", [](Emulator& e) {
        uint64_t total = e.arg_slot(0) * e.arg_slot(1);
        uint64_t p = e.heap_alloc(total);
        if (p) {
            std::vector<uint8_t> zeros(static_cast<size_t>(total), 0);
            if (total) e.mem.write(p, zeros.data(), total);
        }
        e.set_result(p);
    });
    libc("realloc", [](Emulator& e) { e.set_result(e.heap_realloc(e.arg_slot(0), e.arg_slot(1))); });
    libc("free", [](Emulator& e) {
        e.heap_free(e.arg_slot(0));
        e.set_result(0);
    });

    libc("memset", [](Emulator& e) {
        uint64_t p = e.arg_slot(0);
        uint8_t v = static_cast<uint8_t>(e.arg_slot(1));
        uint64_t n = e.arg_slot(2);
        std::vector<uint8_t> block(static_cast<size_t>(n), v);
        if (n) e.mem.write(p, block.data(), n);
        e.set_result(p);
    });
    auto copy_mem = [](Emulator& e) {
        uint64_t dst = e.arg_slot(0), src = e.arg_slot(1), n = e.arg_slot(2);
        if (n) {
            std::vector<uint8_t> tmp(static_cast<size_t>(n));
            e.mem.read(src, tmp.data(), n);
            e.mem.write(dst, tmp.data(), n);
        }
        e.set_result(dst);
    };
    libc("memcpy", copy_mem);
    libc("memmove", copy_mem);  // the temporary buffer already makes it safe
    libc("memcmp", [](Emulator& e) {
        uint64_t a = e.arg_slot(0), b = e.arg_slot(1), n = e.arg_slot(2);
        int result = 0;
        for (uint64_t i = 0; i < n && result == 0; ++i) {
            int x = e.mem.read8(a + i), y = e.mem.read8(b + i);
            result = x - y;
        }
        e.set_result(static_cast<uint64_t>(static_cast<int64_t>(result)));
    });

    // ---- strings -----------------------------------------------------------
    libc("strlen", [](Emulator& e) { e.set_result(e.mem.read_cstring(e.arg_slot(0)).size()); });
    // strnlen must not read past `max`, which is the whole reason a caller uses
    // it: the buffer may not be terminated at all.
    auto bounded_strlen = [](Emulator& e) {
        uint64_t p = e.arg_slot(0), max = e.arg_slot(1), n = 0;
        while (n < max && e.mem.read8(p + n) != 0) ++n;
        e.set_result(n);
    };
    libc("strnlen", bounded_strlen);
    libc("strnlen_s", bounded_strlen);
    libc("strcpy", [](Emulator& e) {
        uint64_t dst = e.arg_slot(0);
        e.mem.write_cstring(dst, e.mem.read_cstring(e.arg_slot(1)));
        e.set_result(dst);
    });
    libc("strncpy", [](Emulator& e) {
        uint64_t dst = e.arg_slot(0), n = e.arg_slot(2);
        std::string s = e.mem.read_cstring(e.arg_slot(1));
        for (uint64_t i = 0; i < n; ++i)
            e.mem.write8(dst + i, i < s.size() ? static_cast<uint8_t>(s[i]) : 0);
        e.set_result(dst);
    });
    libc("strcat", [](Emulator& e) {
        uint64_t dst = e.arg_slot(0);
        std::string a = e.mem.read_cstring(dst), b = e.mem.read_cstring(e.arg_slot(1));
        e.mem.write_cstring(dst, a + b);
        e.set_result(dst);
    });
    libc("strcmp", [](Emulator& e) {
        std::string a = e.mem.read_cstring(e.arg_slot(0)), b = e.mem.read_cstring(e.arg_slot(1));
        int r = a.compare(b);
        e.set_result(static_cast<uint64_t>(static_cast<int64_t>(r < 0 ? -1 : (r > 0 ? 1 : 0))));
    });
    libc("strncmp", [](Emulator& e) {
        uint64_t n = e.arg_slot(2);
        std::string a = e.mem.read_cstring(e.arg_slot(0)), b = e.mem.read_cstring(e.arg_slot(1));
        int r = std::strncmp(a.c_str(), b.c_str(), static_cast<size_t>(n));
        e.set_result(static_cast<uint64_t>(static_cast<int64_t>(r < 0 ? -1 : (r > 0 ? 1 : 0))));
    });
    libc("strchr", [](Emulator& e) {
        uint64_t p = e.arg_slot(0);
        char c = static_cast<char>(e.arg_slot(1));
        std::string s = e.mem.read_cstring(p);
        // Searching for the terminator is defined to find it, not to fail: it is
        // how a caller asks "where does this string end?".  Returning NULL here
        // is a real difference in behaviour - CPython's tokeniser uses exactly
        // this idiom to find the end of a line of source.
        if (c == '\0') {
            e.set_result(p + s.size());
            return;
        }
        size_t pos = s.find(c);
        e.set_result(pos == std::string::npos ? 0 : p + pos);
    });
    libc("strstr", [](Emulator& e) {
        uint64_t p = e.arg_slot(0);
        std::string s = e.mem.read_cstring(p), t = e.mem.read_cstring(e.arg_slot(1));
        size_t pos = s.find(t);
        e.set_result(pos == std::string::npos ? 0 : p + pos);
    });
    libc("atoi", [](Emulator& e) {
        e.set_result(static_cast<uint64_t>(
            static_cast<int64_t>(std::atoi(e.mem.read_cstring(e.arg_slot(0)).c_str()))));
    });

    // ---- environment ---------------------------------------------------------
    libc("getenv", [](Emulator& e) {
        const std::string* v = e.getenv(e.mem.read_cstring(e.arg_slot(0)));
        e.set_result(v ? e.alloc_guest_string(*v) : 0);
    });
    libc("_putenv", [](Emulator& e) {
        // The argument is a single "NAME=VALUE" string; no '=' means remove.
        std::string entry = e.mem.read_cstring(e.arg_slot(0));
        size_t eq = entry.find('=');
        if (eq == std::string::npos)
            e.unsetenv(entry);
        else if (eq + 1 == entry.size())
            e.unsetenv(entry.substr(0, eq));
        else
            e.setenv(entry.substr(0, eq), entry.substr(eq + 1));
        e.set_result(0);
    });
    libc("setenv", [](Emulator& e) {
        std::string name = e.mem.read_cstring(e.arg_slot(0));
        std::string value = e.mem.read_cstring(e.arg_slot(1));
        bool overwrite = e.arg_slot(2) != 0;
        if (overwrite || !e.getenv(name)) e.setenv(name, value);
        e.set_result(0);
    });
    libc("unsetenv", [](Emulator& e) {
        e.unsetenv(e.mem.read_cstring(e.arg_slot(0)));
        e.set_result(0);
    });

    // ---- misc runtime ------------------------------------------------------
    libc("time", [](Emulator& e) {
        auto now = static_cast<uint64_t>(std::time(nullptr));
        uint64_t out = e.arg_slot(0);
        if (out) e.mem.write_sized(out, e.is64() ? 8 : 4, now);
        e.set_result(now);
    });
    libc("clock", [](Emulator& e) { e.set_result(e.cpu().instructions_executed / 1000); });
    libc("rand", [](Emulator& e) {
        // A fixed LCG keeps guest output reproducible across runs.
        static uint32_t state = 1;
        state = state * 1103515245u + 12345u;
        e.set_result((state >> 16) & 0x7FFF);
    });
    libc("srand", [](Emulator& e) { e.set_result(0); });

}

}  // namespace x86emu
