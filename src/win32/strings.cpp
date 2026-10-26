// String helpers shared by the GUI hooks, and code page 932.
#include <algorithm>
#include <cstring>

#include "guest_printf.h"
#include "kanji.h"
#include "hookutil.h"

namespace x86emu::win32 {

std::u16string read_wstr(Emulator& e, uint64_t p, int max_units) {
    std::u16string s;
    if (!p) return s;
    for (int i = 0; max_units < 0 || i < max_units; ++i) {
        uint16_t c = e.mem.read16(p + static_cast<uint64_t>(i) * 2);
        if (!c && max_units < 0) break;
        if (!c && max_units >= 0) {
            // A counted string may still be terminated early by callers that
            // pass a buffer size; keep NULs out of the result.
            break;
        }
        s.push_back(static_cast<char16_t>(c));
        if (s.size() > (1u << 24)) break;
    }
    return s;
}

int write_wstr(Emulator& e, uint64_t dst, const std::u16string& s, int cap) {
    if (cap <= 0 || !dst) return static_cast<int>(s.size());
    int n = std::min(static_cast<int>(s.size()), cap - 1);
    for (int i = 0; i < n; ++i) e.mem.write16(dst + static_cast<uint64_t>(i) * 2, s[i]);
    e.mem.write16(dst + static_cast<uint64_t>(n) * 2, 0);
    return n;
}

std::string to_utf8(const std::u16string& s) { return utf16_string_to_utf8(s); }

std::u16string from_utf8(const std::string& s) {
    std::u16string out;
    size_t i = 0;
    while (i < s.size()) {
        uint32_t c = static_cast<unsigned char>(s[i]);
        int n = 0;
        if (c < 0x80) n = 0;
        else if ((c >> 5) == 6) { c &= 0x1F; n = 1; }
        else if ((c >> 4) == 14) { c &= 0x0F; n = 2; }
        else if ((c >> 3) == 30) { c &= 0x07; n = 3; }
        else { ++i; out.push_back(0xFFFD); continue; }
        ++i;
        for (int k = 0; k < n && i < s.size(); ++k, ++i) c = (c << 6) | (static_cast<unsigned char>(s[i]) & 0x3F);
        if (c >= 0x10000) {
            c -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (c >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (c & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(c));
        }
    }
    return out;
}

std::u16string upper(const std::u16string& s) {
    std::u16string r = s;
    for (auto& c : r) {
        if (c >= 'a' && c <= 'z') c = static_cast<char16_t>(c - 32);
        else if (c >= 0xFF41 && c <= 0xFF5A) c = static_cast<char16_t>(c - 0x20);
    }
    return r;
}

// Code page 932 goes through libtera's kanji.c (BSD), one character at a
// time; the counted loops are here because its string functions stop at NUL.
std::u16string cp932_to_utf16(const std::string& s) {
    std::u16string out;
    size_t i = 0;
    while (i < s.size()) {
        unsigned c = static_cast<unsigned char>(s[i]);
        unsigned code = c;
        size_t len = 1;
        bool lead = (c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC);
        if (lead && i + 1 < s.size()) {
            code = (c << 8) | static_cast<unsigned char>(s[i + 1]);
            len = 2;
        }
        unsigned short u = kj_cp932_to_unicode_c(static_cast<unsigned short>(code));
        if (!u && code) u = len == 2 ? 0x30FB : (c < 0x80 ? c : 0xFFFD);
        out.push_back(static_cast<char16_t>(u));
        i += len;
    }
    return out;
}

std::string utf16_to_cp932(const std::u16string& s) {
    std::string out;
    for (char16_t w : s) {
        unsigned short c = w ? kj_unicode_to_cp932_c(w) : 0;
        if (!c && w) c = '?';  // WideCharToMultiByte's default character
        if (c > 0xFF) out.push_back(static_cast<char>(c >> 8));
        out.push_back(static_cast<char>(c & 0xFF));
    }
    return out;
}

bool is_intresource(uint64_t p) { return p < 0x10000; }

std::u16string res_name(Emulator& e, uint64_t id) {
    if (is_intresource(id)) return u"#" + from_utf8(std::to_string(id));
    return read_wstr(e, id);
}

// The resource directory walk: type, then name, then language (any).
bool find_resource(Emulator& e, uint64_t hmod, uint64_t type, uint64_t name, uint64_t& addr,
                   uint32_t& size) {
    Emulator::Module* m = e.module_for(hmod ? hmod : e.image().image_base);
    if (!m || !m->image.resource_table) return false;
    uint64_t root = m->image.resource_table;
    auto find_entry = [&](uint64_t dir, uint64_t key) -> uint64_t {
        uint32_t named = e.mem.read16(dir + 12), ids = e.mem.read16(dir + 14);
        bool by_name = !is_intresource(key);
        std::u16string wanted;
        if (by_name) {
            wanted = upper(read_wstr(e, key));
            if (!wanted.empty() && wanted[0] == u'#') {
                // "#123" names the integer id 123.
                uint64_t v = 0;
                for (size_t i = 1; i < wanted.size(); ++i) v = v * 10 + (wanted[i] - u'0');
                by_name = false;
                key = v;
            }
        }
        for (uint32_t i = 0; i < named + ids; ++i) {
            uint64_t entry = dir + 16 + static_cast<uint64_t>(i) * 8;
            uint32_t id = e.mem.read32(entry), off = e.mem.read32(entry + 4);
            bool entry_named = (id & 0x80000000u) != 0;
            if (by_name != entry_named) continue;
            if (entry_named) {
                uint64_t str = root + (id & 0x7FFFFFFFu);
                uint32_t len = e.mem.read16(str);
                std::u16string got;
                for (uint32_t k = 0; k < len; ++k) got.push_back(static_cast<char16_t>(e.mem.read16(str + 2 + k * 2)));
                if (upper(got) != wanted) continue;
            } else if (id != static_cast<uint32_t>(key)) {
                continue;
            }
            return root + (off & 0x7FFFFFFFu);
        }
        return 0;
    };
    uint64_t t = find_entry(root, type);
    if (!t) return false;
    uint64_t n = find_entry(t, name);
    if (!n) return false;
    uint32_t count = e.mem.read16(n + 12) + e.mem.read16(n + 14);
    if (!count) return false;
    // The language, as a Japanese Windows would pick it: Japanese, then
    // neutral, then English, then whatever comes first.  A DLL with every
    // language (Wine's comctl32) lists Arabic first, and its property sheets
    // came out with Arabic buttons.
    auto rank = [](uint32_t lang) {
        if (lang == 0x0411) return 0;
        if ((lang & 0x3FF) == 0x11) return 1;  // other Japanese
        if (lang == 0 || lang == 0x0400 || lang == 0x0800) return 2;
        if (lang == 0x0409) return 3;
        if ((lang & 0x3FF) == 0x09) return 4;
        return 5;
    };
    uint32_t pick = 0;
    for (uint32_t i = 1; i < count; ++i)
        if (rank(e.mem.read32(n + 16 + i * 8)) < rank(e.mem.read32(n + 16 + pick * 8))) pick = i;
    uint64_t leaf = root + (e.mem.read32(n + 16 + static_cast<uint64_t>(pick) * 8 + 4) & 0x7FFFFFFFu);
    addr = m->image.base + e.mem.read32(leaf);
    size = e.mem.read32(leaf + 4);
    return true;
}

}  // namespace x86emu::win32
