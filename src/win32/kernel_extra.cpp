// kernel32 for a GUI program: the Japanese locale, code page 932, Global and
// Local memory, atoms, INI files, and the rest of what MFC asks kernel32 that
// a console program never does.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "files.h"
#include "guest_printf.h"
#include "hookutil.h"

namespace x86emu::win32 {

namespace {

constexpr uint32_t kLangJa = 0x0411;

bool cp_is_932(uint32_t cp) { return cp == 0 || cp == 1 || cp == 3 || cp == 932; }

// ---------------------------------------------------------------------------
// INI files
// ---------------------------------------------------------------------------

struct Ini {
    // section -> list of (key, value), in file order
    std::vector<std::pair<std::u16string, std::vector<std::pair<std::u16string, std::u16string>>>> sections;
};

std::u16string trim(const std::u16string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

std::string ini_host_path(const std::u16string& file) {
    std::u16string f = file;
    // A bare name lives in the Windows directory; ours is the working directory.
    if (f.find(u'\\') == std::u16string::npos && f.find(u'/') == std::u16string::npos) f = u".\\" + f;
    return FileTable::host_path(to_utf8(f));
}

Ini read_ini(const std::u16string& file) {
    Ini ini;
    FILE* fp = std::fopen(ini_host_path(file).c_str(), "rb");
    if (!fp) return ini;
    std::string data;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, fp)) > 0) data.append(buf, n);
    std::fclose(fp);
    std::u16string text;
    if (data.size() >= 2 && static_cast<unsigned char>(data[0]) == 0xFF && static_cast<unsigned char>(data[1]) == 0xFE) {
        for (size_t i = 2; i + 1 < data.size(); i += 2)
            text.push_back(static_cast<char16_t>(static_cast<unsigned char>(data[i]) | static_cast<unsigned char>(data[i + 1]) << 8));
    } else {
        text = cp932_to_utf16(data);
    }
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find(u'\n', pos);
        if (nl == std::u16string::npos) nl = text.size();
        std::u16string line = trim(text.substr(pos, nl - pos));
        pos = nl + 1;
        if (line.empty() || line[0] == u';') continue;
        if (line[0] == u'[') {
            size_t close = line.find(u']');
            ini.sections.push_back({line.substr(1, close == std::u16string::npos ? std::u16string::npos : close - 1), {}});
            continue;
        }
        if (ini.sections.empty()) continue;
        size_t eq = line.find(u'=');
        if (eq == std::u16string::npos) continue;
        ini.sections.back().second.push_back({trim(line.substr(0, eq)), trim(line.substr(eq + 1))});
    }
    return ini;
}

void write_ini(const std::u16string& file, const Ini& ini) {
    std::u16string text;
    for (const auto& [sec, keys] : ini.sections) {
        text += u"[" + sec + u"]\r\n";
        for (const auto& [k, v] : keys) text += k + u"=" + v + u"\r\n";
    }
    std::string bytes = utf16_to_cp932(text);
    FILE* fp = std::fopen(ini_host_path(file).c_str(), "wb");
    if (!fp) return;
    std::fwrite(bytes.data(), 1, bytes.size(), fp);
    std::fclose(fp);
}

bool ieq(const std::u16string& a, const std::u16string& b) { return upper(a) == upper(b); }

// GetPrivateProfileString's three modes: all sections, all keys of one, or
// one value.
std::u16string profile_string(const std::u16string& file, const std::u16string* sec,
                              const std::u16string* key, const std::u16string& def, bool& multi) {
    Ini ini = read_ini(file);
    multi = false;
    if (!sec) {
        multi = true;
        std::u16string out;
        for (const auto& s : ini.sections) out += s.first + u'\0';
        return out;
    }
    for (const auto& s : ini.sections) {
        if (!ieq(s.first, *sec)) continue;
        if (!key) {
            multi = true;
            std::u16string out;
            for (const auto& kv : s.second) out += kv.first + u'\0';
            return out;
        }
        for (const auto& kv : s.second) {
            if (!ieq(kv.first, *key)) continue;
            std::u16string v = kv.second;
            if (v.size() >= 2 && (v[0] == u'"' || v[0] == u'\'') && v.back() == v[0]) v = v.substr(1, v.size() - 2);
            return v;
        }
    }
    return def;
}

void write_profile(const std::u16string& file, const std::u16string* sec, const std::u16string* key,
                   const std::u16string* val) {
    if (!sec) return;
    Ini ini = read_ini(file);
    auto it = std::find_if(ini.sections.begin(), ini.sections.end(),
                           [&](const auto& s) { return ieq(s.first, *sec); });
    if (!key) {
        if (it != ini.sections.end()) ini.sections.erase(it);
        write_ini(file, ini);
        return;
    }
    if (it == ini.sections.end()) {
        if (!val) return;
        ini.sections.push_back({*sec, {}});
        it = ini.sections.end() - 1;
    }
    auto& keys = it->second;
    auto kt = std::find_if(keys.begin(), keys.end(), [&](const auto& kv) { return ieq(kv.first, *key); });
    if (!val) {
        if (kt != keys.end()) keys.erase(kt);
    } else if (kt != keys.end()) {
        kt->second = *val;
    } else {
        keys.push_back({*key, *val});
    }
    write_ini(file, ini);
}

// ---------------------------------------------------------------------------
// Time helpers
// ---------------------------------------------------------------------------

void read_systemtime(Emulator& e, uint64_t p, int v[8]) {
    for (int i = 0; i < 8; ++i) v[i] = e.mem.read16(p + i * 2);
}
void write_systemtime(Emulator& e, uint64_t p, const int v[8]) {
    for (int i = 0; i < 8; ++i) e.mem.write16(p + i * 2, static_cast<uint16_t>(v[i]));
}

// FILETIME (100 ns since 1601) <-> broken-down UTC.
void filetime_to_st(uint64_t ft, int v[8]) {
    int64_t ms = static_cast<int64_t>(ft / 10000);
    int64_t days = ms / 86400000;
    int64_t rem = ms % 86400000;
    // days since 1601-01-01 -> civil date
    int64_t z = days - 134774 + 719468;  // shift to 1970-03-01 era, then to 0000-03-01
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int64_t d = doy - (153 * mp + 2) / 5 + 1;
    int64_t m = mp + (mp < 10 ? 3 : -9);
    if (m <= 2) ++y;
    v[0] = static_cast<int>(y);
    v[1] = static_cast<int>(m);
    v[2] = static_cast<int>(((days % 7) + 1) % 7);  // 1601-01-01 was a Monday
    v[3] = static_cast<int>(d);
    v[4] = static_cast<int>(rem / 3600000);
    v[5] = static_cast<int>(rem / 60000 % 60);
    v[6] = static_cast<int>(rem / 1000 % 60);
    v[7] = static_cast<int>(rem % 1000);
}

uint64_t st_to_filetime(const int v[8]) {
    int64_t y = v[0], m = v[1], d = v[3];
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = era * 146097 + doe - 719468 + 134774;
    int64_t ms = days * 86400000 + v[4] * 3600000LL + v[5] * 60000LL + v[6] * 1000LL + v[7];
    return static_cast<uint64_t>(ms) * 10000;
}

