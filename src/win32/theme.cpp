// uxtheme, just enough of it: the visual style of the parts the real program
// asks for by name, drawn the way Windows 11 draws them.
//
// Themes are opt-in per class.  A class with no entry here gets no handle,
// and its caller falls back to classic drawing - which is what Wine's
// comctl32 needs, since it would otherwise draw every control through parts
// we do not have.  MFC's control bars are the exception: without a "REBAR"
// theme, CControlBar::DrawBorders fills the band with the DC's background
// colour, white, where Windows draws the themed band.
#include <cstring>

#include "gui.h"
#include "hookutil.h"

namespace x86emu::win32 {

namespace {

constexpr uint32_t kRebarTheme = 0x7E3E0001;

constexpr uint32_t S_OK = 0, E_FAIL = 0x80004005;
constexpr int RP_BAND = 3, RP_BACKGROUND = 6;

std::u16string lower(std::u16string s) {
    for (char16_t& c : s)
        if (c >= u'A' && c <= u'Z') c = static_cast<char16_t>(c + 32);
    return s;
}

}  // namespace

uint32_t theme_open(const std::u16string& class_list) {
    // "A;B;C": the first class that has a theme wins.
    std::u16string l = lower(class_list);
    size_t at = 0;
    while (at <= l.size()) {
        size_t end = l.find(u';', at);
        if (end == std::u16string::npos) end = l.size();
        std::u16string name = l.substr(at, end - at);
        if (name == u"rebar") return kRebarTheme;
        at = end + 1;
    }
    return 0;
}

void install_theme(Emulator& e) {
    // (hTheme, hdc, iPartId, iStateId, pRect, pClipRect)
    api(e, "DrawThemeBackground", 6, [](Emulator& e) {
        Gui& g = gui(e);
        if (U(e, 0) != kRebarTheme) return ret(e, E_FAIL);
        int part = I(e, 2);
        Rect r = read_rect(e, A(e, 4));
        if (A(e, 5)) r = r.intersect(read_rect(e, A(e, 5)));
        if (part == RP_BAND || part == RP_BACKGROUND || part == 0) draw::fill_rect(g, U(e, 1), r, 0xFFFFFF);
        ret(e, S_OK);
    });
    api(e, "IsThemeBackgroundPartiallyTransparent", 3, [](Emulator& e) { ret(e, 0); });
    api(e, "IsThemePartDefined", 3, [](Emulator& e) { ret(e, U(e, 0) == kRebarTheme); });
}

}  // namespace x86emu::win32
