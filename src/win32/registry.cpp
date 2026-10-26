// A registry the guest can write to and read back.
//
// Jw_cad keeps its settings under HKCU\Software\Jw_cad\jw_win, so "not
// present" (what the console emulator answers) would lose every setting at
// exit.  This is a tree in memory, saved to a text file whenever a key is
// closed after a change and loaded at start.  The file path comes from the
// front end (JWEMU_REGISTRY, default jwemu_registry.txt).
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

#include "hookutil.h"

namespace x86emu::win32 {

namespace {

struct Value {
    std::u16string name;
    uint32_t type = 1;
    std::vector<uint8_t> data;
};

struct Key {
    std::u16string name;
    std::map<std::u16string, std::unique_ptr<Key>> subkeys;  // by upper-cased name
    std::vector<Value> values;
    Key* parent = nullptr;
};

struct Registry {
    Key roots[4];  // HKCR, HKCU, HKLM, HKU
    std::map<uint32_t, Key*> handles;
    uint32_t next = 0x00600010;
    bool dirty = false;
    std::string path;
    bool loaded = false;
};

}  // namespace

std::function<void(const std::string&)> registry_saved_hook;

namespace {

Registry& reg() {
    static Registry r;
    return r;
}

Key* key_from_handle(uint64_t h) {
    Registry& r = reg();
    if (h >= 0x80000000ull && h <= 0x80000003ull) return &r.roots[h - 0x80000000ull];
    if (h == 0x80000005ull) return &r.roots[2];  // HKEY_CURRENT_CONFIG
    auto it = r.handles.find(static_cast<uint32_t>(h));
    return it == r.handles.end() ? nullptr : it->second;
}

std::vector<std::u16string> split_path(const std::u16string& p) {
    std::vector<std::u16string> out;
    std::u16string cur;
    for (char16_t c : p) {
        if (c == u'\\') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

Key* walk(Key* k, const std::u16string& path, bool create) {
    for (const auto& part : split_path(path)) {
        std::u16string up = upper(part);
        auto it = k->subkeys.find(up);
        if (it == k->subkeys.end()) {
            if (!create) return nullptr;
            auto nk = std::make_unique<Key>();
            nk->name = part;
            nk->parent = k;
            Key* raw = nk.get();
            k->subkeys[up] = std::move(nk);
            reg().dirty = true;
            k = raw;
        } else {
            k = it->second.get();
        }
    }
    return k;
}

Value* find_value(Key* k, const std::u16string& name) {
    std::u16string up = upper(name);
    for (auto& v : k->values)
        if (upper(v.name) == up) return &v;
    return nullptr;
}

uint32_t open_handle(Key* k) {
    Registry& r = reg();
    uint32_t h = r.next;
    r.next += 4;
    r.handles[h] = k;
    return h;
}

// ---- persistence: one line per key or value, everything hex-escaped ----------

std::string hex(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) {
        s.push_back(d[p[i] >> 4]);
        s.push_back(d[p[i] & 15]);
    }
    return s;
}

std::vector<uint8_t> unhex(const std::string& s) {
    std::vector<uint8_t> out;
    auto v = [](char c) { return c >= 'a' ? c - 'a' + 10 : (c >= 'A' ? c - 'A' + 10 : c - '0'); };
    for (size_t i = 0; i + 1 < s.size(); i += 2) out.push_back(static_cast<uint8_t>(v(s[i]) << 4 | v(s[i + 1])));
    return out;
}

std::string key_path(Key* k) {
    std::vector<std::u16string> parts;
    while (k && k->parent) {
        parts.push_back(k->name);
        k = k->parent;
    }
    std::u16string p;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        if (!p.empty()) p += u'\\';
        p += *it;
    }
    return to_utf8(p);
}

void save_key(FILE* fp, int root, Key* k) {
    if (k->parent) std::fprintf(fp, "K %d %s\n", root, key_path(k).c_str());
    for (auto& v : k->values) {
        std::string n = to_utf8(v.name);
        std::fprintf(fp, "V %u %s %s\n", v.type, hex(reinterpret_cast<const uint8_t*>(n.data()), n.size()).c_str(),
                     hex(v.data.data(), v.data.size()).c_str());
    }
    for (auto& [name, sub] : k->subkeys) save_key(fp, root, sub.get());
}

void save() {
    Registry& r = reg();
    if (!r.dirty || r.path.empty()) return;
    FILE* fp = std::fopen(r.path.c_str(), "wb");
    if (!fp) return;
    std::fprintf(fp, "jwemu-registry 1\n");
    for (int i = 0; i < 4; ++i) {
        std::fprintf(fp, "K %d \n", i);
        save_key(fp, i, &r.roots[i]);
    }
    std::fclose(fp);
    r.dirty = false;
    if (registry_saved_hook) registry_saved_hook(r.path);
}

void load() {
    Registry& r = reg();
    if (r.loaded) return;
    r.loaded = true;
    const char* env = std::getenv("JWEMU_REGISTRY");
    r.path = env && *env ? env : "jwemu_registry.txt";
    FILE* fp = std::fopen(r.path.c_str(), "rb");
    if (!fp) return;
    char line[1 << 16];
    Key* cur = nullptr;
    while (std::fgets(line, sizeof line, fp)) {
        std::string s = line;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (s.size() > 2 && s[0] == 'K') {
            int root = s[2] - '0';
            if (root < 0 || root > 3) continue;
            std::string path = s.size() > 4 ? s.substr(4) : "";
            cur = walk(&r.roots[root], from_utf8(path), true);
        } else if (s.size() > 2 && s[0] == 'V' && cur) {
            size_t a = s.find(' ', 2), b = s.find(' ', a + 1);
            if (a == std::string::npos || b == std::string::npos) continue;
            Value v;
            v.type = static_cast<uint32_t>(std::strtoul(s.substr(2, a - 2).c_str(), nullptr, 10));
            auto nb = unhex(s.substr(a + 1, b - a - 1));
            v.name = from_utf8(std::string(nb.begin(), nb.end()));
            v.data = unhex(s.substr(b + 1));
            cur->values.push_back(std::move(v));
        }
    }
    std::fclose(fp);
    r.dirty = false;
}

std::u16string read_name(Emulator& e, uint64_t p, bool wide) {
    if (!p) return u"";
    return wide ? read_wstr(e, p) : cp932_to_utf16(e.mem.read_cstring(p));
}

}  // namespace

void registry_flush() { save(); }

void install_registry(Emulator& e) {
    load();
    auto open_key = [](Emulator& e, bool wide, bool create, int out_arg) {
        Key* base = key_from_handle(A(e, 0));
        if (!base) return ret(e, 6);  // ERROR_INVALID_HANDLE
        std::u16string sub = read_name(e, A(e, 1), wide);
        if (base == &reg().roots[0] && upper(sub) == u".JWW" && !walk(base, sub, false)) {
            // What Jw_cad's installer leaves behind: .jww associated with this
            // executable.  Without it every start asks to run install.exe.
            uint64_t h = e.existing_hook("GetModuleFileNameW");
            std::u16string exe = u"Jw_win.exe";
            if (h) {
                uint64_t buf = e.heap_alloc(520);
                e.call_guest(h, {0, buf, 260});
                exe = read_wstr(e, buf);
                e.heap_free(buf);
            }
            auto set_default = [](Key* k, const std::u16string& v) {
                Value val{u"", 1, {}};
                for (char16_t c : v) {
                    val.data.push_back(static_cast<uint8_t>(c & 0xFF));
                    val.data.push_back(static_cast<uint8_t>(c >> 8));
                }
                val.data.push_back(0);
                val.data.push_back(0);
                k->values.push_back(val);
            };
            set_default(walk(base, u".jww", true), u"jwwfile");
            set_default(walk(base, u"jwwfile\\shell\\open\\command", true), u"\"" + exe + u"\" \"%1\"");
            reg().dirty = true;
        }
        Key* k = walk(base, sub, create);
        e.log_call("RegOpenKey(%llX, %s) -> %s", (unsigned long long)A(e, 0), to_utf8(sub).c_str(), k ? "ok" : "missing");
        if (!k) {
            if (A(e, out_arg)) e.mem.write32(A(e, out_arg), 0);
            return ret(e, 2);  // ERROR_FILE_NOT_FOUND
        }
        e.mem.write32(A(e, out_arg), open_handle(k));
        ret(e, 0);
    };
    api(e, "RegOpenKeyExW", 5, [open_key](Emulator& e) { open_key(e, true, false, 4); });
    api(e, "RegOpenKeyExA", 5, [open_key](Emulator& e) { open_key(e, false, false, 4); });
    api(e, "RegOpenKeyW", 3, [open_key](Emulator& e) { open_key(e, true, false, 2); });
    api(e, "RegOpenKeyA", 3, [open_key](Emulator& e) { open_key(e, false, false, 2); });
    api(e, "RegCreateKeyW", 3, [open_key](Emulator& e) { open_key(e, true, true, 2); });
    api(e, "RegCreateKeyA", 3, [open_key](Emulator& e) { open_key(e, false, true, 2); });
    auto create_ex = [](Emulator& e, bool wide) {
        Key* base = key_from_handle(A(e, 0));
        if (!base) return ret(e, 6);
        std::u16string sub = read_name(e, A(e, 1), wide);
        bool existed = walk(base, sub, false) != nullptr;
        Key* k = walk(base, sub, true);
        e.mem.write32(A(e, 7), open_handle(k));
        if (A(e, 8)) e.mem.write32(A(e, 8), existed ? 2 : 1);  // REG_OPENED_EXISTING_KEY / CREATED_NEW_KEY
        ret(e, 0);
    };
    api(e, "RegCreateKeyExW", 9, [create_ex](Emulator& e) { create_ex(e, true); });
    api(e, "RegCreateKeyExA", 9, [create_ex](Emulator& e) { create_ex(e, false); });
    api(e, "RegCloseKey", 1, [](Emulator& e) {
        reg().handles.erase(U(e, 0));
        save();
        ret(e, 0);
    });
    api(e, "RegFlushKey", 1, [](Emulator& e) {
        save();
        ret(e, 0);
    });
    auto query = [](Emulator& e, bool wide) {
        Key* k = key_from_handle(A(e, 0));
        if (!k) return ret(e, 6);
        Value* v = find_value(k, read_name(e, A(e, 1), wide));
        if (!v) return ret(e, 2);
        std::vector<uint8_t> data = v->data;
        if (!wide && (v->type == 1 || v->type == 2 || v->type == 7)) {
            std::u16string w;
            for (size_t i = 0; i + 1 < data.size(); i += 2) w.push_back(static_cast<char16_t>(data[i] | data[i + 1] << 8));
            std::string a = utf16_to_cp932(w);
            data.assign(a.begin(), a.end());
        }
        if (A(e, 3)) e.mem.write32(A(e, 3), v->type);
        uint64_t buf = A(e, 4), lenp = A(e, 5);
        if (!lenp) return ret(e, buf ? 87 : 0);
        uint32_t cap = e.mem.read32(lenp);
        e.mem.write32(lenp, static_cast<uint32_t>(data.size()));
        if (!buf) return ret(e, 0);
        if (cap < data.size()) return ret(e, 234);  // ERROR_MORE_DATA
        if (!data.empty()) e.mem.write(buf, data.data(), data.size());
        ret(e, 0);
    };
    api(e, "RegQueryValueExW", 6, [query](Emulator& e) { query(e, true); });
    api(e, "RegQueryValueExA", 6, [query](Emulator& e) { query(e, false); });
    api(e, "RegGetValueW", 7, [](Emulator& e) {
        Key* k = key_from_handle(A(e, 0));
        if (!k) return ret(e, 6);
        if (A(e, 1)) k = walk(k, read_wstr(e, A(e, 1)), false);
        if (!k) return ret(e, 2);
        Value* v = find_value(k, A(e, 2) ? read_wstr(e, A(e, 2)) : u"");
        if (!v) return ret(e, 2);
        if (A(e, 4)) e.mem.write32(A(e, 4), v->type);
        uint64_t buf = A(e, 5), lenp = A(e, 6);
        if (lenp) {
            uint32_t cap = e.mem.read32(lenp);
            e.mem.write32(lenp, static_cast<uint32_t>(v->data.size()));
            if (buf && cap < v->data.size()) return ret(e, 234);
        }
        if (buf && !v->data.empty()) e.mem.write(buf, v->data.data(), v->data.size());
        ret(e, 0);
    });
    auto set_value = [](Emulator& e, bool wide) {
        Key* k = key_from_handle(A(e, 0));
        if (!k) return ret(e, 6);
        std::u16string name = read_name(e, A(e, 1), wide);
        uint32_t type = U(e, 3);
        uint32_t len = U(e, 5);
        std::vector<uint8_t> data(len);
        if (len) e.mem.read(A(e, 4), data.data(), len);
        if (!wide && (type == 1 || type == 2 || type == 7)) {
            std::string a(data.begin(), data.end());
            std::u16string w = cp932_to_utf16(a);
            data.clear();
            for (char16_t c : w) {
                data.push_back(static_cast<uint8_t>(c & 0xFF));
                data.push_back(static_cast<uint8_t>(c >> 8));
            }
        }
        Value* v = find_value(k, name);
        if (!v) {
            k->values.push_back({name, type, {}});
            v = &k->values.back();
        }
        v->type = type;
        v->data = std::move(data);
        reg().dirty = true;
        ret(e, 0);
    };
    api(e, "RegSetValueExW", 6, [set_value](Emulator& e) { set_value(e, true); });
    api(e, "RegSetValueExA", 6, [set_value](Emulator& e) { set_value(e, false); });
    // The 16-bit pair: the default value of a subkey, always REG_SZ.
    api(e, "RegQueryValueW", 4, [](Emulator& e) {
        Key* k = key_from_handle(A(e, 0));
        if (!k) return ret(e, 6);
        if (A(e, 1)) k = walk(k, read_wstr(e, A(e, 1)), false);
        if (!k) return ret(e, 2);
        Value* v = find_value(k, u"");
        std::vector<uint8_t> data = v ? v->data : std::vector<uint8_t>{0, 0};
        uint64_t lenp = A(e, 3);
        if (lenp) {
            uint32_t cap = e.mem.read32(lenp);
            e.mem.write32(lenp, static_cast<uint32_t>(data.size()));
            if (A(e, 2) && cap < data.size()) return ret(e, 234);
        }
        if (A(e, 2) && !data.empty()) e.mem.write(A(e, 2), data.data(), data.size());
        ret(e, 0);
    });
    api(e, "RegSetValueW", 5, [](Emulator& e) {
        Key* k = key_from_handle(A(e, 0));
        if (!k) return ret(e, 6);
        if (A(e, 1)) k = walk(k, read_wstr(e, A(e, 1)), true);
        std::u16string s = read_wstr(e, A(e, 3));
        std::vector<uint8_t> data;
        for (char16_t c : s) {
            data.push_back(static_cast<uint8_t>(c & 0xFF));
            data.push_back(static_cast<uint8_t>(c >> 8));
        }
        data.push_back(0);
        data.push_back(0);
        Value* v = find_value(k, u"");
        if (!v) {
            k->values.push_back({u"", 1, {}});
            v = &k->values.back();
        }
        v->type = 1;
        v->data = data;
        reg().dirty = true;
        ret(e, 0);
    });
    api(e, "RegDeleteValueW", 2, [](Emulator& e) {
        Key* k = key_from_handle(A(e, 0));
        if (!k) return ret(e, 6);
        std::u16string up = upper(read_wstr(e, A(e, 1)));
        auto it = std::find_if(k->values.begin(), k->values.end(), [&](const Value& v) { return upper(v.name) == up; });
        if (it == k->values.end()) return ret(e, 2);
        k->values.erase(it);
        reg().dirty = true;
        ret(e, 0);
    });
    auto delete_key = [](Emulator& e) {
        Key* k = key_from_handle(A(e, 0));
        if (!k) return ret(e, 6);
        auto parts = split_path(read_wstr(e, A(e, 1)));
        if (parts.empty()) return ret(e, 2);
        std::u16string last = parts.back();
        parts.pop_back();
        for (auto& p : parts) {
            auto it = k->subkeys.find(upper(p));
            if (it == k->subkeys.end()) return ret(e, 2);
            k = it->second.get();
        }
        auto it = k->subkeys.find(upper(last));
        if (it == k->subkeys.end()) return ret(e, 2);
        for (auto h = reg().handles.begin(); h != reg().handles.end();) {
            Key* x = h->second;
            bool inside = false;
            for (Key* y = x; y; y = y->parent)
                if (y == it->second.get()) inside = true;
            h = inside ? reg().handles.erase(h) : std::next(h);
        }
        k->subkeys.erase(it);
        reg().dirty = true;
        ret(e, 0);
    };
    api(e, "RegDeleteKeyW", 2, delete_key);
    api(e, "RegDeleteTreeW", 2, delete_key);
    auto enum_key = [](Emulator& e, bool ex) {
        Key* k = key_from_handle(A(e, 0));
        if (!k) return ret(e, 6);
        uint32_t idx = U(e, 1);
        if (idx >= k->subkeys.size()) return ret(e, 259);  // ERROR_NO_MORE_ITEMS
        auto it = k->subkeys.begin();
        std::advance(it, idx);
        const std::u16string& name = it->second->name;
        if (ex) {
            uint32_t cap = e.mem.read32(A(e, 3));
            if (cap <= name.size()) return ret(e, 234);
            write_wstr(e, A(e, 2), name, static_cast<int>(cap));
            e.mem.write32(A(e, 3), static_cast<uint32_t>(name.size()));
        } else {
            if (U(e, 3) <= name.size()) return ret(e, 234);
            write_wstr(e, A(e, 2), name, I(e, 3));
        }
        ret(e, 0);
    };
    api(e, "RegEnumKeyExW", 8, [enum_key](Emulator& e) { enum_key(e, true); });
    api(e, "RegEnumKeyW", 4, [enum_key](Emulator& e) { enum_key(e, false); });
    api(e, "RegEnumValueW", 8, [](Emulator& e) {
        Key* k = key_from_handle(A(e, 0));
        if (!k) return ret(e, 6);
        uint32_t idx = U(e, 1);
        if (idx >= k->values.size()) return ret(e, 259);
        Value& v = k->values[idx];
        uint32_t cap = e.mem.read32(A(e, 3));
        if (cap <= v.name.size()) return ret(e, 234);
        write_wstr(e, A(e, 2), v.name, static_cast<int>(cap));
        e.mem.write32(A(e, 3), static_cast<uint32_t>(v.name.size()));
        if (A(e, 5)) e.mem.write32(A(e, 5), v.type);
        if (A(e, 7)) {
            uint32_t dcap = e.mem.read32(A(e, 7));
            e.mem.write32(A(e, 7), static_cast<uint32_t>(v.data.size()));
            if (A(e, 6)) {
                if (dcap < v.data.size()) return ret(e, 234);
                if (!v.data.empty()) e.mem.write(A(e, 6), v.data.data(), v.data.size());
            }
        }
        ret(e, 0);
    });
    api(e, "RegQueryInfoKeyW", 12, [](Emulator& e) {
        Key* k = key_from_handle(A(e, 0));
        if (!k) return ret(e, 6);
        size_t maxsub = 0, maxval = 0, maxdata = 0;
        for (auto& [n, s] : k->subkeys) maxsub = std::max(maxsub, s->name.size());
        for (auto& v : k->values) {
            maxval = std::max(maxval, v.name.size());
            maxdata = std::max(maxdata, v.data.size());
        }
        auto put = [&](int i, size_t v) {
            if (A(e, i)) e.mem.write32(A(e, i), static_cast<uint32_t>(v));
        };
        if (A(e, 2)) e.mem.write32(A(e, 2), 0);
        put(4, k->subkeys.size());
        put(5, maxsub);
        put(6, 0);
        put(7, k->values.size());
        put(8, maxval);
        put(9, maxdata);
        put(10, 0);
        if (A(e, 11)) e.mem.write64(A(e, 11), 0);
        ret(e, 0);
    });
    api(e, "RegNotifyChangeKeyValue", 5, [](Emulator& e) { ret(e, 0); });
    api(e, "RegOverridePredefKey", 2, [](Emulator& e) { ret(e, 0); });
    api(e, "RegOpenCurrentUser", 2, [](Emulator& e) {
        e.mem.write32(A(e, 1), 0x80000001);
        ret(e, 0);
    });
    api(e, "GetFileSecurityW", 5, [](Emulator& e) {
        if (A(e, 4)) e.mem.write32(A(e, 4), 0);
        e.set_last_error(50);
        ret(e, 0);
    });
    api(e, "SetFileSecurityW", 3, [](Emulator& e) { ret(e, 1); });
}

}  // namespace x86emu::win32
