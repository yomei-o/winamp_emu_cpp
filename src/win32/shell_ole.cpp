// The thin DLLs: shell32, shlwapi, ole32/oleaut32, uxtheme, imm32, winspool,
// gdiplus, Direct2D/DirectWrite, comdlg32, msvfw32, oleacc, oledlg, winmm,
// and the comctl32 entry points needed when the real DLL is absent.
//
// Most of these answer the way a machine without the feature would: no
// themes, no printers, no Direct2D (Jw_cad falls back to GDI), no COM class
// registrations.  Each answer is one a real Windows can give.
#include <algorithm>
#include <cstring>

#include "files.h"
#include "hookutil.h"
#include "user_internal.h"

namespace x86emu::win32 {

namespace {

constexpr uint32_t E_FAIL = 0x80004005, E_NOTIMPL = 0x80004001, E_NOINTERFACE = 0x80004002,
                   REGDB_E_CLASSNOTREG = 0x80040154, S_OK = 0, S_FALSE = 1;

std::u16string path_dir(const std::u16string& p) {
    size_t s = p.find_last_of(u"\\/");
    return s == std::u16string::npos ? u"" : p.substr(0, s);
}

}  // namespace

void install_shell_ole(Emulator& e) {
    // ---- shell32 ----------------------------------------------------------------
    api(e, "DragAcceptFiles", 2, [](Emulator& e) { ret(e, 0); });
    api(e, "DragFinish", 1, [](Emulator& e) { ret(e, 0); });
    api(e, "DragQueryFileW", 4, [](Emulator& e) { ret(e, 0); });
    api(e, "DragQueryPoint", 2, [](Emulator& e) { ret(e, 0); });
    api(e, "ExtractIconW", 3, [](Emulator& e) { ret(e, 0); });
    api(e, "SHAddToRecentDocs", 2, [](Emulator& e) { ret(e, 0); });
    // MFC's recent-file list looks this up at run time and throws when it is
    // missing (opening a drawing then fails with "invalid argument"); with no
    // shell namespace here, the honest answer is a failed HRESULT.
    // MFC ENSUREs success (a failure is thrown as an invalid argument), so this
    // hands back an inert IShellItem: AddRef/Release count, everything else
    // says not implemented.  SHAddToRecentDocs, the only consumer, ignores it.
    api(e, "SHCreateItemFromParsingName", 4, [](Emulator& e) {
        static uint64_t vtbl = 0;
        if (!vtbl) {
            auto method = [&](const char* name, int nargs, uint32_t result) {
                return e.add_hook(name, nargs * 4, [result](Emulator& e) { ret(e, result); });
            };
            uint64_t qi = e.add_hook("__inert_QueryInterface", 12, [](Emulator& e) {
                if (A(e, 2)) e.mem.write32(A(e, 2), 0);
                ret(e, E_NOINTERFACE);
            });
            uint64_t methods[8] = {
                qi, method("__inert_AddRef", 1, 1), method("__inert_Release", 1, 0),
                method("__inert_BindToHandler", 5, E_NOTIMPL), method("__inert_GetParent", 2, E_NOTIMPL),
                method("__inert_GetDisplayName", 3, E_NOTIMPL), method("__inert_GetAttributes", 3, E_NOTIMPL),
                method("__inert_Compare", 4, E_NOTIMPL),
            };
            vtbl = e.heap_alloc(sizeof methods / 2);
            for (int i = 0; i < 8; ++i) e.mem.write32(vtbl + i * 4, static_cast<uint32_t>(methods[i]));
        }
        uint64_t obj = e.heap_alloc(8);
        e.mem.write32(obj, static_cast<uint32_t>(vtbl));
        if (A(e, 3)) e.mem.write32(A(e, 3), static_cast<uint32_t>(obj));
        ret(e, S_OK);
    });
    api(e, "SHAppBarMessage", 2, [](Emulator& e) { ret(e, 0); });
    api(e, "SHBrowseForFolderW", 1, [](Emulator& e) { ret(e, 0); });
    api(e, "SHGetDesktopFolder", 1, [](Emulator& e) {
        if (A(e, 0)) e.mem.write32(A(e, 0), 0);
        ret(e, E_FAIL);
    });
    api(e, "SHGetFileInfoW", 5, [](Emulator& e) {
        if (A(e, 2)) {
            for (uint32_t i = 0; i < U(e, 3) && i < 692; i += 4) e.mem.write32(A(e, 2) + i, 0);
        }
        ret(e, 0);
    });
    // Special folders: a PIDL is the path itself, as a NUL-terminated wide string.
    auto special_path = [](int csidl) -> std::u16string {
        switch (csidl & 0xFF) {
            case 0x05: case 0x0C: return u"C:\\jww\\Documents";       // CSIDL_PERSONAL / MYDOCUMENTS
            case 0x1A: return u"C:\\jww\\AppData";                    // CSIDL_APPDATA
            case 0x1C: return u"C:\\jww\\AppData";                    // CSIDL_LOCAL_APPDATA
            case 0x23: return u"C:\\jww\\AppData";                    // CSIDL_COMMON_APPDATA
            case 0x00: case 0x10: return u"C:\\jww\\Desktop";         // CSIDL_DESKTOP(DIRECTORY)
            case 0x24: return u"C:\\Windows";
            case 0x25: return u"C:\\Windows\\System32";
            case 0x26: return u"C:\\Program Files";
            case 0x14: return u"C:\\Windows\\Fonts";
        }
        return u"C:\\jww";
    };
    api(e, "SHGetSpecialFolderLocation", 3, [special_path](Emulator& e) {
        std::u16string p = special_path(I(e, 1));
        uint64_t pidl = e.heap_alloc((p.size() + 1) * 2 + 4);
        write_wstr(e, pidl, p, static_cast<int>(p.size() + 1));
        if (A(e, 2)) e.mem.write32(A(e, 2), static_cast<uint32_t>(pidl));
        ret(e, S_OK);
    });
    api(e, "SHGetPathFromIDListW", 2, [](Emulator& e) {
        if (!A(e, 0) || !A(e, 1)) return ret(e, 0);
        write_wstr(e, A(e, 1), read_wstr(e, A(e, 0)), 260);
        ret(e, 1);
    });
    api(e, "SHGetFolderPathW", 5, [special_path](Emulator& e) {
        write_wstr(e, A(e, 4), special_path(I(e, 1)), 260);
        ret(e, S_OK);
    });
    api(e, "SHGetSpecialFolderPathW", 4, [special_path](Emulator& e) {
        write_wstr(e, A(e, 1), special_path(I(e, 2)), 260);
        ret(e, 1);
    });
    api(e, "ILFree", 1, [](Emulator& e) {
        if (A(e, 0)) e.heap_free(A(e, 0));
        ret(e, 0);
    });
    api(e, "ShellExecuteW", 6, [](Emulator& e) {
        std::fprintf(stderr, "[shell] ShellExecute(%s, %s)\n", A(e, 1) ? to_utf8(read_wstr(e, A(e, 1))).c_str() : "",
                     A(e, 2) ? to_utf8(read_wstr(e, A(e, 2))).c_str() : "");
        ret(e, 2);  // SE_ERR_FNF
    });
    api(e, "ShellExecuteExW", 1, [](Emulator& e) { ret(e, 0); });
    api(e, "SHFileOperationW", 1, [](Emulator& e) { ret(e, 1); });
    api(e, "SHChangeNotify", 4, [](Emulator& e) { ret(e, 0); });
    api(e, "CommandLineToArgvW", 2, [](Emulator& e) { ret(e, 0); });

    // ---- shlwapi ------------------------------------------------------------------
    api(e, "PathFindExtensionW", 1, [](Emulator& e) {
        uint64_t p = A(e, 0);
        std::u16string s = read_wstr(e, p);
        size_t dot = s.find_last_of(u'.');
        size_t sep = s.find_last_of(u"\\/ ");
        if (dot == std::u16string::npos || (sep != std::u16string::npos && sep > dot)) return ret(e, p + s.size() * 2);
        ret(e, p + dot * 2);
    });
    api(e, "PathFindFileNameW", 1, [](Emulator& e) {
        uint64_t p = A(e, 0);
        std::u16string s = read_wstr(e, p);
        size_t sep = s.find_last_of(u"\\/:");
        if (sep != std::u16string::npos && sep + 1 == s.size() && sep > 0) {
            size_t prev = s.find_last_of(u"\\/:", sep - 1);
            return ret(e, p + (prev == std::u16string::npos ? 0 : (prev + 1) * 2));
        }
        ret(e, p + (sep == std::u16string::npos ? 0 : (sep + 1) * 2));
    });
    api(e, "PathIsUNCW", 1, [](Emulator& e) {
        std::u16string s = read_wstr(e, A(e, 0));
        ret(e, s.size() >= 2 && s[0] == u'\\' && s[1] == u'\\' ? 1 : 0);
    });
    api(e, "PathRemoveFileSpecW", 1, [](Emulator& e) {
        uint64_t p = A(e, 0);
        std::u16string s = read_wstr(e, p);
        size_t sep = s.find_last_of(u"\\/");
        if (sep == std::u16string::npos) {
            if (s.empty()) return ret(e, 0);
            e.mem.write16(p, 0);
            return ret(e, 1);
        }
        size_t cut = sep;
        if (sep == 2 && s.size() > 1 && s[1] == u':') cut = 3;  // keep "C:\"
        if (sep == 0) cut = 1;
        e.mem.write16(p + cut * 2, 0);
        ret(e, cut != s.size() ? 1 : 0);
    });
    api(e, "PathStripToRootW", 1, [](Emulator& e) {
        uint64_t p = A(e, 0);
        std::u16string s = read_wstr(e, p);
        if (s.size() >= 3 && s[1] == u':' && (s[2] == u'\\' || s[2] == u'/')) {
            e.mem.write16(p + 6, 0);
            return ret(e, 1);
        }
        if (s.size() >= 2 && s[1] == u':') {
            e.mem.write16(p + 4, 0);
            return ret(e, 1);
        }
        ret(e, 0);
    });
    api(e, "PathFileExistsW", 1, [](Emulator& e) {
        FileTable::Stat st;
        ret(e, FileTable::stat_path(FileTable::host_path(to_utf8(read_wstr(e, A(e, 0)))), st) == 0 ? 1 : 0);
    });
    api(e, "PathIsDirectoryW", 1, [](Emulator& e) {
        FileTable::Stat st;
        bool ok = FileTable::stat_path(FileTable::host_path(to_utf8(read_wstr(e, A(e, 0)))), st) == 0 && st.is_dir;
        ret(e, ok ? 0x10 : 0);
    });
    api(e, "PathAddBackslashW", 1, [](Emulator& e) {
        uint64_t p = A(e, 0);
        std::u16string s = read_wstr(e, p);
        if (!s.empty() && s.back() != u'\\') {
            s += u'\\';
            write_wstr(e, p, s, static_cast<int>(s.size() + 1));
        }
        ret(e, p + s.size() * 2);
    });
    api(e, "PathAppendW", 2, [](Emulator& e) {
        uint64_t p = A(e, 0);
        std::u16string s = read_wstr(e, p), more = read_wstr(e, A(e, 1));
        if (!s.empty() && s.back() != u'\\') s += u'\\';
        if (!more.empty() && more[0] == u'\\') more = more.substr(1);
        s += more;
        write_wstr(e, p, s, 260);
        ret(e, 1);
    });
    api(e, "PathCombineW", 3, [](Emulator& e) {
        std::u16string a = A(e, 1) ? read_wstr(e, A(e, 1)) : u"", b = A(e, 2) ? read_wstr(e, A(e, 2)) : u"";
        std::u16string r;
        if (b.size() >= 2 && (b[1] == u':' || (b[0] == u'\\' && b[1] == u'\\'))) r = b;
        else {
            r = a;
            if (!r.empty() && r.back() != u'\\' && !b.empty()) r += u'\\';
            r += b;
        }
        write_wstr(e, A(e, 0), r, 260);
        ret(e, A(e, 0));
    });
    api(e, "PathRemoveExtensionW", 1, [](Emulator& e) {
        uint64_t p = A(e, 0);
        std::u16string s = read_wstr(e, p);
        size_t dot = s.find_last_of(u'.');
        size_t sep = s.find_last_of(u"\\/");
        if (dot != std::u16string::npos && (sep == std::u16string::npos || dot > sep)) e.mem.write16(p + dot * 2, 0);
        ret(e, 0);
    });
    api(e, "PathRenameExtensionW", 2, [](Emulator& e) {
        uint64_t p = A(e, 0);
        std::u16string s = read_wstr(e, p);
        size_t dot = s.find_last_of(u'.');
        size_t sep = s.find_last_of(u"\\/");
        if (dot != std::u16string::npos && (sep == std::u16string::npos || dot > sep)) s = s.substr(0, dot);
        s += read_wstr(e, A(e, 1));
        write_wstr(e, p, s, 260);
        ret(e, 1);
    });
    api(e, "PathIsRelativeW", 1, [](Emulator& e) {
        std::u16string s = read_wstr(e, A(e, 0));
        ret(e, !(s.size() >= 2 && s[1] == u':') && !(s.size() >= 1 && s[0] == u'\\') ? 1 : 0);
    });
    api(e, "StrFormatKBSizeW", 3, [](Emulator& e) {
        uint64_t n = e.mem.read64(e.cpu().regs[4] + 4);
        (void)n;
        uint64_t sz = A(e, 0) | (A(e, 1) << 32);
        std::u16string s = from_utf8(std::to_string((sz + 1023) / 1024) + " KB");
        write_wstr(e, A(e, 2), s, I(e, 3));
        ret(e, A(e, 2));
    });
    api(e, "SHDeleteKeyW", 2, [](Emulator& e) { ret(e, 0); });
    api(e, "SHGetValueW", 6, [](Emulator& e) { ret(e, 2); });
    api(e, "SHSetValueW", 6, [](Emulator& e) { ret(e, 0); });
    api(e, "StrCmpLogicalW", 2, [](Emulator& e) {
        std::u16string a = upper(read_wstr(e, A(e, 0))), b = upper(read_wstr(e, A(e, 1)));
        ret(e, static_cast<uint32_t>(a < b ? -1 : (a == b ? 0 : 1)));
    });

    // ---- ole32 -------------------------------------------------------------------
    for (const char* n : {"CoInitialize", "OleInitialize"}) api(e, n, 1, [](Emulator& e) { ret(e, S_OK); });
    api(e, "CoInitializeEx", 2, [](Emulator& e) { ret(e, S_OK); });
    for (const char* n : {"CoUninitialize", "OleUninitialize", "CoFreeUnusedLibraries"})
        api(e, n, 0, [](Emulator& e) { ret(e, 0); });
    api(e, "CoCreateInstance", 5, [](Emulator& e) {
        if (A(e, 4)) e.mem.write32(A(e, 4), 0);
        ret(e, REGDB_E_CLASSNOTREG);
    });
    api(e, "CoCreateInstanceEx", 6, [](Emulator& e) { ret(e, REGDB_E_CLASSNOTREG); });
    api(e, "CoGetClassObject", 5, [](Emulator& e) {
        if (A(e, 4)) e.mem.write32(A(e, 4), 0);
        ret(e, REGDB_E_CLASSNOTREG);
    });
    api(e, "CoDisconnectObject", 2, [](Emulator& e) { ret(e, S_OK); });
    api(e, "CoLockObjectExternal", 3, [](Emulator& e) { ret(e, S_OK); });
    api(e, "CoRegisterMessageFilter", 2, [](Emulator& e) {
        if (A(e, 1)) e.mem.write32(A(e, 1), 0);
        ret(e, S_OK);
    });
    api(e, "CoRegisterClassObject", 5, [](Emulator& e) {
        if (A(e, 4)) e.mem.write32(A(e, 4), 1);
        ret(e, S_OK);
    });
    api(e, "CoRevokeClassObject", 1, [](Emulator& e) { ret(e, S_OK); });
    api(e, "CoTaskMemAlloc", 1, [](Emulator& e) { ret(e, e.heap_alloc(A(e, 0) ? A(e, 0) : 1)); });
    api(e, "CoTaskMemRealloc", 2, [](Emulator& e) { ret(e, e.heap_realloc(A(e, 0), A(e, 1))); });
    api(e, "CoTaskMemFree", 1, [](Emulator& e) {
        if (A(e, 0)) e.heap_free(A(e, 0));
        ret(e, 0);
    });
    api(e, "CreateStreamOnHGlobal", 3, [](Emulator& e) {
        if (A(e, 2)) e.mem.write32(A(e, 2), 0);
        ret(e, E_NOTIMPL);
    });
    api(e, "DoDragDrop", 4, [](Emulator& e) {
        if (A(e, 3)) e.mem.write32(A(e, 3), 0);
        ret(e, 0x00040101);  // DRAGDROP_S_CANCEL
    });
    api(e, "RegisterDragDrop", 2, [](Emulator& e) { ret(e, S_OK); });
    api(e, "RevokeDragDrop", 1, [](Emulator& e) { ret(e, S_OK); });
    api(e, "IsAccelerator", 4, [](Emulator& e) { ret(e, 0); });
    api(e, "OleCreateMenuDescriptor", 2, [](Emulator& e) { ret(e, 0); });
    api(e, "OleDestroyMenuDescriptor", 1, [](Emulator& e) { ret(e, S_OK); });
    api(e, "OleDuplicateData", 3, [](Emulator& e) { ret(e, 0); });
    api(e, "OleFlushClipboard", 0, [](Emulator& e) { ret(e, S_OK); });
    // The OLE clipboard over the plain one.  OleSetClipboard copies out every
    // format the data object renders into global memory (MFC's
    // COleDataSource::CacheGlobalData - how Jw_cad copies a drawing), and
    // OleGetClipboard hands back a data object that reads the same store, so
    // a paste finds what was copied whichever API it uses.
    //   IDataObject: 3 GetData, 5 QueryGetData, 8 EnumFormatEtc
    //   IEnumFORMATETC: 2 Release, 3 Next
    //   FORMATETC: cfFormat(+0) ptd(+4) dwAspect(+8) lindex(+12) tymed(+16)
    //   STGMEDIUM: tymed(+0) hGlobal(+4) pUnkForRelease(+8)
    api(e, "OleSetClipboard", 1, [](Emulator& e) {
        Gui& g = gui(e);
        uint64_t obj = A(e, 0);
        g.clipboard.clear();
        if (!obj) return ret(e, S_OK);
        auto method = [&](uint64_t self, int index) { return e.mem.read32(e.mem.read32(self) + index * 4); };
        uint64_t penum_slot = e.heap_alloc(4), fmt = e.heap_alloc(20), med = e.heap_alloc(12), fetched = e.heap_alloc(4);
        e.mem.write32(penum_slot, 0);
        if (e.call_guest(method(obj, 8), {obj, 1 /*DATADIR_GET*/, penum_slot}) == S_OK) {
            uint64_t penum = e.mem.read32(penum_slot);
            for (int guard = 0; penum && guard < 64; ++guard) {
                e.mem.write32(fetched, 0);
                if (e.call_guest(method(penum, 3), {penum, 1, fmt, fetched}) != S_OK || !e.mem.read32(fetched)) break;
                uint32_t cf = e.mem.read16(fmt);
                if (!(e.mem.read32(fmt + 16) & 1)) continue;  // TYMED_HGLOBAL only
                e.mem.write32(fmt + 16, 1);
                for (int i = 0; i < 12; i += 4) e.mem.write32(med + i, 0);
                if (e.call_guest(method(obj, 3), {obj, fmt, med}) != S_OK || e.mem.read32(med) != 1) continue;
                uint64_t h = e.mem.read32(med + 4);
                std::vector<uint8_t> data(static_cast<size_t>(h ? e.heap_block_size(h) : 0));
                if (!data.empty()) e.mem.read(h, data.data(), data.size());
                g.clipboard[cf] = std::move(data);
                // ReleaseStgMedium: the owner's Release, or free the memory.
                if (uint64_t unk = e.mem.read32(med + 8)) e.call_guest(method(unk, 2), {unk});
                else if (h) e.heap_free(h);
            }
            if (penum) e.call_guest(method(penum, 2), {penum});
        }
        e.heap_free(penum_slot);
        e.heap_free(fmt);
        e.heap_free(med);
        e.heap_free(fetched);
        e.log_call("OleSetClipboard: %zu formats", g.clipboard.size());
        ret(e, S_OK);
    });
    api(e, "OleGetClipboard", 1, [](Emulator& e) {
        static uint64_t vtbl = 0;
        if (!vtbl) {
            auto fixed = [&](const char* name, int nargs, uint32_t result) {
                return e.add_hook(name, nargs * 4, [result](Emulator& e) { ret(e, result); });
            };
            uint64_t qi = e.add_hook("__clip_QueryInterface", 12, [](Emulator& e) {
                // IUnknown and IDataObject: itself.
                if (A(e, 2)) e.mem.write32(A(e, 2), U(e, 0));
                ret(e, S_OK);
            });
            uint64_t get_data = e.add_hook("__clip_GetData", 12, [](Emulator& e) {
                Gui& g = gui(e);
                uint64_t fmt = A(e, 1), med = A(e, 2);
                auto it = g.clipboard.find(e.mem.read16(fmt));
                if (it == g.clipboard.end() || !(e.mem.read32(fmt + 16) & 1)) return ret(e, 0x80040064);  // DV_E_FORMATETC
                uint64_t h = e.heap_alloc(it->second.empty() ? 1 : it->second.size());
                if (!it->second.empty()) e.mem.write(h, it->second.data(), it->second.size());
                e.mem.write32(med, 1);  // TYMED_HGLOBAL
                e.mem.write32(med + 4, static_cast<uint32_t>(h));
                e.mem.write32(med + 8, 0);
                ret(e, S_OK);
            });
            uint64_t query = e.add_hook("__clip_QueryGetData", 8, [](Emulator& e) {
                Gui& g = gui(e);
                uint64_t fmt = A(e, 1);
                ret(e, g.clipboard.count(e.mem.read16(fmt)) && (e.mem.read32(fmt + 16) & 1) ? S_OK : S_FALSE);
            });
            uint64_t methods[12] = {
                qi, fixed("__clip_AddRef", 1, 1), fixed("__clip_Release", 1, 0), get_data,
                fixed("__clip_GetDataHere", 3, E_NOTIMPL), query, fixed("__clip_GetCanonicalFormatEtc", 3, E_NOTIMPL),
                fixed("__clip_SetData", 4, E_NOTIMPL), fixed("__clip_EnumFormatEtc", 3, E_NOTIMPL),
                fixed("__clip_DAdvise", 5, E_NOTIMPL), fixed("__clip_DUnadvise", 2, E_NOTIMPL),
                fixed("__clip_EnumDAdvise", 2, E_NOTIMPL),
            };
            vtbl = e.heap_alloc(sizeof methods / 2);
            for (int i = 0; i < 12; ++i) e.mem.write32(vtbl + i * 4, static_cast<uint32_t>(methods[i]));
        }
        uint64_t obj = e.heap_alloc(8);
        e.mem.write32(obj, static_cast<uint32_t>(vtbl));
        if (A(e, 0)) e.mem.write32(A(e, 0), static_cast<uint32_t>(obj));
        ret(e, S_OK);
    });
    api(e, "OleIsCurrentClipboard", 1, [](Emulator& e) { ret(e, S_FALSE); });
    api(e, "OleLockRunning", 3, [](Emulator& e) { ret(e, S_OK); });
    api(e, "OleQueryCreateFromData", 1, [](Emulator& e) { ret(e, S_FALSE); });
    api(e, "OleTranslateAccelerator", 3, [](Emulator& e) { ret(e, S_FALSE); });
    api(e, "ReleaseStgMedium", 1, [](Emulator& e) { ret(e, 0); });
    api(e, "StringFromCLSID", 2, [](Emulator& e) {
        uint64_t g = A(e, 0);
        char buf[64];
        std::snprintf(buf, sizeof buf, "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", e.mem.read32(g),
                      e.mem.read16(g + 4), e.mem.read16(g + 6), e.mem.read8(g + 8), e.mem.read8(g + 9),
                      e.mem.read8(g + 10), e.mem.read8(g + 11), e.mem.read8(g + 12), e.mem.read8(g + 13),
                      e.mem.read8(g + 14), e.mem.read8(g + 15));
        std::u16string s = from_utf8(buf);
        uint64_t p = e.heap_alloc((s.size() + 1) * 2);
        write_wstr(e, p, s, static_cast<int>(s.size() + 1));
        e.mem.write32(A(e, 1), static_cast<uint32_t>(p));
        ret(e, S_OK);
    });
    e.alias_hook("StringFromCLSID", "StringFromIID");
    api(e, "OleUIBusyW", 1, [](Emulator& e) { ret(e, 1); });

    // ---- oleaut32 (the ordinals Jw_cad imports beyond what the core has) -----------
    api(e, "VariantCopy", 2, [](Emulator& e) {
        for (int i = 0; i < 16; i += 4) e.mem.write32(A(e, 0) + i, e.mem.read32(A(e, 1) + i));
        ret(e, S_OK);
    });
    api(e, "VariantCopyInd", 2, [](Emulator& e) {
        for (int i = 0; i < 16; i += 4) e.mem.write32(A(e, 0) + i, e.mem.read32(A(e, 1) + i));
        ret(e, S_OK);
    });
    api(e, "VariantChangeType", 4, [](Emulator& e) { ret(e, 0x80020005); });  // DISP_E_TYPEMISMATCH
    api(e, "VariantChangeTypeEx", 5, [](Emulator& e) { ret(e, 0x80020005); });
    api(e, "VarBstrFromDate", 5, [](Emulator& e) { ret(e, E_FAIL); });
    api(e, "LoadTypeLib", 2, [](Emulator& e) { ret(e, 0x80029C4A); });  // TYPE_E_CANTLOADLIBRARY
    api(e, "SystemTimeToVariantTime", 2, [](Emulator& e) { ret(e, 0); });
    api(e, "VariantTimeToSystemTime", 3, [](Emulator& e) { ret(e, 0); });

    // ---- uxtheme: themes are off ------------------------------------------------------
    api(e, "IsAppThemed", 0, [](Emulator& e) { ret(e, 1); });
    api(e, "IsThemeActive", 0, [](Emulator& e) { ret(e, 1); });
    // Themes: only the rebar band MFC's control bars draw (theme.cpp); every
    // other class stays unthemed, so Wine's comctl32 keeps its classic paths.
    auto open_theme = [](Emulator& e) {
        std::u16string list = A(e, 1) ? read_wstr(e, A(e, 1)) : u"";
        uint32_t h = theme_open(list);
        e.log_call("OpenThemeData(%s) = %08X", to_utf8(list).c_str(), h);
        ret(e, h);
    };
    api(e, "OpenThemeData", 2, open_theme);
    api(e, "OpenThemeDataEx", 3, open_theme);
    api(e, "OpenThemeDataForDpi", 3, open_theme);
    api(e, "DrawThemeTextEx", 9, [](Emulator& e) { ret(e, E_FAIL); });
    api(e, "GetThemeBackgroundExtent", 5, [](Emulator& e) { ret(e, E_FAIL); });
    api(e, "GetThemeMargins", 7, [](Emulator& e) { ret(e, E_FAIL); });
    api(e, "CloseThemeData", 1, [](Emulator& e) { ret(e, S_OK); });
    api(e, "GetWindowTheme", 1, [](Emulator& e) { ret(e, 0); });
    api(e, "SetWindowTheme", 3, [](Emulator& e) { ret(e, S_OK); });
    static const std::pair<const char*, int> kTheme[] = {
        {"DrawThemeBackground", 6}, {"DrawThemeText", 9}, {"GetThemeColor", 5}, {"GetThemePartSize", 7},
        {"DrawThemeParentBackground", 3}, {"GetCurrentThemeName", 6}, {"GetThemeBackgroundContentRect", 6},
        {"DrawThemeEdge", 8}, {"GetThemeFont", 6}, {"GetThemeMetric", 6}, {"GetThemeInt", 5},
        {"GetThemeTextExtent", 9}, {"DrawThemeIcon", 7}, {"GetThemeRect", 5},
    };
    for (const auto& [n, k] : kTheme) api(e, n, k, [](Emulator& e) { ret(e, E_FAIL); });
    api(e, "GetThemeSysColor", 2, [](Emulator& e) { ret(e, gui(e).sys_color(I(e, 1))); });
    api(e, "IsThemeBackgroundPartiallyTransparent", 3, [](Emulator& e) { ret(e, 0); });
    api(e, "IsThemePartDefined", 3, [](Emulator& e) { ret(e, 0); });
    api(e, "EnableThemeDialogTexture", 2, [](Emulator& e) { ret(e, S_OK); });
    api(e, "BufferedPaintInit", 0, [](Emulator& e) { ret(e, S_OK); });
    api(e, "BufferedPaintUnInit", 0, [](Emulator& e) { ret(e, S_OK); });

    // ---- imm32 ---------------------------------------------------------------------------
    api(e, "ImmGetContext", 1, [](Emulator& e) { ret(e, 0x00110001); });
    api(e, "ImmReleaseContext", 2, [](Emulator& e) { ret(e, 1); });
    api(e, "ImmGetOpenStatus", 1, [](Emulator& e) { ret(e, 0); });
    api(e, "ImmSetOpenStatus", 2, [](Emulator& e) { ret(e, 1); });
    api(e, "ImmAssociateContext", 2, [](Emulator& e) { ret(e, 0); });
    api(e, "ImmAssociateContextEx", 3, [](Emulator& e) { ret(e, 1); });
    api(e, "ImmGetCompositionStringW", 4, [](Emulator& e) { ret(e, 0); });
    api(e, "ImmSetCompositionWindow", 2, [](Emulator& e) { ret(e, 1); });
    api(e, "ImmSetCompositionFontW", 2, [](Emulator& e) { ret(e, 1); });
    api(e, "ImmNotifyIME", 4, [](Emulator& e) { ret(e, 1); });
    api(e, "ImmGetConversionStatus", 3, [](Emulator& e) {
        if (A(e, 1)) e.mem.write32(A(e, 1), 0);
        if (A(e, 2)) e.mem.write32(A(e, 2), 0);
        ret(e, 1);
    });
    api(e, "ImmSetConversionStatus", 3, [](Emulator& e) { ret(e, 1); });
    api(e, "ImmIsIME", 1, [](Emulator& e) { ret(e, 1); });
    api(e, "ImmGetDefaultIMEWnd", 1, [](Emulator& e) { ret(e, 0); });

    // ---- gdiplus: starts, loads nothing ------------------------------------------------------
    api(e, "GdiplusStartup", 3, [](Emulator& e) {
        if (A(e, 0)) e.mem.write32(A(e, 0), 0x00600DD1);
        if (A(e, 2)) {
            e.mem.write32(A(e, 2), 0);
            e.mem.write32(A(e, 2) + 4, 0);
        }
        ret(e, 0);  // Ok
    });
    api(e, "GdiplusShutdown", 1, [](Emulator& e) { ret(e, 0); });
    api(e, "GdipAlloc", 1, [](Emulator& e) { ret(e, e.heap_alloc(A(e, 0) ? A(e, 0) : 1)); });
    api(e, "GdipFree", 1, [](Emulator& e) {
        if (A(e, 0)) e.heap_free(A(e, 0));
        ret(e, 0);
    });
    static const std::pair<const char*, int> kGdip[] = {
        {"GdipBitmapLockBits", 5}, {"GdipBitmapUnlockBits", 2}, {"GdipCloneImage", 2},
        {"GdipCreateBitmapFromHBITMAP", 3}, {"GdipCreateBitmapFromScan0", 6}, {"GdipCreateBitmapFromStream", 2},
        {"GdipCreateFromHDC", 2}, {"GdipDeleteGraphics", 1}, {"GdipDisposeImage", 1}, {"GdipDrawImageI", 4},
        {"GdipDrawImageRectI", 6}, {"GdipGetImageGraphicsContext", 2}, {"GdipGetImageHeight", 2},
        {"GdipGetImagePalette", 3}, {"GdipGetImagePaletteSize", 2}, {"GdipGetImagePixelFormat", 2},
        {"GdipGetImageWidth", 2}, {"GdipSetInterpolationMode", 2}, {"GdipCreateBitmapFromFile", 2},
        {"GdipLoadImageFromFile", 2}, {"GdipSaveImageToFile", 4}, {"GdipGetImageEncoders", 3},
        {"GdipGetImageEncodersSize", 2},
    };
    // Image work through GDI+ is not there yet: every call reports NotImplemented.
    for (const auto& [n, k] : kGdip) api(e, n, k, [](Emulator& e) { ret(e, 6); });

    // ---- Direct2D / DirectWrite: not present ---------------------------------------------------
    api(e, "D2D1CreateFactory", 4, [](Emulator& e) {
        if (A(e, 3)) e.mem.write32(A(e, 3), 0);
        ret(e, E_NOINTERFACE);
    });
    api(e, "D2D1MakeRotateMatrix", 4, [](Emulator& e) { ret(e, 0); });
    api(e, "D2D1MakeSkewMatrix", 5, [](Emulator& e) { ret(e, 0); });
    api(e, "D2D1IsMatrixInvertible", 1, [](Emulator& e) { ret(e, 1); });
    api(e, "D2D1InvertMatrix", 1, [](Emulator& e) { ret(e, 1); });
    api(e, "DWriteCreateFactory", 3, [](Emulator& e) {
        if (A(e, 2)) e.mem.write32(A(e, 2), 0);
        ret(e, E_NOINTERFACE);
    });

    // ---- msvfw32 / oleacc / winmm ------------------------------------------------------------------
    api(e, "DrawDibOpen", 0, [](Emulator& e) { ret(e, 0x00DD0001); });
    api(e, "DrawDibClose", 1, [](Emulator& e) { ret(e, 1); });
    api(e, "DrawDibRealize", 3, [](Emulator& e) { ret(e, 0); });
    api(e, "DrawDibDraw", 13, [](Emulator& e) {
        // (hdd, hdc, xDst, yDst, dxDst, dyDst, lpbi, lpBits, xSrc, ySrc, dxSrc, dySrc, flags)
        Gui& g = gui(e);
        (void)g;
        // Defer to StretchDIBits semantics via the hook table.
        uint64_t addr = e.existing_hook("StretchDIBits");
        if (!addr) return ret(e, 0);
        int dw = I(e, 4), dh = I(e, 5), sw = I(e, 10), sh = I(e, 11);
        uint64_t bi = A(e, 6);
        if (sw <= 0) sw = static_cast<int32_t>(e.mem.read32(bi + 4));
        if (sh <= 0) sh = std::abs(static_cast<int32_t>(e.mem.read32(bi + 8)));
        if (dw <= 0) dw = sw;
        if (dh <= 0) dh = sh;
        uint64_t bits = A(e, 7);
        if (!bits) {
            uint32_t hs = e.mem.read32(bi);
            int bpp = e.mem.read16(bi + 14);
            uint32_t used = e.mem.read32(bi + 32);
            int ncol = bpp <= 8 ? (used ? static_cast<int>(used) : 1 << bpp) : 0;
            bits = bi + hs + static_cast<uint64_t>(ncol) * 4;
        }
        uint64_t r = e.call_guest(addr, {A(e, 1), static_cast<uint64_t>(I(e, 2)), static_cast<uint64_t>(I(e, 3)),
                                         static_cast<uint64_t>(dw), static_cast<uint64_t>(dh), static_cast<uint64_t>(I(e, 8)),
                                         static_cast<uint64_t>(I(e, 9)), static_cast<uint64_t>(sw), static_cast<uint64_t>(sh),
                                         bits, bi, 0, 0x00CC0020});
        ret(e, r ? 1 : 0);
    });
    api(e, "AccessibleObjectFromWindow", 4, [](Emulator& e) { ret(e, E_FAIL); });
    api(e, "CreateStdAccessibleObject", 4, [](Emulator& e) { ret(e, E_FAIL); });
    api(e, "LresultFromObject", 3, [](Emulator& e) { ret(e, 0); });
    api(e, "PlaySoundW", 3, [](Emulator& e) { ret(e, 1); });
    api(e, "sndPlaySoundW", 2, [](Emulator& e) { ret(e, 1); });

    // ---- comdlg32 ---------------------------------------------------------------------------------
    api(e, "CommDlgExtendedError", 0, [](Emulator& e) { ret(e, 0); });
    api(e, "GetFileTitleW", 3, [](Emulator& e) {
        std::u16string s = read_wstr(e, A(e, 0));
        size_t sep = s.find_last_of(u"\\/:");
        std::u16string t = sep == std::u16string::npos ? s : s.substr(sep + 1);
        if (!A(e, 1) || I(e, 2) <= static_cast<int>(t.size())) return ret(e, t.size() + 1);
        write_wstr(e, A(e, 1), t, I(e, 2));
        ret(e, 0);
    });
    // The file dialogs: the front end may supply a path through JWEMU_OPEN; the
    // browser build replaces this with its own picker later.
    auto file_dialog = [](Emulator& e, bool save) {
        uint64_t ofn = A(e, 0);
        uint64_t file = e.mem.read32(ofn + 28);
        uint32_t cap = e.mem.read32(ofn + 32);
        const char* env = std::getenv(save ? "JWEMU_SAVE" : "JWEMU_OPEN");
        if (!env || !*env || !file || !cap) return ret(e, 0);
        std::u16string p = from_utf8(env);
        write_wstr(e, file, p, static_cast<int>(cap));
        size_t sep = p.find_last_of(u"\\/");
        e.mem.write16(ofn + 56, static_cast<uint16_t>(sep == std::u16string::npos ? 0 : sep + 1));  // nFileOffset
        size_t dot = p.find_last_of(u'.');
        e.mem.write16(ofn + 58, static_cast<uint16_t>(dot == std::u16string::npos ? 0 : dot + 1));  // nFileExtension
        ret(e, 1);
    };
    api(e, "GetOpenFileNameW", 1, [file_dialog](Emulator& e) { file_dialog(e, false); });
    api(e, "GetSaveFileNameW", 1, [file_dialog](Emulator& e) { file_dialog(e, true); });
    api(e, "ChooseColorW", 1, [](Emulator& e) { ret(e, 0); });
    api(e, "ChooseFontW", 1, [](Emulator& e) { ret(e, 0); });
    api(e, "PrintDlgExW", 1, [](Emulator& e) { ret(e, E_FAIL); });
    api(e, "PageSetupDlgW", 1, [](Emulator& e) { ret(e, 0); });
    api(e, "FindTextW", 1, [](Emulator& e) { ret(e, 0); });
    api(e, "ReplaceTextW", 1, [](Emulator& e) { ret(e, 0); });

    // ---- comctl32 fallbacks, used only when the real DLL could not be loaded ------------------------
    api(e, "InitCommonControlsEx", 1, [](Emulator& e) { ret(e, 1); });
    api(e, "InitCommonControls", 0, [](Emulator& e) { ret(e, 0); });
    api(e, "ImageList_AddMasked", 3, [](Emulator& e) { ret(e, static_cast<uint64_t>(-1) & 0xFFFFFFFF); });

    // ---- time: the host's clock, so timers and message times agree ----------------------------------
    api(e, "GetTickCount", 0, [](Emulator& e) { ret(e, static_cast<uint32_t>(gui(e).host->now_ms())); });
    api(e, "GetTickCount64", 0, [](Emulator& e) { ret(e, gui(e).host->now_ms()); });
    api(e, "timeGetTime", 0, [](Emulator& e) { ret(e, static_cast<uint32_t>(gui(e).host->now_ms())); });
    api(e, "QueryPerformanceCounter", 1, [](Emulator& e) {
        if (A(e, 0)) e.mem.write64(A(e, 0), gui(e).host->now_ms() * 1000);
        ret(e, 1);
    });
    api(e, "QueryPerformanceFrequency", 1, [](Emulator& e) {
        if (A(e, 0)) e.mem.write64(A(e, 0), 1000000);
        ret(e, 1);
    });
    api(e, "Sleep", 1, [](Emulator& e) {
        if (e.threads().size() > 1) return e.sleep_current(U(e, 0)), ret(e, 0);
        Gui& g = gui(e);
        uint64_t until = g.host->now_ms() + U(e, 0);
        while (U(e, 0) && g.host->now_ms() < until) g.pump_host(static_cast<int>(until - g.host->now_ms()));
        ret(e, 0);
    });
    (void)path_dir;
}

}  // namespace x86emu::win32