int64_t tz_bias_minutes() {
    // Local minus UTC, in minutes, from the host.
    std::time_t now = std::time(nullptr);
    std::tm lt{}, gt{};
#if defined(_WIN32)
    localtime_s(&lt, &now);
    gmtime_s(&gt, &now);
#else
    localtime_r(&now, &lt);
    gmtime_r(&now, &gt);
#endif
    lt.tm_isdst = 0;
    gt.tm_isdst = 0;
    return static_cast<int64_t>(std::difftime(std::mktime(&lt), std::mktime(&gt)) / 60);
}

}  // namespace

void install_kernel_extra(Emulator& e) {
    Gui& g = gui(e);
    (void)g;

    // ---- locale and code pages ----------------------------------------------
    api(e, "GetACP", 0, [](Emulator& e) { ret(e, 932); });
    api(e, "GetOEMCP", 0, [](Emulator& e) { ret(e, 932); });
    api(e, "GetConsoleCP", 0, [](Emulator& e) { ret(e, 932); });
    api(e, "GetConsoleOutputCP", 0, [](Emulator& e) { ret(e, 932); });
    api(e, "GetCPInfo", 2, [](Emulator& e) {
        uint64_t p = A(e, 1);
        if (!p) return ret(e, 0);
        for (int i = 0; i < 20; i += 4) e.mem.write32(p + i, 0);
        uint32_t cp = U(e, 0);
        if (cp_is_932(cp)) {
            e.mem.write32(p, 2);
            e.mem.write8(p + 4, '?');
            uint8_t lead[] = {0x81, 0x9F, 0xE0, 0xFC};
            for (int i = 0; i < 4; ++i) e.mem.write8(p + 6 + i, lead[i]);
        } else {
            e.mem.write32(p, cp == 65001 ? 4 : 1);
            e.mem.write8(p + 4, '?');
        }
        ret(e, 1);
    });
    api(e, "GetCPInfoExW", 3, [](Emulator& e) {
        uint64_t p = A(e, 2);
        if (!p) return ret(e, 0);
        for (int i = 0; i < 544; i += 4) e.mem.write32(p + i, 0);
        uint32_t cp = U(e, 0);
        bool sj = cp_is_932(cp);
        e.mem.write32(p, sj ? 2 : 1);
        e.mem.write8(p + 4, '?');
        if (sj) {
            uint8_t lead[] = {0x81, 0x9F, 0xE0, 0xFC};
            for (int i = 0; i < 4; ++i) e.mem.write8(p + 6 + i, lead[i]);
        }
        e.mem.write16(p + 18, '?');
        e.mem.write32(p + 20, sj ? 932 : cp);
        write_wstr(e, p + 24, sj ? u"932  (ANSI/OEM - Japanese Shift-JIS)" : u"", 260);
        ret(e, 1);
    });
    api(e, "IsDBCSLeadByte", 1, [](Emulator& e) {
        uint32_t c = U(e, 0) & 0xFF;
        ret(e, (c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC));
    });
    api(e, "IsDBCSLeadByteEx", 2, [](Emulator& e) {
        uint32_t c = U(e, 1) & 0xFF;
        ret(e, cp_is_932(U(e, 0)) && ((c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC)));
    });
    api(e, "MultiByteToWideChar", 6, [](Emulator& e) {
        uint32_t cp = U(e, 0);
        uint64_t src = A(e, 2);
        int len = I(e, 3);
        uint64_t dst = A(e, 4);
        int cap = I(e, 5);
        std::string s;
        if (len < 0) {
            s = e.mem.read_cstring(src);
        } else if (len > 0) {
            s.resize(static_cast<size_t>(len));
            e.mem.read(src, s.data(), static_cast<uint64_t>(len));
        }
        std::u16string w;
        // MB_ERR_INVALID_CHARS: fail on bytes that are not valid in the code
        // page.  Programs detect UTF-8 this way - Jw_cad labels a Shift-JIS
        // batch file "[UTF-8]" and garbles its comment otherwise.
        if (cp == 65001 && (U(e, 1) & 0x8)) {
            size_t i = 0;
            while (i < s.size()) {
                unsigned char c = static_cast<unsigned char>(s[i]);
                int n = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 && c >= 0xC2 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 && c <= 0xF4 ? 3 : -1;
                bool ok = n >= 0 && i + static_cast<size_t>(n) < s.size() + (n == 0 ? 1 : 0);
                for (int k = 1; ok && k <= n; ++k) ok = (static_cast<unsigned char>(s[i + k]) & 0xC0) == 0x80;
                if (!ok) {
                    e.set_last_error(1113);  // ERROR_NO_UNICODE_TRANSLATION
                    return ret(e, 0);
                }
                i += static_cast<size_t>(n) + 1;
            }
        }
        if (cp == 65001) w = utf8_to_utf16(s);
        else if (cp_is_932(cp)) w = cp932_to_utf16(s);
        else for (char c : s) w.push_back(static_cast<char16_t>(static_cast<unsigned char>(c)));
        if (len < 0) w.push_back(0);
        if (cap == 0 || !dst) return ret(e, w.size());
        if (static_cast<int>(w.size()) > cap) {
            e.set_last_error(122);  // ERROR_INSUFFICIENT_BUFFER
            return ret(e, 0);
        }
        for (size_t i = 0; i < w.size(); ++i) e.mem.write16(dst + i * 2, w[i]);
        ret(e, w.size());
    });
    api(e, "WideCharToMultiByte", 8, [](Emulator& e) {
        uint32_t cp = U(e, 0);
        uint64_t src = A(e, 2);
        int len = I(e, 3);
        uint64_t dst = A(e, 4);
        int cap = I(e, 5);
        std::u16string w = len < 0 ? read_wstr(e, src) : read_wstr(e, src, len);
        if (len >= 0 && static_cast<int>(w.size()) < len) {
            // Embedded NULs: read exactly len units.
            w.clear();
            for (int i = 0; i < len; ++i) w.push_back(static_cast<char16_t>(e.mem.read16(src + i * 2)));
        }
        std::string out;
        if (cp == 65001) {
            out = to_utf8(w);
        } else if (cp_is_932(cp)) {
            out = utf16_to_cp932(w);
            if (A(e, 7)) {
                bool used = false;
                for (char16_t c : w)
                    if (c && c != '?' && utf16_to_cp932(std::u16string(1, c)) == "?") used = true;
                e.mem.write32(A(e, 7), used);
            }
        } else {
            for (char16_t c : w) out.push_back(c < 256 ? static_cast<char>(c) : '?');
        }
        if (len < 0) out.push_back('\0');
        if (cap == 0 || !dst) return ret(e, out.size());
        if (static_cast<int>(out.size()) > cap) {
            e.set_last_error(122);
            return ret(e, 0);
        }
        e.mem.write(dst, out.data(), out.size());
        ret(e, out.size());
    });
    auto lang = [](Emulator& e) { ret(e, kLangJa); };
    for (const char* n : {"GetUserDefaultLCID", "GetSystemDefaultLCID", "GetThreadLocale",
                          "GetUserDefaultLangID", "GetSystemDefaultLangID",
                          "GetUserDefaultUILanguage", "GetSystemDefaultUILanguage",
                          "GetThreadUILanguage"})
        api(e, n, 0, lang);
    api(e, "SetThreadLocale", 1, [](Emulator& e) { ret(e, 1); });
    api(e, "SetThreadUILanguage", 1, [](Emulator& e) { ret(e, U(e, 0)); });
    api(e, "IsValidLocale", 2, [](Emulator& e) { ret(e, 1); });
    auto locale_info = [](uint32_t type) -> std::u16string {
        switch (type & 0x0FFFFFFF & ~0x80000000u) {
            case 0x0001: return u"0411";       // LOCALE_ILANGUAGE
            case 0x0002: return u"日本語";      // LOCALE_SLANGUAGE
            case 0x1001: return u"Japanese";   // LOCALE_SENGLANGUAGE
            case 0x0003: return u"JPN";        // LOCALE_SABBREVLANGNAME
            case 0x0004: return u"日本語";      // LOCALE_SNATIVELANGNAME
            case 0x0005: return u"81";         // LOCALE_ICOUNTRY
            case 0x0006: return u"日本";        // LOCALE_SCOUNTRY
            case 0x1002: return u"Japan";      // LOCALE_SENGCOUNTRY
            case 0x0007: return u"JPN";        // LOCALE_SABBREVCTRYNAME
            case 0x000B: return u"932";        // LOCALE_IDEFAULTCODEPAGE (OEM)
            case 0x1004: return u"932";        // LOCALE_IDEFAULTANSICODEPAGE
            case 0x100B: return u"10001";      // LOCALE_IDEFAULTMACCODEPAGE
            case 0x000C: return u",";          // LOCALE_SLIST
            case 0x000D: return u"0";          // LOCALE_IMEASURE
            case 0x000E: return u".";          // LOCALE_SDECIMAL
            case 0x000F: return u",";          // LOCALE_STHOUSAND
            case 0x0010: return u"3;0";        // LOCALE_SGROUPING
            case 0x0011: return u"2";          // LOCALE_IDIGITS
            case 0x0012: return u"1";          // LOCALE_ILZERO
            case 0x1010: return u"1";          // LOCALE_INEGNUMBER
            case 0x0013: return u"0123456789"; // LOCALE_SNATIVEDIGITS
            case 0x0014: return u"¥";          // LOCALE_SCURRENCY
            case 0x0015: return u"JPY";
            case 0x0016: return u".";
            case 0x0017: return u",";
            case 0x0018: return u"3;0";
            case 0x0019: return u"0";          // LOCALE_ICURRDIGITS
            case 0x001B: return u"0";          // LOCALE_ICURRENCY
            case 0x001C: return u"1";          // LOCALE_INEGCURR
            case 0x001D: return u"/";          // LOCALE_SDATE
            case 0x001E: return u":";          // LOCALE_STIME
            case 0x001F: return u"yyyy/MM/dd"; // LOCALE_SSHORTDATE
            case 0x0020: return u"yyyy'年'M'月'd'日'";  // LOCALE_SLONGDATE
            case 0x1003: return u"H:mm:ss";    // LOCALE_STIMEFORMAT
            case 0x0021: return u"2";          // LOCALE_IDATE
            case 0x0022: return u"2";          // LOCALE_ILDATE
            case 0x0023: return u"1";          // LOCALE_ITIME
            case 0x0024: return u"1";          // LOCALE_ICENTURY
            case 0x0025: return u"0";          // LOCALE_ITLZERO
            case 0x0028: return u"午前";
            case 0x0029: return u"午後";
            case 0x1009: return u"1";          // LOCALE_ICALENDARTYPE
            case 0x100C: return u"6";          // LOCALE_IFIRSTDAYOFWEEK
            case 0x0059: return u"ja";         // LOCALE_SISO639LANGNAME
            case 0x005A: return u"JP";         // LOCALE_SISO3166CTRYNAME
            case 0x005C: return u"ja-JP";      // LOCALE_SNAME
            case 0x006E: return u"ja-JP";
            case 0x0068: return u"-";
        }
        return u"";
    };
    api(e, "GetLocaleInfoW", 4, [locale_info](Emulator& e) {
        uint32_t type = U(e, 1);
        if (type & 0x20000000) {  // LOCALE_RETURN_NUMBER
            std::u16string s = locale_info(type);
            uint32_t v = 0;
            for (char16_t c : s) if (c >= '0' && c <= '9') v = v * 10 + (c - '0');
            if (A(e, 2) && I(e, 3) >= 2) e.mem.write32(A(e, 2), v);
            return ret(e, 2);
        }
        if ((type & 0xFFFF) == 0x58) {  // LOCALE_FONTSIGNATURE
            if (A(e, 2) && I(e, 3) >= 16) {
                uint32_t sig[4] = {0x00000287, 0x00000000, 0x00020000, 0x00000000};
                for (int i = 0; i < 4; ++i) e.mem.write32(A(e, 2) + i * 4, sig[i]);
            }
            return ret(e, 16);
        }
        std::u16string s = locale_info(type);
        if (s.empty()) return ret(e, 0);
        if (!A(e, 2) || I(e, 3) == 0) return ret(e, s.size() + 1);
        write_wstr(e, A(e, 2), s, I(e, 3));
        ret(e, std::min<size_t>(s.size() + 1, static_cast<size_t>(I(e, 3))));
    });
    api(e, "GetLocaleInfoA", 4, [locale_info](Emulator& e) {
        std::string s = utf16_to_cp932(locale_info(U(e, 1)));
        if (s.empty()) return ret(e, 0);
        if (!A(e, 2) || I(e, 3) == 0) return ret(e, s.size() + 1);
        size_t n = std::min<size_t>(s.size(), static_cast<size_t>(I(e, 3) - 1));
        e.mem.write(A(e, 2), s.data(), n);
        e.mem.write8(A(e, 2) + n, 0);
        ret(e, n + 1);
    });
    api(e, "GetLocaleInfoEx", 4, [locale_info](Emulator& e) {
        std::u16string s = locale_info(U(e, 1));
        if (s.empty()) return ret(e, 0);
        if (!A(e, 2) || I(e, 3) == 0) return ret(e, s.size() + 1);
        write_wstr(e, A(e, 2), s, I(e, 3));
        ret(e, s.size() + 1);
    });
    api(e, "GetUserDefaultLocaleName", 2, [](Emulator& e) {
        write_wstr(e, A(e, 0), u"ja-JP", I(e, 1));
        ret(e, 6);
    });
    api(e, "LCIDToLocaleName", 4, [](Emulator& e) {
        if (!A(e, 1)) return ret(e, 6);
        write_wstr(e, A(e, 1), u"ja-JP", I(e, 2));
        ret(e, 6);
    });
    api(e, "LocaleNameToLCID", 2, [](Emulator& e) { ret(e, kLangJa); });
    api(e, "ConvertDefaultLocale", 1, [](Emulator& e) { ret(e, kLangJa); });
    api(e, "GetKeyboardLayout", 1, [](Emulator& e) { ret(e, 0x04110411); });
    // CT_CTYPE1 only, which is what anyone asks.  GetStringTypeW is the same
    // without the leading locale argument.
    auto string_type = [](Emulator& e, int first) {
        uint32_t info = U(e, first);
        uint64_t src = A(e, first + 1), out = A(e, first + 3);
        int n = I(e, first + 2);
        std::u16string s = n < 0 ? read_wstr(e, src) : read_wstr(e, src, n);
        if (n < 0) n = static_cast<int>(s.size()) + 1;
        for (int i = 0; i < n; ++i) {
            uint16_t c = i < static_cast<int>(s.size()) ? s[i] : static_cast<uint16_t>(e.mem.read16(src + i * 2));
            uint16_t t = 0;
            if (info == 1) {
                if (c >= 'A' && c <= 'Z') t = 0x0001 | 0x0100;
                else if (c >= 'a' && c <= 'z') t = 0x0002 | 0x0100;
                else if (c >= '0' && c <= '9') t = 0x0004 | 0x0080;
                else if (c == ' ' || c == 9 || c == 10 || c == 13 || c == 0x3000) t = 0x0008 | (c == ' ' || c == 0x3000 ? 0x0040 : 0);
                else if (c < 0x20 || c == 0x7F) t = 0x0020;
                else if (c < 0x80) t = 0x0010;
                else if (c >= 0x3040) t = 0x0100;
            }
            e.mem.write16(out + i * 2, t);
        }
        ret(e, 1);
    };
    api(e, "GetStringTypeExW", 5, [string_type](Emulator& e) { string_type(e, 1); });
    api(e, "GetStringTypeW", 4, [string_type](Emulator& e) { string_type(e, 0); });

    api(e, "CompareStringA", 6, [](Emulator& e) {
        uint32_t flags = U(e, 1);
        auto rd = [&](uint64_t p, int n) {
            std::string s;
            if (n < 0) s = e.mem.read_cstring(p);
            else if (n > 0) {
                s.resize(static_cast<size_t>(n));
                e.mem.read(p, s.data(), s.size());
            }
            return cp932_to_utf16(s);
        };
        std::u16string a = rd(A(e, 2), I(e, 3)), b = rd(A(e, 4), I(e, 5));
        if (flags & 1) {  // NORM_IGNORECASE
            a = upper(a);
            b = upper(b);
        }
        ret(e, a < b ? 1 : (a == b ? 2 : 3));
    });
    api(e, "CharUpperW", 1, [](Emulator& e) {
        uint64_t p = A(e, 0);
        if (p < 0x10000) {
            uint32_t c = static_cast<uint32_t>(p);
            return ret(e, upper(std::u16string(1, static_cast<char16_t>(c)))[0]);
        }
        std::u16string s = upper(read_wstr(e, p));
        for (size_t i = 0; i < s.size(); ++i) e.mem.write16(p + i * 2, s[i]);
        ret(e, p);
    });
    api(e, "CharLowerW", 1, [](Emulator& e) {
        uint64_t p = A(e, 0);
        auto low = [](char16_t c) -> char16_t { return c >= 'A' && c <= 'Z' ? static_cast<char16_t>(c + 32) : c; };
        if (p < 0x10000) return ret(e, low(static_cast<char16_t>(p)));
        std::u16string s = read_wstr(e, p);
        for (size_t i = 0; i < s.size(); ++i) e.mem.write16(p + i * 2, low(s[i]));
        ret(e, p);
    });
    api(e, "CharUpperBuffW", 2, [](Emulator& e) {
        uint64_t p = A(e, 0);
        uint32_t n = U(e, 1);
        for (uint32_t i = 0; i < n; ++i) {
            uint16_t c = e.mem.read16(p + i * 2);
            if (c >= 'a' && c <= 'z') e.mem.write16(p + i * 2, static_cast<uint16_t>(c - 32));
        }
        ret(e, n);
    });
    api(e, "CharLowerBuffW", 2, [](Emulator& e) {
        uint64_t p = A(e, 0);
        uint32_t n = U(e, 1);
        for (uint32_t i = 0; i < n; ++i) {
            uint16_t c = e.mem.read16(p + i * 2);
            if (c >= 'A' && c <= 'Z') e.mem.write16(p + i * 2, static_cast<uint16_t>(c + 32));
        }
        ret(e, n);
    });
    api(e, "IsCharLowerW", 1, [](Emulator& e) { ret(e, U(e, 0) >= 'a' && U(e, 0) <= 'z'); });
    api(e, "IsCharUpperW", 1, [](Emulator& e) { ret(e, U(e, 0) >= 'A' && U(e, 0) <= 'Z'); });
    api(e, "IsCharAlphaW", 1, [](Emulator& e) {
        uint32_t c = U(e, 0) & 0xFFFF;
        ret(e, (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c >= 0x3040);
    });
    api(e, "IsCharAlphaNumericW", 1, [](Emulator& e) {
        uint32_t c = U(e, 0) & 0xFFFF;
        ret(e, (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c >= 0x3040);
    });
    api(e, "CharNextW", 1, [](Emulator& e) {
        uint64_t p = A(e, 0);
        ret(e, e.mem.read16(p) ? p + 2 : p);
    });
    api(e, "CharPrevW", 2, [](Emulator& e) {
        uint64_t s = A(e, 0), p = A(e, 1);
        ret(e, p > s ? p - 2 : s);
    });
    api(e, "CharNextA", 1, [](Emulator& e) {
        uint64_t p = A(e, 0);
        uint8_t c = e.mem.read8(p);
        if (!c) return ret(e, p);
        bool lead = (c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC);
        ret(e, lead && e.mem.read8(p + 1) ? p + 2 : p + 1);
    });
    api(e, "CharPrevA", 2, [](Emulator& e) {
        uint64_t s = A(e, 0), p = A(e, 1);
        uint64_t q = s, prev = s;
        while (q < p) {
            prev = q;
            uint8_t c = e.mem.read8(q);
            bool lead = (c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC);
            q += lead ? 2 : 1;
        }
        ret(e, prev);
    });

    // ---- Global / Local memory ---------------------------------------------
    // Movable blocks are fixed ones in disguise: the handle *is* the pointer,
    // which every caller copes with because GlobalLock/GlobalHandle map one to
    // the other.
    auto galloc = [](Emulator& e) {
        uint32_t flags = U(e, 0);
        uint64_t size = A(e, 1);
        uint64_t p = e.heap_alloc(size ? size : 1);
        if (p && (flags & 0x40)) {  // GMEM_ZEROINIT
            std::vector<uint8_t> z(static_cast<size_t>(size ? size : 1), 0);
            e.mem.write(p, z.data(), z.size());
        }
        ret(e, p);
    };
    api(e, "GlobalAlloc", 2, galloc);
    api(e, "LocalAlloc", 2, galloc);
    auto gfree = [](Emulator& e) {
        if (A(e, 0)) e.heap_free(A(e, 0));
        ret(e, 0);
    };
    api(e, "GlobalFree", 1, gfree);
    api(e, "LocalFree", 1, gfree);
    auto glock = [](Emulator& e) { ret(e, A(e, 0)); };
    api(e, "GlobalLock", 1, glock);
    api(e, "LocalLock", 1, glock);
    api(e, "GlobalHandle", 1, glock);
    api(e, "LocalHandle", 1, glock);
    api(e, "GlobalUnlock", 1, [](Emulator& e) {
        e.set_last_error(0);
        ret(e, 0);
    });
    api(e, "LocalUnlock", 1, [](Emulator& e) {
        e.set_last_error(0);
        ret(e, 0);
    });
    auto gsize = [](Emulator& e) { ret(e, A(e, 0) ? e.heap_block_size(A(e, 0)) : 0); };
    api(e, "GlobalSize", 1, gsize);
    api(e, "LocalSize", 1, gsize);
    api(e, "GlobalFlags", 1, [](Emulator& e) { ret(e, 0); });
    api(e, "LocalFlags", 1, [](Emulator& e) { ret(e, 0); });
    auto grealloc = [](Emulator& e) {
        uint64_t p = A(e, 0), size = A(e, 1);
        uint32_t flags = U(e, 2);
        if (flags & 0x80) return ret(e, p);  // GMEM_MODIFY
        uint64_t old = p ? e.heap_block_size(p) : 0;
        uint64_t q = e.heap_realloc(p, size ? size : 1);
        if (q && (flags & 0x40) && size > old) {
            std::vector<uint8_t> z(static_cast<size_t>(size - old), 0);
            e.mem.write(q + old, z.data(), z.size());
        }
        ret(e, q);
    };
    api(e, "GlobalReAlloc", 3, grealloc);
    api(e, "LocalReAlloc", 3, grealloc);
    api(e, "GlobalCompact", 1, [](Emulator& e) { ret(e, 0x1000000); });
    api(e, "GlobalMemoryStatus", 1, [](Emulator& e) {
        uint64_t p = A(e, 0);
        e.mem.write32(p, 32);
        e.mem.write32(p + 4, 30);
        e.mem.write32(p + 8, 0x7FFFFFFF);
        e.mem.write32(p + 12, 0x60000000);
        e.mem.write32(p + 16, 0x7FFFFFFF);
        e.mem.write32(p + 20, 0x60000000);
        e.mem.write32(p + 24, 0x7FFE0000);
        e.mem.write32(p + 28, 0x60000000);
        ret(e, 0);
    });
    api(e, "HeapQueryInformation", 5, [](Emulator& e) {
        if (A(e, 2) && U(e, 3) >= 4) e.mem.write32(A(e, 2), 2);  // low-fragmentation heap
        if (A(e, 4)) e.mem.write32(A(e, 4), 4);
        ret(e, 1);
    });

    // ---- atoms --------------------------------------------------------------
    api(e, "GlobalAddAtomW", 1, [](Emulator& e) {
        uint64_t p = A(e, 0);
        if (p < 0x10000) return ret(e, p);
        ret(e, gui(e).add_atom(read_wstr(e, p)));
    });
    e.alias_hook("GlobalAddAtomW", "AddAtomW");
    api(e, "GlobalAddAtomA", 1, [](Emulator& e) {
        uint64_t p = A(e, 0);
        if (p < 0x10000) return ret(e, p);
        ret(e, gui(e).add_atom(cp932_to_utf16(e.mem.read_cstring(p))));
    });
    api(e, "GlobalFindAtomW", 1, [](Emulator& e) {
        uint64_t p = A(e, 0);
        if (p < 0x10000) return ret(e, p);
        Gui& g = gui(e);
        auto it = g.atoms.find(upper(read_wstr(e, p)));
        ret(e, it == g.atoms.end() ? 0 : it->second);
    });
    e.alias_hook("GlobalFindAtomW", "FindAtomW");
    api(e, "GlobalDeleteAtom", 1, [](Emulator& e) { ret(e, 0); });
    e.alias_hook("GlobalDeleteAtom", "DeleteAtom");
    api(e, "GlobalGetAtomNameW", 3, [](Emulator& e) {
        Gui& g = gui(e);
        uint32_t a = U(e, 0);
        std::u16string name;
        if (a < 0xC000) name = u"#" + from_utf8(std::to_string(a));
        else {
            auto it = g.atom_names.find(a);
            if (it == g.atom_names.end()) return ret(e, 0);
            name = it->second;
        }
        ret(e, static_cast<uint64_t>(write_wstr(e, A(e, 1), name, I(e, 2))));
    });
    e.alias_hook("GlobalGetAtomNameW", "GetAtomNameW");

    // ---- INI files ------------------------------------------------------------
    auto opt_str = [](Emulator& e, uint64_t p, std::u16string& store) -> const std::u16string* {
        if (!p) return nullptr;
        store = read_wstr(e, p);
        return &store;
    };
    auto get_profile_string = [opt_str](Emulator& e, int file_arg) {
        std::u16string sb, kb;
        const std::u16string* sec = opt_str(e, A(e, 0), sb);
        const std::u16string* key = opt_str(e, A(e, 1), kb);
        std::u16string def = A(e, 2) ? read_wstr(e, A(e, 2)) : u"";
        uint64_t out = A(e, 3);
        int cap = I(e, 4);
        std::u16string file = file_arg >= 0 ? read_wstr(e, A(e, file_arg)) : u"win.ini";
        bool multi = false;
        std::u16string v = profile_string(file, sec, key, def, multi);
        if (!out || cap <= 0) return ret(e, 0);
        if (multi) {
            // Double-NUL-terminated list, truncated to fit.
            int n = std::min(static_cast<int>(v.size()), cap - 2);
            if (n < 0) n = 0;
            for (int i = 0; i < n; ++i) e.mem.write16(out + i * 2, v[i]);
            e.mem.write16(out + n * 2, 0);
            if (n + 1 < cap) e.mem.write16(out + (n + 1) * 2, 0);
            return ret(e, static_cast<uint64_t>(static_cast<int>(v.size()) > cap - 2 ? cap - 2 : n));
        }
        ret(e, static_cast<uint64_t>(write_wstr(e, out, v, cap)));
    };
    api(e, "GetPrivateProfileStringW", 6, [get_profile_string](Emulator& e) { get_profile_string(e, 5); });
    api(e, "GetProfileStringW", 5, [get_profile_string](Emulator& e) { get_profile_string(e, -1); });
    auto get_profile_int = [](Emulator& e, int file_arg) {
        std::u16string sec = read_wstr(e, A(e, 0)), key = read_wstr(e, A(e, 1));
        std::u16string file = file_arg >= 0 ? read_wstr(e, A(e, file_arg)) : u"win.ini";
        bool multi;
        std::u16string v = profile_string(file, &sec, &key, u"\x01", multi);
        if (v == u"\x01") return ret(e, U(e, 2));
        int64_t n = 0;
        bool neg = false;
        size_t i = 0;
        if (i < v.size() && v[i] == '-') {
            neg = true;
            ++i;
        }
        for (; i < v.size() && v[i] >= '0' && v[i] <= '9'; ++i) n = n * 10 + (v[i] - '0');
        ret(e, static_cast<uint32_t>(neg ? -n : n));
    };
    api(e, "GetPrivateProfileIntW", 4, [get_profile_int](Emulator& e) { get_profile_int(e, 3); });
    api(e, "GetProfileIntW", 3, [get_profile_int](Emulator& e) { get_profile_int(e, -1); });
    api(e, "WritePrivateProfileStringW", 4, [opt_str](Emulator& e) {
        std::u16string sb, kb, vb;
        const std::u16string* sec = opt_str(e, A(e, 0), sb);
        const std::u16string* key = opt_str(e, A(e, 1), kb);
        const std::u16string* val = opt_str(e, A(e, 2), vb);
        write_profile(read_wstr(e, A(e, 3)), sec, key, val);
        ret(e, 1);
    });
    api(e, "WriteProfileStringW", 3, [](Emulator& e) { ret(e, 1); });
    api(e, "GetPrivateProfileSectionW", 4, [](Emulator& e) {
        if (A(e, 1) && I(e, 2) >= 2) {
            e.mem.write16(A(e, 1), 0);
            e.mem.write16(A(e, 1) + 2, 0);
        }
        ret(e, 0);
    });

    // ---- directories and files ------------------------------------------------
    api(e, "GetWindowsDirectoryW", 2, [](Emulator& e) {
        ret(e, static_cast<uint64_t>(write_wstr(e, A(e, 0), u"C:\\Windows", I(e, 1))));
    });
    api(e, "GetSystemWindowsDirectoryW", 2, [](Emulator& e) {
        ret(e, static_cast<uint64_t>(write_wstr(e, A(e, 0), u"C:\\Windows", I(e, 1))));
    });
    api(e, "GetSystemDirectoryW", 2, [](Emulator& e) {
        ret(e, static_cast<uint64_t>(write_wstr(e, A(e, 0), u"C:\\Windows\\System32", I(e, 1))));
    });
    api(e, "GetWindowsDirectoryA", 2, [](Emulator& e) {
        if (A(e, 0) && I(e, 1) > 10) e.mem.write_cstring(A(e, 0), "C:\\Windows");
        ret(e, 10);
    });
    api(e, "GetSystemDirectoryA", 2, [](Emulator& e) {
        if (A(e, 0) && I(e, 1) > 19) e.mem.write_cstring(A(e, 0), "C:\\Windows\\System32");
        ret(e, 19);
    });
    api(e, "GetDiskFreeSpaceW", 5, [](Emulator& e) {
        if (A(e, 1)) e.mem.write32(A(e, 1), 8);
        if (A(e, 2)) e.mem.write32(A(e, 2), 512);
        if (A(e, 3)) e.mem.write32(A(e, 3), 0x1000000);
        if (A(e, 4)) e.mem.write32(A(e, 4), 0x2000000);
        ret(e, 1);
    });
    api(e, "GetDiskFreeSpaceExW", 4, [](Emulator& e) {
        for (int i = 1; i <= 3; ++i)
            if (A(e, i)) {
                e.mem.write32(A(e, i), 0);
                e.mem.write32(A(e, i) + 4, 0x10);
            }
        ret(e, 1);
    });
    api(e, "LockFile", 5, [](Emulator& e) { ret(e, 1); });
    api(e, "UnlockFile", 5, [](Emulator& e) { ret(e, 1); });
    api(e, "LockFileEx", 6, [](Emulator& e) { ret(e, 1); });
    api(e, "UnlockFileEx", 5, [](Emulator& e) { ret(e, 1); });
    api(e, "MoveFileW", 2, [](Emulator& e) {
        std::string a = FileTable::host_path(to_utf8(read_wstr(e, A(e, 0))));
        std::string b = FileTable::host_path(to_utf8(read_wstr(e, A(e, 1))));
        ret(e, FileTable::rename_file(a, b) == 0 ? 1 : 0);
    });
    api(e, "ReplaceFileW", 6, [](Emulator& e) {
        std::string to = FileTable::host_path(to_utf8(read_wstr(e, A(e, 0))));
        std::string from = FileTable::host_path(to_utf8(read_wstr(e, A(e, 1))));
        FileTable::remove_file(to);
        ret(e, FileTable::rename_file(from, to) == 0 ? 1 : 0);
    });
    api(e, "GetTempFileNameW", 4, [](Emulator& e) {
        static uint32_t counter = 1;
        std::u16string dir = read_wstr(e, A(e, 0));
        std::u16string pre = read_wstr(e, A(e, 1)).substr(0, 3);
        uint32_t u = U(e, 2) ? U(e, 2) : counter++;
        char hex[16];
        std::snprintf(hex, sizeof hex, "%X", u & 0xFFFF);
        std::u16string name = dir;
        if (!name.empty() && name.back() != u'\\' && name.back() != u'/') name += u'\\';
        name += pre + from_utf8(hex) + u".tmp";
        write_wstr(e, A(e, 3), name, 260);
        if (!U(e, 2)) {
            // A unique name is also a created, empty file.
            if (FILE* fp = std::fopen(FileTable::host_path(to_utf8(name)).c_str(), "wb")) std::fclose(fp);
        }
        ret(e, u & 0xFFFF);
    });

    // ---- time ---------------------------------------------------------------------
    api(e, "FileTimeToSystemTime", 2, [](Emulator& e) {
        uint64_t ft = e.mem.read64(A(e, 0));
        int v[8];
        filetime_to_st(ft, v);
        write_systemtime(e, A(e, 1), v);
        ret(e, 1);
    });
    api(e, "SystemTimeToFileTime", 2, [](Emulator& e) {
        int v[8];
        read_systemtime(e, A(e, 0), v);
        e.mem.write64(A(e, 1), st_to_filetime(v));
        ret(e, 1);
    });
    api(e, "FileTimeToLocalFileTime", 2, [](Emulator& e) {
        e.mem.write64(A(e, 1), e.mem.read64(A(e, 0)) + static_cast<uint64_t>(tz_bias_minutes() * 600000000LL));
        ret(e, 1);
    });
    api(e, "LocalFileTimeToFileTime", 2, [](Emulator& e) {
        e.mem.write64(A(e, 1), e.mem.read64(A(e, 0)) - static_cast<uint64_t>(tz_bias_minutes() * 600000000LL));
        ret(e, 1);
    });
    api(e, "SystemTimeToTzSpecificLocalTime", 3, [](Emulator& e) {
        int v[8];
        read_systemtime(e, A(e, 1), v);
        uint64_t ft = st_to_filetime(v) + static_cast<uint64_t>(tz_bias_minutes() * 600000000LL);
        filetime_to_st(ft, v);
        write_systemtime(e, A(e, 2), v);
        ret(e, 1);
    });
    api(e, "TzSpecificLocalTimeToSystemTime", 3, [](Emulator& e) {
        int v[8];
        read_systemtime(e, A(e, 1), v);
        uint64_t ft = st_to_filetime(v) - static_cast<uint64_t>(tz_bias_minutes() * 600000000LL);
        filetime_to_st(ft, v);
        write_systemtime(e, A(e, 2), v);
        ret(e, 1);
    });
    auto fmt_date = [](Emulator& e, bool time) {
        int v[8];
        if (A(e, 2)) {
            read_systemtime(e, A(e, 2), v);
        } else {
            std::time_t now = std::time(nullptr);
            std::tm lt{};
#if defined(_WIN32)
            localtime_s(&lt, &now);
#else
            localtime_r(&now, &lt);
#endif
            v[0] = lt.tm_year + 1900; v[1] = lt.tm_mon + 1; v[2] = lt.tm_wday; v[3] = lt.tm_mday;
            v[4] = lt.tm_hour; v[5] = lt.tm_min; v[6] = lt.tm_sec; v[7] = 0;
        }
        std::u16string fmt = A(e, 3) ? read_wstr(e, A(e, 3)) : (time ? u"H:mm:ss" : u"yyyy/MM/dd");
        std::u16string out;
        for (size_t i = 0; i < fmt.size();) {
            char16_t c = fmt[i];
            size_t n = 1;
            while (i + n < fmt.size() && fmt[i + n] == c) ++n;
            auto num = [&](int val, size_t width) {
                char b[16];
                std::snprintf(b, sizeof b, "%0*d", static_cast<int>(width), val);
                out += from_utf8(b);
            };
            if (c == '\'') {
                size_t j = fmt.find(u'\'', i + 1);
                if (j == std::u16string::npos) j = fmt.size();
                out += fmt.substr(i + 1, j - i - 1);
                i = j + 1;
                continue;
            }
            if (!time && c == 'y') num(n >= 3 ? v[0] : v[0] % 100, n >= 3 ? 4 : n);
            else if (!time && c == 'M') num(v[1], n >= 2 ? 2 : 1);
            else if (!time && c == 'd' && n <= 2) num(v[3], n);
            else if (!time && c == 'd') {
                static const char16_t* days[] = {u"日", u"月", u"火", u"水", u"木", u"金", u"土"};
                out += days[v[2] % 7];
            } else if (time && (c == 'H' || c == 'h')) num(c == 'H' ? v[4] : (v[4] % 12 ? v[4] % 12 : 12), n);
            else if (time && c == 'm') num(v[5], n);
            else if (time && c == 's') num(v[6], n);
            else if (time && c == 't') out += v[4] < 12 ? u"午前" : u"午後";
            else out += fmt.substr(i, n);
            i += n;
        }
        if (!A(e, 4) || I(e, 5) == 0) return ret(e, out.size() + 1);
        write_wstr(e, A(e, 4), out, I(e, 5));
        ret(e, out.size() + 1);
    };
    api(e, "GetDateFormatW", 6, [fmt_date](Emulator& e) { fmt_date(e, false); });
    api(e, "GetTimeFormatW", 6, [fmt_date](Emulator& e) { fmt_date(e, true); });

    // ---- small things -------------------------------------------------------------
    api(e, "MulDiv", 3, [](Emulator& e) {
        int64_t a = I(e, 0), b = I(e, 1), c = I(e, 2);
        if (c == 0) return ret(e, 0xFFFFFFFF);
        int64_t p = a * b;
        // Rounded half away from zero, as kernel32 does.
        int64_t q = (p >= 0) == (c > 0) ? (std::llabs(p) + std::llabs(c) / 2) / std::llabs(c)
                                         : -((std::llabs(p) + std::llabs(c) / 2) / std::llabs(c));
        ret(e, static_cast<uint32_t>(static_cast<int32_t>(q)));
    });
    api(e, "FreeLibraryAndExitThread", 2, [](Emulator& e) { e.exit_thread(U(e, 1)); });
    api(e, "GetProductInfo", 5, [](Emulator& e) {
        if (A(e, 4)) e.mem.write32(A(e, 4), 0x30);
        ret(e, 1);
    });
    api(e, "Beep", 2, [](Emulator& e) { ret(e, 1); });
    api(e, "SetErrorMode", 1, [](Emulator& e) { ret(e, 0); });
    api(e, "GetErrorMode", 0, [](Emulator& e) { ret(e, 0); });
    api(e, "WinExec", 2, [](Emulator& e) { ret(e, 2); });
    // Activation contexts: one context, always active.
    api(e, "CreateActCtxW", 1, [](Emulator& e) { ret(e, 0x00ACC700); });
    api(e, "CreateActCtxA", 1, [](Emulator& e) { ret(e, 0x00ACC700); });
    api(e, "ActivateActCtx", 2, [](Emulator& e) {
        if (A(e, 1)) e.mem.write32(A(e, 1), 1);
        ret(e, 1);
    });
    api(e, "DeactivateActCtx", 2, [](Emulator& e) { ret(e, 1); });
    api(e, "ReleaseActCtx", 1, [](Emulator& e) { ret(e, 0); });
    api(e, "AddRefActCtx", 1, [](Emulator& e) { ret(e, 0); });
    api(e, "GetCurrentActCtx", 1, [](Emulator& e) {
        if (A(e, 0)) e.mem.write32(A(e, 0), 0);
        ret(e, 1);
    });
    api(e, "FindActCtxSectionStringW", 5, [](Emulator& e) {
        e.set_last_error(2);
        ret(e, 0);
    });
    // MFC asks before every window it creates (afxcomctl32: is there an
    // activation context to switch to?).  Failing here makes CWnd::CreateEx
    // give up before it calls CreateWindowEx at all.  An empty, successful
    // answer says "no special context".
    api(e, "QueryActCtxW", 7, [](Emulator& e) {
        uint64_t buf = A(e, 4);
        uint32_t cb = U(e, 5);
        for (uint32_t i = 0; i + 4 <= cb && buf; i += 4) e.mem.write32(buf + i, 0);
        if (A(e, 6)) e.mem.write32(A(e, 6), cb);
        ret(e, 1);
    });

    // ---- security: one user, an administrator --------------------------------------
    api(e, "OpenProcessToken", 3, [](Emulator& e) {
        if (A(e, 2)) e.mem.write32(A(e, 2), 0x00700F00);
        ret(e, 1);
    });
    api(e, "OpenThreadToken", 4, [](Emulator& e) {
        e.set_last_error(1008);  // ERROR_NO_TOKEN
        ret(e, 0);
    });
    api(e, "AllocateAndInitializeSid", 11, [](Emulator& e) {
        uint64_t sid = e.heap_alloc(8 + 4 * 8);
        uint8_t count = static_cast<uint8_t>(U(e, 1));
        e.mem.write8(sid, 1);
        e.mem.write8(sid + 1, count);
        for (int i = 0; i < 6; ++i) e.mem.write8(sid + 2 + i, e.mem.read8(A(e, 0) + i));
        for (int i = 0; i < count && i < 8; ++i) e.mem.write32(sid + 8 + i * 4, U(e, 2 + i));
        e.mem.write32(A(e, 10), static_cast<uint32_t>(sid));
        ret(e, 1);
    });
    api(e, "FreeSid", 1, [](Emulator& e) {
        if (A(e, 0)) e.heap_free(A(e, 0));
        ret(e, 0);
    });
    api(e, "CheckTokenMembership", 3, [](Emulator& e) {
        if (A(e, 2)) e.mem.write32(A(e, 2), 1);
        ret(e, 1);
    });
    api(e, "GetTokenInformation", 5, [](Emulator& e) {
        if (A(e, 4)) e.mem.write32(A(e, 4), 0);
        e.set_last_error(87);
        ret(e, 0);
    });
    api(e, "NtQueryInformationProcess", 5, [](Emulator& e) {
        // Class 0 (ProcessBasicInformation) is the one anyone asks for.
        if (U(e, 1) == 0 && A(e, 2) && U(e, 3) >= 24) {
            for (int i = 0; i < 24; i += 4) e.mem.write32(A(e, 2) + i, 0);
            e.mem.write32(A(e, 2) + 16, static_cast<uint32_t>(e.pid()));
            if (A(e, 4)) e.mem.write32(A(e, 4), 24);
            return ret(e, 0);
        }
        ret(e, 0xC0000003);  // STATUS_INVALID_INFO_CLASS
    });

    // ---- odds and ends ------------------------------------------------------------------
    api(e, "GetLocalTime", 1, [](Emulator& e) {
        std::time_t now = std::time(nullptr);
        std::tm lt{};
#if defined(_WIN32)
        localtime_s(&lt, &now);
#else
        localtime_r(&now, &lt);
#endif
        int v[8] = {lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_wday, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec, 0};
        write_systemtime(e, A(e, 0), v);
        ret(e, 0);
    });
    api(e, "GetNumberFormatW", 6, [](Emulator& e) {
        std::u16string s = read_wstr(e, A(e, 2));
        if (!A(e, 4) || I(e, 5) == 0) return ret(e, s.size() + 1);
        write_wstr(e, A(e, 4), s, I(e, 5));
        ret(e, s.size() + 1);
    });
    api(e, "EnumCalendarInfoW", 4, [](Emulator& e) { ret(e, 1); });
    api(e, "EnumResourceLanguagesW", 5, [](Emulator& e) {
        // One language per resource: Japanese.
        e.call_guest(A(e, 3), {A(e, 0), A(e, 1), A(e, 2), 0x0411, A(e, 4)});
        ret(e, 1);
    });
    api(e, "FindResourceExA", 4, [](Emulator& e) {
        uint64_t addr;
        uint32_t size;
        // A resource directory entry is what FindResource hands out; the
        // existing FindResourceW hook speaks that form, so ask it.
        uint64_t h = e.existing_hook("FindResourceW");
        if (!h) return ret(e, 0);
        auto arg = [&](int i) -> uint64_t {
            uint64_t p = A(e, i);
            if (p < 0x10000) return p;
            std::u16string w = cp932_to_utf16(e.mem.read_cstring(p));
            uint64_t buf = e.heap_alloc((w.size() + 1) * 2);
            write_wstr(e, buf, w, static_cast<int>(w.size() + 1));
            return buf;
        };
        uint64_t type = arg(1), name = arg(2);
        uint64_t r = e.call_guest(h, {A(e, 0), name, type});
        if (type >= 0x10000) e.heap_free(type);
        if (name >= 0x10000) e.heap_free(name);
        (void)addr;
        (void)size;
        ret(e, r);
    });
    api(e, "CheckForReadOnlyResource", 1, [](Emulator& e) { ret(e, 0); });

    // One drive, C:.  A file dialog that walks A: to Z: would otherwise list
    // twenty-six drives that are all the same place.
    auto drive_type = [](const std::u16string& root) -> uint32_t {
        if (root.empty()) return 3;  // the current drive
        char16_t d = root[0];
        if (d >= u'a' && d <= u'z') d = static_cast<char16_t>(d - 32);
        if (root.size() >= 2 && root[1] == u':') return d == u'C' ? 3 : 1;  // DRIVE_FIXED / DRIVE_NO_ROOT_DIR
        return 3;
    };
    api(e, "GetDriveTypeW", 1, [drive_type](Emulator& e) { ret(e, drive_type(A(e, 0) ? read_wstr(e, A(e, 0)) : u"")); });
    api(e, "GetDriveTypeA", 1, [drive_type](Emulator& e) {
        ret(e, drive_type(A(e, 0) ? cp932_to_utf16(e.mem.read_cstring(A(e, 0))) : u""));
    });
    api(e, "GetLogicalDrives", 0, [](Emulator& e) { ret(e, 1u << 2); });
    api(e, "GetLogicalDriveStringsW", 2, [](Emulator& e) {
        static const char16_t s[] = u"C:\\\0";
        if (U(e, 0) < 5 || !A(e, 1)) return ret(e, 5);
        for (int i = 0; i < 5; ++i) e.mem.write16(A(e, 1) + i * 2, s[i]);
        ret(e, 4);
    });

    // IsBad*Ptr: whether the memory is there, the only honest answer we have.
    auto bad_range = [](Emulator& e, uint64_t p, uint64_t n) {
        if (!p) return n != 0;
        for (uint64_t a = p & ~0xFFFull; a < p + n; a += 0x1000)
            if (!e.mem.is_mapped(a)) return true;
        return false;
    };
    api(e, "IsBadReadPtr", 2, [bad_range](Emulator& e) { ret(e, bad_range(e, A(e, 0), A(e, 1)) ? 1 : 0); });
    api(e, "IsBadWritePtr", 2, [bad_range](Emulator& e) { ret(e, bad_range(e, A(e, 0), A(e, 1)) ? 1 : 0); });
    api(e, "IsBadCodePtr", 1, [bad_range](Emulator& e) { ret(e, bad_range(e, A(e, 0), 1) ? 1 : 0); });
    api(e, "IsBadStringPtrW", 2, [bad_range](Emulator& e) {
        uint64_t p = A(e, 0), max = A(e, 1);
        for (uint64_t i = 0; i < max; ++i) {
            if (bad_range(e, p + i * 2, 2)) return ret(e, 1);
            if (!e.mem.read16(p + i * 2)) break;
        }
        ret(e, 0);
    });
    api(e, "IsBadStringPtrA", 2, [bad_range](Emulator& e) {
        uint64_t p = A(e, 0), max = A(e, 1);
        for (uint64_t i = 0; i < max; ++i) {
            if (bad_range(e, p + i, 1)) return ret(e, 1);
            if (!e.mem.read8(p + i)) break;
        }
        ret(e, 0);
    });

    // Delay-load imports resolved by the loader on first call (Wine's DLLs use
    // them for uxtheme and friends).  The descriptor names the DLL and holds
    // RVAs to its IAT and name table; the thunk being resolved is one IAT slot.
    api(e, "ResolveDelayLoadedAPI", 6, [](Emulator& e) {
        uint64_t base = A(e, 0), desc = A(e, 1), thunk = A(e, 4);
        uint32_t attrs = e.mem.read32(desc);
        bool rva = (attrs & 1) != 0;  // dlattrRva
        auto at = [&](uint32_t v) -> uint64_t { return rva ? base + v : v; };
        std::string dll = e.mem.read_cstring(at(e.mem.read32(desc + 4)));
        uint64_t hmod_slot = at(e.mem.read32(desc + 8));
        uint64_t iat = at(e.mem.read32(desc + 12));
        uint64_t intab = at(e.mem.read32(desc + 16));
        uint64_t index = (thunk - iat) / 4;
        uint32_t entry = e.mem.read32(intab + index * 4);
        uint64_t hmod = e.load_library_for_guest(dll);
        if (hmod_slot) e.mem.write32(hmod_slot, static_cast<uint32_t>(hmod));
        uint64_t addr = 0;
        if (entry & 0x80000000u) {
            uint32_t ord = entry & 0xFFFF;
            if (hmod && e.module_for(hmod)) addr = e.find_export_ordinal(hmod, ord);
            if (!addr) {
                const char* known = Emulator::well_known_ordinal(dll, ord);
                addr = e.resolve_import(dll, known ? known : "#" + std::to_string(ord));
            }
        } else {
            std::string name = e.mem.read_cstring(at(entry) + 2);
            if (hmod && e.module_for(hmod)) addr = e.find_export(hmod, name);
            if (!addr) addr = e.resolve_import(dll, name);
        }
        e.log_call("ResolveDelayLoadedAPI(%s #%llu) = 0x%llX", dll.c_str(), (unsigned long long)index, (unsigned long long)addr);
        e.mem.write32(thunk, static_cast<uint32_t>(addr));
        ret(e, addr);
    });

    // The last stop before a C runtime terminates the process: say why, since
    // the program itself will not.
    api(e, "UnhandledExceptionFilter", 1, [](Emulator& e) {
        uint64_t ep = A(e, 0);
        uint64_t rec = ep ? e.mem.read32(ep) : 0;
        std::fprintf(stderr, "[jwemu] unhandled exception code %08X at %08X\n",
                     rec ? e.mem.read32(rec) : 0, rec ? e.mem.read32(rec + 12) : 0);
        std::fprintf(stderr, "%s", e.stack_trace(24).c_str());
        auto h = e.cpu().history();
        if (!h.empty()) {
            std::fprintf(stderr, "  last instructions:\n");
            size_t from = h.size() > 64 ? h.size() - 64 : 0;
            for (size_t i = from; i < h.size(); ++i) std::fprintf(stderr, "    %08llX\n", (unsigned long long)h[i]);
        }
        ret(e, 1);  // EXCEPTION_EXECUTE_HANDLER
    });
}

}  // namespace x86emu::win32
