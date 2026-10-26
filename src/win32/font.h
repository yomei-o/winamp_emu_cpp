// TrueType text for GDI, rasterised by stb_truetype.
//
// Windows hands a program whatever face it asks for by name; this has a
// handful of freely licensed faces (fonts/) and maps every requested name onto
// one of them: Mincho and serif names to IPAex Mincho / Liberation Serif,
// everything else to IPAex Gothic.  Metrics are derived the way GDI derives
// them from a TrueType face - tmAscent and tmDescent from the OS/2 table's
// usWinAscent/usWinDescent, a negative lfHeight meaning the em size and a
// positive one the cell height - so layout computed from GetTextMetrics and
// GetTextExtentPoint32 behaves like the real thing even though the glyphs
// differ.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "gui.h"

namespace x86emu::win32 {

struct Glyph {
    int advance = 0;   // pixels
    int x0 = 0, y0 = 0;  // bitmap offset from the pen position on the baseline
    int w = 0, h = 0;
    std::vector<uint8_t> alpha;
};

struct FontFile;

struct RealFont {
    FontFile* file = nullptr;
    FontFile* fallback = nullptr;
    float scale = 1;
    float fb_scale = 1;
    int height = 0;      // tmHeight
    int ascent = 0, descent = 0;
    int internal_leading = 0, external_leading = 0;
    int ave_width = 0, max_width = 0;
    int weight = 400;
    bool italic = false, underline = false, strikeout = false;
    int width_override = 0;  // lfWidth: horizontal scale
    float xscale = 1;
    int escapement = 0;  // tenths of a degree
    std::u16string face;  // as the program asked for it
    uint8_t charset = 1;
    uint8_t pitch_family = 0;
    bool monospace_ja = false;  // full-width CJK, half-width ASCII
    std::unordered_map<uint32_t, Glyph> cache;
    const Glyph& glyph(uint32_t cp);
    int advance(uint32_t cp);
};

// The font a LOGFONT selects.  Never null: there is always some face.
RealFont* realize_font(const LogFont& lf);
// For EnumFontFamilies: the faces this machine pretends to have.
struct FaceInfo {
    std::u16string name;
    bool fixed;
    uint8_t charset;
    bool vertical;  // the '@' variant
};
const std::vector<FaceInfo>& font_faces();
// Where font files are read from; set by the front end.
void set_font_dir(const std::string& dir);
bool fonts_available();

}  // namespace x86emu::win32
