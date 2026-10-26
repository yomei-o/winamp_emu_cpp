// The "A" entry points, for DLLs built against the ANSI MFC (Jw_cad's
// common_lib.dll is one).  Strings are code page 932; each function converts
// what it takes, calls the W implementation through its hook address, and
// converts what comes back.
//
// What is left out is OLE embedding, metafiles and printing, whose W
// counterparts are not here either: calling one of those still stops with
// the import named.
#include <algorithm>
#include <cstring>

#include "guest_printf.h"
#include "hookutil.h"
#include "user_internal.h"

namespace x86emu::win32 {

namespace {

// A UTF-16 copy of a CP932 string in guest memory, freed with the scope.
struct WideArg {
    Emulator& e;
    uint64_t addr = 0;
    WideArg(Emulator& em, uint64_t a, int len = -1) : e(em) {
        if (a < 0x10000) {
            addr = a;  // NULL or an integer atom / resource id: pass through
            return;
        }
        std::string s;
        if (len < 0) s = e.mem.read_cstring(a);
        else {
            s.resize(static_cast<size_t>(len));
            if (len) e.mem.read(a, s.data(), s.size());
        }
        std::u16string w = cp932_to_utf16(s);
        addr = e.heap_alloc((w.size() + 1) * 2);
        write_wstr(e, addr, w, static_cast<int>(w.size() + 1));
        owned = true;
    }
    ~WideArg() {
        if (owned) e.heap_free(addr);
    }
    bool owned = false;
};

// A wide output buffer of `units`, copied back to an ANSI buffer of `cap` bytes.
struct WideOut {
    Emulator& e;
    uint64_t addr = 0;
    int units;
    WideOut(Emulator& em, int u) : e(em), units(std::max(u, 1)) {
        addr = e.heap_alloc(static_cast<size_t>(units) * 2 + 2);
        e.mem.write16(addr, 0);
    }
    ~WideOut() { e.heap_free(addr); }
    std::string text() const { return utf16_to_cp932(read_wstr(e, addr, units)); }
    // Writes the ANSI text (NUL-terminated, truncated to cap); returns bytes without NUL.
    int store(uint64_t dst, int cap) const {
        std::string a = text();
        if (!dst || cap <= 0) return static_cast<int>(a.size());
        int n = std::min(static_cast<int>(a.size()), cap - 1);
        if (n) e.mem.write(dst, a.data(), static_cast<uint64_t>(n));
        e.mem.write8(dst + n, 0);
        return n;
    }
};

uint64_t call_w(Emulator& e, const char* wname, std::vector<uint64_t> args) {
    uint64_t h = e.existing_hook(wname);
    if (!h) throw std::runtime_error(std::string("ansi forward: no hook for ") + wname);
    return e.call_guest(h, args) & 0xFFFFFFFFull;
}

// Forwards with the listed argument positions converted as input strings.
void forward(Emulator& e, const char* aname, const char* wname, int nargs, std::vector<int> strings) {
    std::string w = wname;
    api(e, aname, nargs, [w, nargs, strings](Emulator& e) {
        std::vector<uint64_t> args;
        std::vector<std::unique_ptr<WideArg>> keep;
        for (int i = 0; i < nargs; ++i) {
            uint64_t v = A(e, i);
            if (std::find(strings.begin(), strings.end(), i) != strings.end()) {
                keep.push_back(std::make_unique<WideArg>(e, v));
                v = keep.back()->addr;
            }
            args.push_back(v);
        }
        ret(e, call_w(e, w.c_str(), args));
    });
}

// LOGFONTA (60 bytes) <-> LOGFONTW (92 bytes).
void logfont_a_to_w(Emulator& e, uint64_t a, uint64_t w) {
    for (int i = 0; i < 28; i += 4) e.mem.write32(w + i, e.mem.read32(a + i));
    std::string face(32, '\0');
    e.mem.read(a + 28, face.data(), 32);
    face = face.c_str();
    write_wstr(e, w + 28, cp932_to_utf16(face), 32);
}
void logfont_w_to_a(Emulator& e, uint64_t w, uint64_t a) {
    for (int i = 0; i < 28; i += 4) e.mem.write32(a + i, e.mem.read32(w + i));
    std::string face = utf16_to_cp932(read_wstr(e, w + 28, 32));
    face.resize(31);
    e.mem.write(a + 28, face.c_str(), std::min<size_t>(face.size() + 1, 32));
    e.mem.write8(a + 59, 0);
}

// MENUITEMINFOA <-> MENUITEMINFOW: same layout, the string differs.
uint64_t menuinfo_a_to_w(Emulator& e, uint64_t a, std::unique_ptr<WideArg>& keep) {
    uint64_t w = e.heap_alloc(48);
    for (int i = 0; i < 48; i += 4) e.mem.write32(w + i, e.mem.read32(a + i));
    uint32_t mask = e.mem.read32(a + 4), type = e.mem.read32(a + 8);
    bool str = (mask & 0x40) || ((mask & 0x10) && !(type & (0x800 | 0x4 | 0x100)));
    if (str && e.mem.read32(a + 36)) {
        keep = std::make_unique<WideArg>(e, e.mem.read32(a + 36));
        e.mem.write32(w + 36, static_cast<uint32_t>(keep->addr));
    }
    return w;
}

}  // namespace

void install_ansi(Emulator& e) {
    // ---- advapi32 -----------------------------------------------------------------
    forward(e, "RegDeleteKeyA", "RegDeleteKeyW", 2, {1});
    forward(e, "RegDeleteValueA", "RegDeleteValueW", 2, {1});
    forward(e, "RegSetValueA", "RegSetValueW", 5, {1, 3});
    api(e, "RegQueryValueA", 4, [](Emulator& e) {
        WideArg sub(e, A(e, 1));
        uint32_t cap = A(e, 3) ? e.mem.read32(A(e, 3)) : 0;
        uint64_t lenw = e.heap_alloc(4);
        e.mem.write32(lenw, 2048);
        WideOut out(e, 1024);
        uint64_t r = call_w(e, "RegQueryValueW", {A(e, 0), sub.addr, out.addr, lenw});
        e.heap_free(lenw);
        if (r) return ret(e, r);
        std::string a = out.text();
        if (A(e, 3)) e.mem.write32(A(e, 3), static_cast<uint32_t>(a.size() + 1));
        if (A(e, 2) && cap > a.size()) e.mem.write_cstring(A(e, 2), a);
        ret(e, A(e, 2) && cap <= a.size() ? 234 : 0);
    });
    api(e, "RegEnumKeyA", 4, [](Emulator& e) {
        WideOut out(e, 260);
        uint64_t r = call_w(e, "RegEnumKeyW", {A(e, 0), A(e, 1), out.addr, 260});
        if (!r) out.store(A(e, 2), I(e, 3));
        ret(e, r);
    });
    api(e, "RegEnumKeyExA", 8, [](Emulator& e) {
        WideOut out(e, 260);
        uint64_t len = e.heap_alloc(4);
        e.mem.write32(len, 260);
        uint64_t r = call_w(e, "RegEnumKeyExW", {A(e, 0), A(e, 1), out.addr, len, 0, 0, 0, A(e, 7)});
        e.heap_free(len);
        if (!r) {
            int n = out.store(A(e, 2), static_cast<int>(e.mem.read32(A(e, 3))));
            e.mem.write32(A(e, 3), static_cast<uint32_t>(n));
        }
        ret(e, r);
    });
    api(e, "RegEnumValueA", 8, [](Emulator& e) {
        WideOut out(e, 260);
        uint64_t len = e.heap_alloc(4);
        e.mem.write32(len, 260);
        uint64_t r = call_w(e, "RegEnumValueW", {A(e, 0), A(e, 1), out.addr, len, 0, A(e, 5), A(e, 6), A(e, 7)});
        e.heap_free(len);
        if (!r) {
            int n = out.store(A(e, 2), static_cast<int>(e.mem.read32(A(e, 3))));
            e.mem.write32(A(e, 3), static_cast<uint32_t>(n));
        }
        ret(e, r);
    });
    api(e, "GetFileSecurityA", 5, [](Emulator& e) {
        if (A(e, 4)) e.mem.write32(A(e, 4), 0);
        e.set_last_error(50);
        ret(e, 0);
    });
    api(e, "SetFileSecurityA", 3, [](Emulator& e) { ret(e, 1); });

    // ---- gdi32 ----------------------------------------------------------------------
    forward(e, "CreateDCA", "CreateDCW", 4, {0, 1, 2});
    api(e, "CreateFontIndirectA", 1, [](Emulator& e) {
        uint64_t w = e.heap_alloc(92);
        logfont_a_to_w(e, A(e, 0), w);
        uint64_t r = call_w(e, "CreateFontIndirectW", {w});
        e.heap_free(w);
        ret(e, r);
    });
    forward(e, "CreateFontA", "CreateFontW", 14, {13});
    api(e, "GetObjectA", 3, [](Emulator& e) {
        Gui& g = gui(e);
        GdiObject* o = g.obj(A(e, 0));
        if (o && o->kind == ObjKind::Font) {
            if (!A(e, 2)) return ret(e, 60);
            uint64_t w = e.heap_alloc(92);
            call_w(e, "GetObjectW", {A(e, 0), 92, w});
            uint64_t a = e.heap_alloc(60);
            logfont_w_to_a(e, w, a);
            int n = std::min(I(e, 1), 60);
            std::vector<uint8_t> tmp(static_cast<size_t>(n));
            e.mem.read(a, tmp.data(), tmp.size());
            e.mem.write(A(e, 2), tmp.data(), tmp.size());
            e.heap_free(w);
            e.heap_free(a);
            return ret(e, static_cast<uint64_t>(n));
        }
        ret(e, call_w(e, "GetObjectW", {A(e, 0), A(e, 1), A(e, 2)}));
    });
    api(e, "GetTextMetricsA", 2, [](Emulator& e) {
        // TEXTMETRICA: the four character fields are bytes, the rest the same.
        uint64_t w = e.heap_alloc(60);
        uint64_t r = call_w(e, "GetTextMetricsW", {A(e, 0), w});
        uint64_t a = A(e, 1);
        for (int i = 0; i < 44; i += 4) e.mem.write32(a + i, e.mem.read32(w + i));
        e.mem.write8(a + 44, 0x20);
        e.mem.write8(a + 45, 0xFC);
        e.mem.write8(a + 46, 0x1F);
        e.mem.write8(a + 47, 0x20);
        for (int i = 0; i < 5; ++i) e.mem.write8(a + 48 + i, e.mem.read8(w + 52 + i));
        e.heap_free(w);
        ret(e, r);
    });
    api(e, "GetTextFaceA", 3, [](Emulator& e) {
        WideOut out(e, 64);
        call_w(e, "GetTextFaceW", {A(e, 0), 64, out.addr});
        if (!A(e, 2)) return ret(e, out.text().size() + 1);
        ret(e, static_cast<uint64_t>(out.store(A(e, 2), I(e, 1))));
    });
    api(e, "ExtTextOutA", 8, [](Emulator& e) {
        // The dx array is per byte in the ANSI call; fold it per character.
        std::string a(static_cast<size_t>(std::max(0, I(e, 6))), '\0');
        if (!a.empty()) e.mem.read(A(e, 5), a.data(), a.size());
        std::u16string w = cp932_to_utf16(a);
        uint64_t s = e.heap_alloc((w.size() + 1) * 2);
        write_wstr(e, s, w, static_cast<int>(w.size() + 1));
        uint64_t dx = 0;
        if (A(e, 7) && !w.empty()) {
            dx = e.heap_alloc(w.size() * 4);
            size_t bi = 0;
            for (size_t ci = 0; ci < w.size(); ++ci) {
                unsigned char c = static_cast<unsigned char>(a[bi]);
                bool lead = (c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC);
                int v = static_cast<int32_t>(e.mem.read32(A(e, 7) + bi * 4));
                if (lead && bi + 1 < a.size()) {
                    v += static_cast<int32_t>(e.mem.read32(A(e, 7) + (bi + 1) * 4));
                    bi += 2;
                } else {
                    bi += 1;
                }
                e.mem.write32(dx + ci * 4, static_cast<uint32_t>(v));
            }
        }
        uint64_t r = call_w(e, "ExtTextOutW", {A(e, 0), A(e, 1), A(e, 2), A(e, 3), A(e, 4), s, w.size(), dx});
        e.heap_free(s);
        if (dx) e.heap_free(dx);
        ret(e, r);
    });
    api(e, "GetTextExtentPointA", 4, [](Emulator& e) {
        WideArg s(e, A(e, 1), I(e, 2));
        uint64_t n = s.addr >= 0x10000 ? read_wstr(e, s.addr).size() : 0;
        ret(e, call_w(e, "GetTextExtentPoint32W", {A(e, 0), s.addr, n, A(e, 3)}));
    });
    api(e, "GetCharWidthA", 4, [](Emulator& e) { ret(e, call_w(e, "GetCharWidthW", {A(e, 0), A(e, 1), A(e, 2), A(e, 3)})); });
    api(e, "EnumFontFamiliesA", 4, [](Emulator& e) { ret(e, 1); });
    api(e, "EnumFontFamiliesExA", 5, [](Emulator& e) { ret(e, 1); });
    api(e, "SetMapperFlags", 2, [](Emulator& e) { ret(e, 0); });
    api(e, "SetTextJustification", 3, [](Emulator& e) { ret(e, 1); });
    api(e, "SetColorAdjustment", 2, [](Emulator& e) { ret(e, 1); });
    api(e, "SelectClipPath", 2, [](Emulator& e) { ret(e, 0); });

    // ---- kernel32 ---------------------------------------------------------------------
    forward(e, "CopyFileA", "CopyFileW", 3, {0, 1});
    forward(e, "SetCurrentDirectoryA", "SetCurrentDirectoryW", 1, {0});
    // The Struct pair stores bytes as hex followed by a one-byte checksum
    // (the sum of the bytes), which is how a reader tells a stale size apart.
    api(e, "WritePrivateProfileStructA", 5, [](Emulator& e) {
        WideArg sec(e, A(e, 0)), key(e, A(e, 1)), file(e, A(e, 4));
        if (!A(e, 2)) return ret(e, call_w(e, "WritePrivateProfileStringW", {sec.addr, key.addr, 0, file.addr}));
        static const char* hex = "0123456789ABCDEF";
        std::string text;
        uint8_t sum = 0;
        for (uint32_t i = 0; i < U(e, 3); ++i) {
            uint8_t b = e.mem.read8(A(e, 2) + i);
            sum = static_cast<uint8_t>(sum + b);
            text += hex[b >> 4];
            text += hex[b & 15];
        }
        text += hex[sum >> 4];
        text += hex[sum & 15];
        uint64_t s = e.alloc_guest_string(text);
        WideArg val(e, s);
        ret(e, call_w(e, "WritePrivateProfileStringW", {sec.addr, key.addr, val.addr, file.addr}));
    });
    api(e, "GetPrivateProfileStructA", 5, [](Emulator& e) {
        WideArg sec(e, A(e, 0)), key(e, A(e, 1)), file(e, A(e, 4));
        uint32_t size = U(e, 3);
        int cap = static_cast<int>(size * 2 + 8);
        WideOut out(e, cap);
        uint64_t empty = e.alloc_guest_string("");
        WideArg def(e, empty);
        uint64_t n = call_w(e, "GetPrivateProfileStringW", {sec.addr, key.addr, def.addr, out.addr, static_cast<uint64_t>(cap), file.addr});
        std::u16string t = read_wstr(e, out.addr);
        if (n != size * 2 + 2 || t.size() != size * 2 + 2) return ret(e, 0);
        auto nib = [](char16_t c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return -1;
        };
        std::vector<uint8_t> bytes;
        uint8_t sum = 0;
        for (size_t i = 0; i < t.size(); i += 2) {
            int h = nib(t[i]), l = nib(t[i + 1]);
            if (h < 0 || l < 0) return ret(e, 0);
            bytes.push_back(static_cast<uint8_t>(h * 16 + l));
        }
        for (uint32_t i = 0; i < size; ++i) sum = static_cast<uint8_t>(sum + bytes[i]);
        if (sum != bytes[size]) return ret(e, 0);
        if (size) e.mem.write(A(e, 2), bytes.data(), size);
        ret(e, 1);
    });
    api(e, "SetPriorityClass", 2, [](Emulator& e) { ret(e, 1); });
    forward(e, "GetDiskFreeSpaceA", "GetDiskFreeSpaceW", 5, {0});
    forward(e, "GetFileAttributesExA", "GetFileAttributesExW", 3, {0});
    forward(e, "SetFileAttributesA", "SetFileAttributesW", 2, {0});
    forward(e, "MoveFileA", "MoveFileW", 2, {0, 1});
    forward(e, "ReplaceFileA", "ReplaceFileW", 6, {0, 1, 2});
    forward(e, "GlobalFindAtomA", "GlobalFindAtomW", 1, {0});
    forward(e, "GetPrivateProfileIntA", "GetPrivateProfileIntW", 4, {0, 1, 3});
    forward(e, "GetProfileIntA", "GetProfileIntW", 3, {0, 1});
    forward(e, "WritePrivateProfileStringA", "WritePrivateProfileStringW", 4, {0, 1, 2, 3});
    api(e, "GetPrivateProfileStringA", 6, [](Emulator& e) {
        WideArg sec(e, A(e, 0)), key(e, A(e, 1)), def(e, A(e, 2)), file(e, A(e, 5));
        int cap = I(e, 4);
        WideOut out(e, cap > 0 ? cap : 1);
        uint64_t r = call_w(e, "GetPrivateProfileStringW", {sec.addr, key.addr, def.addr, out.addr, static_cast<uint64_t>(cap), file.addr});
        if (!A(e, 3) || cap <= 0) return ret(e, 0);
        if (!A(e, 0) || !A(e, 1)) {
            // A double-NUL list: convert entry by entry.
            std::string all;
            uint64_t p = out.addr;
            for (uint64_t k = 0; k < r;) {
                std::u16string one = read_wstr(e, p + k * 2);
                all += utf16_to_cp932(one);
                all.push_back('\0');
                k += one.size() + 1;
            }
            all.push_back('\0');
            size_t n = std::min(all.size(), static_cast<size_t>(cap));
            e.mem.write(A(e, 3), all.data(), n);
            return ret(e, n >= 2 ? n - 2 : 0);
        }
        ret(e, static_cast<uint64_t>(out.store(A(e, 3), cap)));
    });
    api(e, "GetAtomNameA", 3, [](Emulator& e) {
        WideOut out(e, 256);
        call_w(e, "GetAtomNameW", {A(e, 0), out.addr, 256});
        ret(e, static_cast<uint64_t>(out.store(A(e, 1), I(e, 2))));
    });
    api(e, "GlobalGetAtomNameA", 3, [](Emulator& e) {
        WideOut out(e, 256);
        call_w(e, "GlobalGetAtomNameW", {A(e, 0), out.addr, 256});
        ret(e, static_cast<uint64_t>(out.store(A(e, 1), I(e, 2))));
    });
    api(e, "GetShortPathNameA", 3, [](Emulator& e) {
        std::string s = e.mem.read_cstring(A(e, 0));
        if (A(e, 1) && U(e, 2) > s.size()) e.mem.write_cstring(A(e, 1), s);
        ret(e, s.size() + (U(e, 2) > s.size() ? 0 : 1));
    });
    api(e, "GetTempFileNameA", 4, [](Emulator& e) {
        WideArg dir(e, A(e, 0)), pre(e, A(e, 1));
        WideOut out(e, 260);
        uint64_t r = call_w(e, "GetTempFileNameW", {dir.addr, pre.addr, A(e, 2), out.addr});
        out.store(A(e, 3), 260);
        ret(e, r);
    });
    api(e, "GetVolumeInformationA", 8, [](Emulator& e) {
        WideArg root(e, A(e, 0));
        WideOut name(e, 64), fs(e, 64);
        uint64_t r = call_w(e, "GetVolumeInformationW", {root.addr, A(e, 1) ? name.addr : 0, 64, A(e, 3), A(e, 4), A(e, 5), A(e, 6) ? fs.addr : 0, 64});
        if (A(e, 1)) name.store(A(e, 1), I(e, 2));
        if (A(e, 6)) fs.store(A(e, 6), I(e, 7));
        ret(e, r);
    });
    api(e, "SearchPathA", 6, [](Emulator& e) {
        WideArg path(e, A(e, 0)), file(e, A(e, 1)), ext(e, A(e, 2));
        WideOut out(e, 260);
        uint64_t r = call_w(e, "SearchPathW", {path.addr, file.addr, ext.addr, 260, out.addr, 0});
        if (!r) return ret(e, 0);
        int n = out.store(A(e, 4), I(e, 3));
        if (A(e, 5)) {
            std::string s = out.text();
            size_t sep = s.find_last_of("\\/");
            e.mem.write32(A(e, 5), static_cast<uint32_t>(A(e, 4) + (sep == std::string::npos ? 0 : sep + 1)));
        }
        ret(e, static_cast<uint64_t>(n));
    });
    api(e, "GetStringTypeExA", 5, [](Emulator& e) {
        int n = I(e, 3);
        std::string s = n < 0 ? e.mem.read_cstring(A(e, 2)) : std::string(static_cast<size_t>(n), '\0');
        if (n > 0) e.mem.read(A(e, 2), s.data(), s.size());
        if (n < 0) n = static_cast<int>(s.size()) + 1;
        for (int i = 0; i < n; ++i) {
            unsigned char c = i < static_cast<int>(s.size()) ? static_cast<unsigned char>(s[i]) : 0;
            uint16_t t = 0;
            if (U(e, 1) == 1) {
                if (c >= 'A' && c <= 'Z') t = 0x101;
                else if (c >= 'a' && c <= 'z') t = 0x102;
                else if (c >= '0' && c <= '9') t = 0x84;
                else if (c == ' ' || (c >= 9 && c <= 13)) t = c == ' ' ? 0x48 : 0x8;
                else if (c < 0x20 || c == 0x7F) t = 0x20;
                else if (c < 0x80) t = 0x10;
            }
            e.mem.write16(A(e, 4) + i * 2, t);
        }
        ret(e, 1);
    });
    api(e, "VerifyVersionInfoA", 4, [](Emulator& e) { ret(e, 1); });
    api(e, "VerifyVersionInfoW", 4, [](Emulator& e) { ret(e, 1); });
    api(e, "lstrcpynA", 3, [](Emulator& e) {
        int n = I(e, 2);
        if (n <= 0 || !A(e, 0)) return ret(e, A(e, 0));
        std::string s = e.mem.read_cstring(A(e, 1));
        if (static_cast<int>(s.size()) > n - 1) s.resize(static_cast<size_t>(n - 1));
        e.mem.write_cstring(A(e, 0), s);
        ret(e, A(e, 0));
    });

    // ---- user32 ---------------------------------------------------------------------------
    api(e, "AppendMenuA", 4, [](Emulator& e) {
        bool str = !(U(e, 1) & (0x800 | 0x4 | 0x100));
        WideArg s(e, str ? A(e, 3) : 0);
        ret(e, call_w(e, "AppendMenuW", {A(e, 0), A(e, 1), A(e, 2), str ? s.addr : A(e, 3)}));
    });
    api(e, "InsertMenuA", 5, [](Emulator& e) {
        bool str = !(U(e, 2) & (0x800 | 0x4 | 0x100));
        WideArg s(e, str ? A(e, 4) : 0);
        ret(e, call_w(e, "InsertMenuW", {A(e, 0), A(e, 1), A(e, 2), A(e, 3), str ? s.addr : A(e, 4)}));
    });
    api(e, "ModifyMenuA", 5, [](Emulator& e) {
        bool str = !(U(e, 2) & (0x800 | 0x4 | 0x100));
        WideArg s(e, str ? A(e, 4) : 0);
        ret(e, call_w(e, "ModifyMenuW", {A(e, 0), A(e, 1), A(e, 2), A(e, 3), str ? s.addr : A(e, 4)}));
    });
    api(e, "InsertMenuItemA", 4, [](Emulator& e) {
        std::unique_ptr<WideArg> keep;
        uint64_t w = menuinfo_a_to_w(e, A(e, 3), keep);
        uint64_t r = call_w(e, "InsertMenuItemW", {A(e, 0), A(e, 1), A(e, 2), w});
        e.heap_free(w);
        ret(e, r);
    });
    api(e, "SetMenuItemInfoA", 4, [](Emulator& e) {
        std::unique_ptr<WideArg> keep;
        uint64_t w = menuinfo_a_to_w(e, A(e, 3), keep);
        uint64_t r = call_w(e, "SetMenuItemInfoW", {A(e, 0), A(e, 1), A(e, 2), w});
        e.heap_free(w);
        ret(e, r);
    });
    api(e, "GetMenuItemInfoA", 4, [](Emulator& e) {
        uint64_t a = A(e, 3);
        uint64_t w = e.heap_alloc(48);
        for (int i = 0; i < 48; i += 4) e.mem.write32(w + i, e.mem.read32(a + i));
        uint64_t abuf = e.mem.read32(a + 36);
        uint32_t cch = e.mem.read32(a + 40);
        WideOut out(e, static_cast<int>(cch ? cch : 1));
        if (abuf) e.mem.write32(w + 36, static_cast<uint32_t>(out.addr));
        uint64_t r = call_w(e, "GetMenuItemInfoW", {A(e, 0), A(e, 1), A(e, 2), w});
        for (int i = 0; i < 48; i += 4)
            if (i != 36) e.mem.write32(a + i, e.mem.read32(w + i));
        if (abuf) {
            int n = out.store(abuf, static_cast<int>(cch));
            e.mem.write32(a + 40, static_cast<uint32_t>(n));
        }
        e.heap_free(w);
        ret(e, r);
    });
    api(e, "GetMenuStringA", 5, [](Emulator& e) {
        WideOut out(e, 256);
        uint64_t r = call_w(e, "GetMenuStringW", {A(e, 0), A(e, 1), out.addr, 256, A(e, 4)});
        if (!A(e, 2)) return ret(e, out.text().size());
        (void)r;
        ret(e, static_cast<uint64_t>(out.store(A(e, 2), I(e, 3))));
    });
    api(e, "GetDlgItemTextA", 4, [](Emulator& e) {
        int cap = I(e, 3);
        WideOut out(e, cap > 0 ? cap : 1);
        call_w(e, "GetDlgItemTextW", {A(e, 0), A(e, 1), out.addr, static_cast<uint64_t>(cap > 0 ? cap : 1)});
        ret(e, static_cast<uint64_t>(out.store(A(e, 2), cap)));
    });
    api(e, "GetKeyNameTextA", 3, [](Emulator& e) {
        WideOut out(e, 64);
        call_w(e, "GetKeyNameTextW", {A(e, 0), out.addr, 64});
        ret(e, static_cast<uint64_t>(out.store(A(e, 1), I(e, 2))));
    });
    api(e, "DrawTextExA", 6, [](Emulator& e) {
        WideArg s(e, A(e, 1), I(e, 2));
        uint64_t n = s.addr >= 0x10000 ? read_wstr(e, s.addr).size() : 0;
        ret(e, call_w(e, "DrawTextExW", {A(e, 0), s.addr, n, A(e, 3), A(e, 4), A(e, 5)}));
    });
    api(e, "DrawStateA", 10, [](Emulator& e) {
        uint32_t type = U(e, 9) & 0xF;
        bool str = type == 1 || type == 2;
        WideArg s(e, str ? A(e, 3) : 0, str && U(e, 4) ? static_cast<int>(U(e, 4)) : -1);
        uint64_t n = str && s.addr >= 0x10000 ? read_wstr(e, s.addr).size() : A(e, 4);
        ret(e, call_w(e, "DrawStateW", {A(e, 0), A(e, 1), A(e, 2), str ? s.addr : A(e, 3), n, A(e, 5), A(e, 6), A(e, 7), A(e, 8), A(e, 9)}));
    });
    api(e, "GrayStringA", 9, [](Emulator& e) {
        WideArg s(e, A(e, 2) ? 0 : A(e, 3), I(e, 4) > 0 ? I(e, 4) : -1);
        uint64_t n = !A(e, 2) && s.addr >= 0x10000 ? read_wstr(e, s.addr).size() : A(e, 4);
        ret(e, call_w(e, "GrayStringW", {A(e, 0), A(e, 1), A(e, 2), A(e, 2) ? A(e, 3) : s.addr, n, A(e, 5), A(e, 6), A(e, 7), A(e, 8)}));
    });
    api(e, "TabbedTextOutA", 8, [](Emulator& e) {
        WideArg s(e, A(e, 3), I(e, 4));
        uint64_t n = s.addr >= 0x10000 ? read_wstr(e, s.addr).size() : 0;
        ret(e, call_w(e, "TabbedTextOutW", {A(e, 0), A(e, 1), A(e, 2), s.addr, n, A(e, 5), A(e, 6), A(e, 7)}));
    });
    api(e, "GetTabbedTextExtentA", 5, [](Emulator& e) {
        WideArg s(e, A(e, 1), I(e, 2));
        uint64_t n = s.addr >= 0x10000 ? read_wstr(e, s.addr).size() : 0;
        ret(e, call_w(e, "GetTabbedTextExtentW", {A(e, 0), s.addr, n, A(e, 3), A(e, 4)}));
    });
    api(e, "CharUpperA", 1, [](Emulator& e) {
        uint64_t p = A(e, 0);
        if (p < 0x10000) {
            uint32_t c = static_cast<uint32_t>(p);
            return ret(e, c >= 'a' && c <= 'z' ? c - 32 : c);
        }
        std::string s = e.mem.read_cstring(p);
        for (size_t i = 0; i < s.size(); ++i) {
            unsigned char c = static_cast<unsigned char>(s[i]);
            if ((c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC)) {
                ++i;
                continue;
            }
            if (c >= 'a' && c <= 'z') e.mem.write8(p + i, static_cast<uint8_t>(c - 32));
        }
        ret(e, p);
    });
    api(e, "CharUpperBuffA", 2, [](Emulator& e) {
        for (uint32_t i = 0; i < U(e, 1); ++i) {
            uint8_t c = e.mem.read8(A(e, 0) + i);
            if ((c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC)) {
                ++i;
                continue;
            }
            if (c >= 'a' && c <= 'z') e.mem.write8(A(e, 0) + i, static_cast<uint8_t>(c - 32));
        }
        ret(e, U(e, 1));
    });
    api(e, "IsCharLowerA", 1, [](Emulator& e) { ret(e, (U(e, 0) & 0xFF) >= 'a' && (U(e, 0) & 0xFF) <= 'z'); });
    api(e, "ToAsciiEx", 6, [](Emulator& e) {
        uint32_t vk = U(e, 0);
        uint8_t c = 0;
        if (vk >= 'A' && vk <= 'Z') c = static_cast<uint8_t>(vk + 32);
        else if (vk >= '0' && vk <= '9') c = static_cast<uint8_t>(vk);
        else if (vk == ' ') c = ' ';
        if (!c) return ret(e, 0);
        e.mem.write16(A(e, 3), c);
        ret(e, 1);
    });
    forward(e, "UnregisterClassA", "UnregisterClassW", 2, {0});
    forward(e, "SetPropA", "SetPropW", 3, {1});
    forward(e, "GetPropA", "GetPropW", 2, {1});
    forward(e, "RemovePropA", "RemovePropW", 2, {1});
    forward(e, "GetClassInfoA", "GetClassInfoW", 3, {1});
    forward(e, "GetClassInfoExA", "GetClassInfoExW", 3, {1});
    forward(e, "FindWindowA", "FindWindowW", 2, {0, 1});
    forward(e, "FindWindowExA", "FindWindowExW", 4, {2, 3});
    api(e, "EnumDisplaySettingsA", 3, [](Emulator& e) {
        // DEVMODEA has 32 fewer bytes of names before the same fields.
        Gui& g = gui(e);
        uint64_t p = A(e, 2);
        if (!p) return ret(e, 0);
        e.mem.write32(p + 72, 32);  // dmBitsPerPel
        e.mem.write32(p + 76, static_cast<uint32_t>(g.screen_w));
        e.mem.write32(p + 80, static_cast<uint32_t>(g.screen_h));
        e.mem.write32(p + 88, 60);
        ret(e, 1);
    });
    forward(e, "WinHelpA", "WinHelpW", 4, {1});
    api(e, "CopyAcceleratorTableA", 3, [](Emulator& e) { ret(e, call_w(e, "CopyAcceleratorTableW", {A(e, 0), A(e, 1), A(e, 2)})); });
    api(e, "CreateAcceleratorTableA", 2, [](Emulator& e) { ret(e, call_w(e, "CreateAcceleratorTableW", {A(e, 0), A(e, 1)})); });
    api(e, "DefFrameProcA", 5, [](Emulator& e) { ret(e, call_w(e, "DefFrameProcW", {A(e, 0), A(e, 1), A(e, 2), A(e, 3), A(e, 4)})); });
    api(e, "DefMDIChildProcA", 4, [](Emulator& e) { ret(e, call_w(e, "DefMDIChildProcW", {A(e, 0), A(e, 1), A(e, 2), A(e, 3)})); });
    api(e, "PostThreadMessageA", 4, [](Emulator& e) { ret(e, call_w(e, "PostThreadMessageW", {A(e, 0), A(e, 1), A(e, 2), A(e, 3)})); });
    api(e, "SendNotifyMessageA", 4, [](Emulator& e) { ret(e, call_w(e, "SendNotifyMessageW", {A(e, 0), A(e, 1), A(e, 2), A(e, 3)})); });
    api(e, "MapVirtualKeyExA", 3, [](Emulator& e) { ret(e, call_w(e, "MapVirtualKeyW", {A(e, 0), A(e, 1)})); });
    api(e, "GetMenuBarInfo", 4, [](Emulator& e) { ret(e, 0); });

    // SendMessageA: the messages that carry text carry it in code page 932.
    // MFC's dialog initialisation (RT_DLGINIT) fills combo boxes this way even
    // in a Unicode program, so passing the bytes through as UTF-16 put "*.jww"
    // into Jw_cad's file dialog as three garbage characters.
    auto send_a = [](Emulator& e, uint32_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp) -> uint64_t {
        Gui& g = gui(e);
        Window* w = g.win(hwnd);
        if (!w) return 0;
        bool owner_list = false;
        if (w->cls && (w->cls->name == u"COMBOBOX" || w->cls->name == u"LISTBOX" || w->cls->name == u"COMBOLBOX")) {
            bool od = (w->style & 0x30) != 0;
            bool has = w->cls->name == u"COMBOBOX" ? (w->style & 0x200) != 0 : (w->style & 0x40) != 0;
            owner_list = od && !has;
        }
        switch (msg) {
            case 0x000C:  // WM_SETTEXT
            case 0x00C2:  // EM_REPLACESEL
            case 0x0143: case 0x014A: case 0x014C: case 0x0158: case 0x014D: case 0x0145:  // CB_*STRING, CB_DIR
            case 0x0180: case 0x0181: case 0x018F: case 0x01A2: case 0x018C: case 0x018D: {  // LB_*STRING, LB_DIR
                if (owner_list && msg != 0x000C) break;
                WideArg s(e, lp);
                return g.send(hwnd, msg, wp, s.addr);
            }
            case 0x000D: {  // WM_GETTEXT: wp is the buffer size in bytes
                WideOut out(e, static_cast<int>(wp ? wp : 1));
                g.send(hwnd, msg, wp, out.addr);
                return static_cast<uint64_t>(out.store(lp, static_cast<int>(wp)));
            }
            case 0x000E: {  // WM_GETTEXTLENGTH, in bytes
                uint64_t n = g.send(hwnd, msg, 0, 0);
                WideOut out(e, static_cast<int>(n + 1));
                g.send(hwnd, 0x000D, n + 1, out.addr);
                return out.text().size();
            }
            case 0x0148: case 0x0189: {  // CB_GETLBTEXT / LB_GETTEXT
                if (owner_list) break;
                uint64_t n = g.send(hwnd, msg == 0x0148 ? 0x0149 : 0x018A, wp, 0);
                if (static_cast<int32_t>(n) < 0) return n;
                WideOut out(e, static_cast<int>(n + 1));
                g.send(hwnd, msg, wp, out.addr);
                return static_cast<uint64_t>(out.store(lp, 1 << 16));
            }
            case 0x0149: case 0x018A: {  // ..TEXTLEN, in bytes
                if (owner_list) break;
                uint64_t n = g.send(hwnd, msg, wp, 0);
                if (static_cast<int32_t>(n) < 0) return n;
                WideOut out(e, static_cast<int>(n + 1));
                g.send(hwnd, msg == 0x0149 ? 0x0148 : 0x0189, wp, out.addr);
                return out.text().size();
            }
        }
        return g.send(hwnd, msg, wp, lp);
    };
    api(e, "SendMessageA", 4, [send_a](Emulator& e) { ret(e, send_a(e, U(e, 0), U(e, 1), A(e, 2), A(e, 3))); });
    api(e, "SendMessageTimeoutA", 7, [send_a](Emulator& e) {
        uint64_t r = send_a(e, U(e, 0), U(e, 1), A(e, 2), A(e, 3));
        if (A(e, 6)) e.mem.write32(A(e, 6), static_cast<uint32_t>(r));
        ret(e, 1);
    });
    api(e, "SendDlgItemMessageA", 5, [send_a](Emulator& e) {
        uint64_t h = e.call_guest(e.existing_hook("GetDlgItem"), {A(e, 0), A(e, 1)}) & 0xFFFFFFFF;
        ret(e, h ? send_a(e, static_cast<uint32_t>(h), U(e, 2), A(e, 3), A(e, 4)) : 0);
    });
    // The printf family of user32.  wsprintf is variadic and so cdecl; the
    // wv- forms take a va_list and are stdcall like everything else.
    auto wsprintf_core = [](Emulator& e, uint64_t dst, uint64_t fmt, Emulator::Args& va, bool wide) -> uint64_t {
        std::string s = format_guest(e, fmt, va, wide);
        if (wide) {
            std::u16string w = utf8_to_utf16(s);
            write_wstr(e, dst, w, static_cast<int>(w.size() + 1));
            return w.size();
        }
        // Narrow formatting copies bytes through, so the result is already CP932.
        e.mem.write_cstring(dst, s);
        return s.size();
    };
    e.add_hook("wsprintfA", 0, [wsprintf_core](Emulator& e) {
        Emulator::Args a(e, 2);
        ret(e, wsprintf_core(e, A(e, 0), A(e, 1), a, false));
    });
    api(e, "wvsprintfW", 3, [wsprintf_core](Emulator& e) {
        Emulator::Args a = Emulator::Args::va_list_at(e, A(e, 2));
        ret(e, wsprintf_core(e, A(e, 0), A(e, 1), a, true));
    });
    api(e, "wvsprintfA", 3, [wsprintf_core](Emulator& e) {
        Emulator::Args a = Emulator::Args::va_list_at(e, A(e, 2));
        ret(e, wsprintf_core(e, A(e, 0), A(e, 1), a, false));
    });
    // The structures that embed a LOGFONT are smaller in their ANSI form:
    // NONCLIENTMETRICSA is 344 bytes (or 340) against the W form's 504/500,
    // and writing the W form into an ANSI caller's stack buffer is a GS
    // cookie failure a few calls later.
    api(e, "SystemParametersInfoA", 4, [](Emulator& e) {
        uint32_t action = U(e, 0);
        uint64_t p = A(e, 2);
        if (action == 0x0029 && p) {  // SPI_GETNONCLIENTMETRICS
            uint32_t asize = e.mem.read32(p);
            uint64_t w = e.heap_alloc(504);
            e.mem.write32(w, 504);
            uint64_t r = call_w(e, "SystemParametersInfoW", {action, 504, w, A(e, 3)});
            // A: cbSize, 5 ints, LOGFONTA, 2 ints, LOGFONTA, 2 ints, 3 x LOGFONTA, [padded border]
            uint64_t a = p;
            for (int i = 4; i < 24; i += 4) e.mem.write32(a + i, e.mem.read32(w + i));
            logfont_w_to_a(e, w + 24, a + 24);
            e.mem.write32(a + 84, e.mem.read32(w + 116));
            e.mem.write32(a + 88, e.mem.read32(w + 120));
            logfont_w_to_a(e, w + 124, a + 92);
            e.mem.write32(a + 152, e.mem.read32(w + 216));
            e.mem.write32(a + 156, e.mem.read32(w + 220));
            logfont_w_to_a(e, w + 224, a + 160);
            logfont_w_to_a(e, w + 316, a + 220);
            logfont_w_to_a(e, w + 408, a + 280);
            if (asize >= 344) e.mem.write32(a + 340, e.mem.read32(w + 500));
            e.heap_free(w);
            return ret(e, r);
        }
        if (action == 0x001F && p) {  // SPI_GETICONTITLELOGFONT
            uint64_t w = e.heap_alloc(92);
            uint64_t r = call_w(e, "SystemParametersInfoW", {action, 92, w, A(e, 3)});
            logfont_w_to_a(e, w, p);
            e.heap_free(w);
            return ret(e, r);
        }
        ret(e, call_w(e, "SystemParametersInfoW", {A(e, 0), A(e, 1), A(e, 2), A(e, 3)}));
    });
    api(e, "GetMonitorInfoA", 2, [](Emulator& e) {
        // MONITORINFOEXA's device name is bytes; only the common part is filled.
        uint64_t p = A(e, 1);
        if (!p) return ret(e, 0);
        uint32_t size = e.mem.read32(p);
        uint64_t w = e.heap_alloc(104);
        e.mem.write32(w, 40);
        uint64_t r = call_w(e, "GetMonitorInfoW", {A(e, 0), w});
        for (int i = 4; i < 40; i += 4) e.mem.write32(p + i, e.mem.read32(w + i));
        if (size >= 72) e.mem.write_cstring(p + 40, "\\\\.\\DISPLAY1");
        e.heap_free(w);
        ret(e, r);
    });

    // ---- shell32 / shlwapi / winmm / winspool -----------------------------------------------
    api(e, "DragQueryFileA", 4, [](Emulator& e) { ret(e, 0); });
    api(e, "ExtractIconA", 3, [](Emulator& e) { ret(e, 0); });
    api(e, "SHBrowseForFolderA", 1, [](Emulator& e) { ret(e, 0); });
    api(e, "SHGetFileInfoA", 5, [](Emulator& e) { ret(e, 0); });
    api(e, "SHGetMalloc", 1, [](Emulator& e) {
        if (A(e, 0)) e.mem.write32(A(e, 0), 0);
        ret(e, 0x80004005);
    });
    api(e, "SHGetPathFromIDListA", 2, [](Emulator& e) {
        if (!A(e, 0) || !A(e, 1)) return ret(e, 0);
        e.mem.write_cstring(A(e, 1), utf16_to_cp932(read_wstr(e, A(e, 0))));
        ret(e, 1);
    });
    api(e, "ShellExecuteA", 6, [](Emulator& e) { ret(e, 2); });
    api(e, "Shell_NotifyIconA", 2, [](Emulator& e) { ret(e, 1); });  // no tray here
    api(e, "ShellExecuteExA", 1, [](Emulator& e) { ret(e, 0); });
    // shlwapi's ANSI path helpers, done on the bytes (lead bytes skipped).
    auto last_sep = [](const std::string& s, const char* seps) -> size_t {
        size_t found = std::string::npos;
        for (size_t i = 0; i < s.size(); ++i) {
            unsigned char c = static_cast<unsigned char>(s[i]);
            if ((c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC)) {
                ++i;
                continue;
            }
            if (std::strchr(seps, c)) found = i;
        }
        return found;
    };
    api(e, "PathFindExtensionA", 1, [last_sep](Emulator& e) {
        std::string s = e.mem.read_cstring(A(e, 0));
        size_t dot = last_sep(s, "."), sep = last_sep(s, "\\/ ");
        if (dot == std::string::npos || (sep != std::string::npos && sep > dot)) return ret(e, A(e, 0) + s.size());
        ret(e, A(e, 0) + dot);
    });
    api(e, "PathFindFileNameA", 1, [last_sep](Emulator& e) {
        std::string s = e.mem.read_cstring(A(e, 0));
        size_t sep = last_sep(s, "\\/:");
        ret(e, A(e, 0) + (sep == std::string::npos ? 0 : sep + 1));
    });
    api(e, "PathIsUNCA", 1, [](Emulator& e) {
        std::string s = e.mem.read_cstring(A(e, 0));
        ret(e, s.size() >= 2 && s[0] == '\\' && s[1] == '\\' ? 1 : 0);
    });
    api(e, "PathRemoveExtensionA", 1, [last_sep](Emulator& e) {
        std::string s = e.mem.read_cstring(A(e, 0));
        size_t dot = last_sep(s, "."), sep = last_sep(s, "\\/");
        if (dot != std::string::npos && (sep == std::string::npos || dot > sep)) e.mem.write8(A(e, 0) + dot, 0);
        ret(e, 0);
    });
    api(e, "PathStripToRootA", 1, [](Emulator& e) {
        std::string s = e.mem.read_cstring(A(e, 0));
        if (s.size() >= 3 && s[1] == ':') {
            e.mem.write8(A(e, 0) + 3, 0);
            return ret(e, 1);
        }
        ret(e, 0);
    });
    api(e, "StrFormatKBSizeA", 4, [](Emulator& e) {
        uint64_t sz = A(e, 0) | (A(e, 1) << 32);
        std::string s = std::to_string((sz + 1023) / 1024) + " KB";
        if (A(e, 2) && I(e, 3) > static_cast<int>(s.size())) e.mem.write_cstring(A(e, 2), s);
        ret(e, A(e, 2));
    });
    api(e, "PathCchRemoveFileSpec", 2, [](Emulator& e) {
        uint64_t p = A(e, 0);
        std::u16string s = read_wstr(e, p);
        size_t sep = s.find_last_of(u"\\/");
        if (sep == std::u16string::npos) return ret(e, 1);  // S_FALSE
        e.mem.write16(p + (sep == 2 && s[1] == u':' ? 3 : sep) * 2, 0);
        ret(e, 0);
    });
    api(e, "PlaySoundA", 3, [](Emulator& e) { ret(e, 1); });
    api(e, "OpenPrinterA", 3, [](Emulator& e) {
        if (A(e, 1)) e.mem.write32(A(e, 1), 0);
        ret(e, 0);
    });
    api(e, "DocumentPropertiesA", 6, [](Emulator& e) { ret(e, static_cast<uint64_t>(-1) & 0xFFFFFFFF); });
    api(e, "GetJobA", 6, [](Emulator& e) { ret(e, 0); });
    api(e, "GdipCreateBitmapFromFileICM", 2, [](Emulator& e) { ret(e, 6); });
    api(e, "GdipCreateBitmapFromStreamICM", 2, [](Emulator& e) { ret(e, 6); });
    api(e, "CoGetMalloc", 2, [](Emulator& e) {
        if (A(e, 1)) e.mem.write32(A(e, 1), 0);
        ret(e, 0x80004005);
    });
    api(e, "CLSIDFromString", 2, [](Emulator& e) { ret(e, 0x800401F3); });  // CO_E_CLASSSTRING
    api(e, "CLSIDFromProgID", 2, [](Emulator& e) { ret(e, 0x800401F3); });
}

}  // namespace x86emu::win32
