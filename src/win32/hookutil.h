// Small helpers shared by the hook files of the GUI subsystem.
#pragma once

#include <cstdint>
#include <functional>

#include "emulator.h"
#include "gui.h"

namespace x86emu::win32 {

// Registers a stdcall Win32 entry point taking `nargs` 4-byte arguments.
inline void api(Emulator& e, const char* name, int nargs, std::function<void(Emulator&)> fn) {
    e.add_hook(name, e.is64() ? 0 : nargs * 4, std::move(fn));
}
// A cdecl one (wsprintf and friends).
inline void api_cdecl(Emulator& e, const char* name, std::function<void(Emulator&)> fn) {
    e.add_hook(name, 0, std::move(fn));
}

inline uint64_t A(Emulator& e, int i) { return e.arg_slot(i); }
inline int32_t I(Emulator& e, int i) { return static_cast<int32_t>(e.arg_slot(i)); }
inline uint32_t U(Emulator& e, int i) { return static_cast<uint32_t>(e.arg_slot(i)); }
inline void ret(Emulator& e, uint64_t v) { e.set_result(v); }

inline Rect read_rect(Emulator& e, uint64_t p) {
    return {static_cast<int32_t>(e.mem.read32(p)), static_cast<int32_t>(e.mem.read32(p + 4)),
            static_cast<int32_t>(e.mem.read32(p + 8)), static_cast<int32_t>(e.mem.read32(p + 12))};
}
inline void write_rect(Emulator& e, uint64_t p, const Rect& r) {
    if (!p) return;
    e.mem.write32(p, static_cast<uint32_t>(r.l));
    e.mem.write32(p + 4, static_cast<uint32_t>(r.t));
    e.mem.write32(p + 8, static_cast<uint32_t>(r.r));
    e.mem.write32(p + 12, static_cast<uint32_t>(r.b));
}
inline Point read_point(Emulator& e, uint64_t p) {
    return {static_cast<int32_t>(e.mem.read32(p)), static_cast<int32_t>(e.mem.read32(p + 4))};
}
inline void write_point(Emulator& e, uint64_t p, Point pt) {
    if (!p) return;
    e.mem.write32(p, static_cast<uint32_t>(pt.x));
    e.mem.write32(p + 4, static_cast<uint32_t>(pt.y));
}
inline int16_t lo16(uint64_t v) { return static_cast<int16_t>(v & 0xFFFF); }
inline int16_t hi16(uint64_t v) { return static_cast<int16_t>((v >> 16) & 0xFFFF); }
inline uint32_t make_lparam(int lo, int hi) {
    return (static_cast<uint32_t>(hi & 0xFFFF) << 16) | static_cast<uint32_t>(lo & 0xFFFF);
}

}  // namespace x86emu::win32
