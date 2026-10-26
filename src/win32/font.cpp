#include "font.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

namespace x86emu::win32 {

struct FontFile {
    std::string path;
    std::vector<uint8_t> data;
    stbtt_fontinfo info{};
    bool ok = false;
    int units_per_em = 2048;
    int win_ascent = 0, win_descent = 0;  // OS/2, font units
    int hhea_ascent = 0, hhea_descent = 0, line_gap = 0;
    int avg_char_width = 0;               // OS/2 xAvgCharWidth
    int max_advance = 0;
};

namespace {

std::string g_font_dir = "fonts";

uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>(p[0] << 8 | p[1]); }
uint32_t be32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

const uint8_t* find_table(const FontFile& f, const char* tag, uint32_t* len) {
    const uint8_t* d = f.data.data();
    if (f.data.size() < 12) return nullptr;
    int n = be16(d + 4);
    for (int i = 0; i < n; ++i) {
        const uint8_t* rec = d + 12 + 16 * i;
        if (rec + 16 > d + f.data.size()) break;
        if (std::memcmp(rec, tag, 4) == 0) {
            uint32_t off = be32(rec + 8);
            if (len) *len = be32(rec + 12);
            if (off >= f.data.size()) return nullptr;
            return d + off;
        }
    }
    return nullptr;
}

FontFile* load_file(const std::string& name) {
    static std::unordered_map<std::string, std::unique_ptr<FontFile>> files;
    auto it = files.find(name);
    if (it != files.end()) return it->second->ok ? it->second.get() : nullptr;
    auto f = std::make_unique<FontFile>();
    f->path = g_font_dir + "/" + name;
    if (FILE* fp = std::fopen(f->path.c_str(), "rb")) {
        std::fseek(fp, 0, SEEK_END);
        long sz = std::ftell(fp);
        std::fseek(fp, 0, SEEK_SET);
        if (sz > 0) {
            f->data.resize(static_cast<size_t>(sz));
            if (std::fread(f->data.data(), 1, f->data.size(), fp) != f->data.size())
                f->data.clear();
        }
        std::fclose(fp);
    }
    if (!f->data.empty()) {
        int off = stbtt_GetFontOffsetForIndex(f->data.data(), 0);
        if (off >= 0 && stbtt_InitFont(&f->info, f->data.data(), off)) {
            f->ok = true;
            uint32_t len = 0;
            if (const uint8_t* head = find_table(*f, "head", &len)) f->units_per_em = be16(head + 18);
            stbtt_GetFontVMetrics(&f->info, &f->hhea_ascent, &f->hhea_descent, &f->line_gap);
            f->win_ascent = f->hhea_ascent;
            f->win_descent = -f->hhea_descent;
            if (const uint8_t* os2 = find_table(*f, "OS/2", &len)) {
                if (len >= 78) {
                    f->avg_char_width = static_cast<int16_t>(be16(os2 + 2));
                    f->win_ascent = be16(os2 + 74);
                    f->win_descent = be16(os2 + 76);
                }
            }
            if (const uint8_t* hhea = find_table(*f, "hhea", &len)) f->max_advance = be16(hhea + 10);
        }
    }
    FontFile* raw = f.get();
    files[name] = std::move(f);
    return raw->ok ? raw : nullptr;
}

bool contains(const std::u16string& s, const char16_t* needle) {
    return s.find(needle) != std::u16string::npos;
}

std::u16string lower_ascii(std::u16string s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char16_t>(c + 32);
    return s;
}

}  // namespace

void set_font_dir(const std::string& dir) { g_font_dir = dir; }

bool fonts_available() { return load_file("jwsubgothic.ttf") != nullptr; }

const std::vector<FaceInfo>& font_faces() {
    static const std::vector<FaceInfo> faces = {
        {u"ＭＳ ゴシック", true, 128, false},
        {u"ＭＳ Ｐゴシック", false, 128, false},
        {u"ＭＳ 明朝", true, 128, false},
        {u"ＭＳ Ｐ明朝", false, 128, false},
        {u"MS UI Gothic", false, 128, false},
        {u"@ＭＳ ゴシック", true, 128, true},
        {u"@ＭＳ 明朝", true, 128, true},
        {u"IPAexゴシック", true, 128, false},
        {u"IPAex明朝", true, 128, false},
        {u"BIZ UDゴシック", true, 128, false},
        {u"Yu Gothic UI", false, 128, false},
        {u"Meiryo UI", false, 128, false},
        {u"Arial", false, 0, false},
        {u"Times New Roman", false, 0, false},
        {u"Courier New", true, 0, false},
        {u"Century", false, 0, false},
        {u"Tahoma", false, 0, false},
        {u"Segoe UI", false, 0, false},
    };
    return faces;
}

// Which file stands in for a face name.
static FontFile* file_for_face(const std::u16string& face_in, bool& ja_mono) {
    std::u16string face = lower_ascii(face_in);
    if (!face.empty() && face[0] == u'@') face = face.substr(1);
    ja_mono = false;
    const char* file = "jwsubgothic.ttf";
    bool latin = false;
    if (contains(face, u"明朝") || contains(face, u"mincho")) {
        file = "jwsubmincho.ttf";
        ja_mono = !contains(face, u"Ｐ") && !contains(face, u"p明朝");
    } else if (contains(face, u"ゴシック") || contains(face, u"gothic") ||
               contains(face, u"meiryo") || contains(face, u"メイリオ") ||
               contains(face, u"游") || contains(face, u"yu ")) {
        file = contains(face, u"biz") ? "bizudgothic-sub.ttf" : "jwsubgothic.ttf";
        ja_mono = (contains(face, u"ゴシック") && !contains(face, u"Ｐ")) || contains(face, u"biz");
        if (contains(face, u"ui")) ja_mono = false;
    } else if (contains(face, u"courier") || contains(face, u"consolas") ||
               contains(face, u"mono") || contains(face, u"fixedsys") ||
               contains(face, u"terminal")) {
        file = "jwsubmono.ttf";
        latin = true;
    } else if (contains(face, u"times") || contains(face, u"century") ||
               contains(face, u"serif") || contains(face, u"roman") ||
               contains(face, u"georgia") || contains(face, u"bookman")) {
        file = "jwsubserif.ttf";
        latin = true;
    } else if (contains(face, u"calibri")) {
        file = "jwsubcalibri.ttf";
        latin = true;
    } else if (contains(face, u"arial") || contains(face, u"tahoma") ||
               contains(face, u"segoe") || contains(face, u"verdana") ||
               contains(face, u"helvetica") || contains(face, u"sans")) {
        file = "jwsubsans.ttf";
        latin = true;
    }
    FontFile* f = load_file(file);
    if (!f && latin) f = load_file("jwsubgothic.ttf");
    if (!f) f = load_file("bizudgothic-sub.ttf");
    return f;
}

RealFont* realize_font(const LogFont& lf) {
    struct Key {
        std::u16string face;
        int h, w, weight, esc;
        uint8_t it, ul, so;
        bool operator==(const Key& o) const {
            return face == o.face && h == o.h && w == o.w && weight == o.weight &&
                   esc == o.esc && it == o.it && ul == o.ul && so == o.so;
        }
    };
    struct KeyHash {
        size_t operator()(const Key& k) const {
            size_t h = std::hash<std::u16string>()(k.face);
            h = h * 31 + static_cast<size_t>(k.h);
            h = h * 31 + static_cast<size_t>(k.w);
            h = h * 31 + static_cast<size_t>(k.weight);
            h = h * 31 + static_cast<size_t>(k.esc);
            return h * 31 + (k.it << 2 | k.ul << 1 | k.so);
        }
    };
    static std::unordered_map<Key, std::unique_ptr<RealFont>, KeyHash> cache;
    Key key{lf.face, lf.height, lf.width, lf.weight, lf.escapement, lf.italic, lf.underline,
            lf.strikeout};
    auto it = cache.find(key);
    if (it != cache.end()) return it->second.get();

    auto rf = std::make_unique<RealFont>();
    bool ja_mono = false;
    rf->file = file_for_face(lf.face, ja_mono);
    rf->fallback = load_file("jwsubgothic.ttf");
    if (!rf->fallback) rf->fallback = load_file("bizudgothic-sub.ttf");
    rf->monospace_ja = ja_mono;
    rf->face = lf.face;
    rf->charset = lf.charset;
    rf->pitch_family = lf.pitch_family;
    rf->weight = lf.weight ? lf.weight : 400;
    rf->italic = lf.italic;
    rf->underline = lf.underline;
    rf->strikeout = lf.strikeout;
    rf->escapement = lf.escapement;

    int h = lf.height;
    if (h == 0) h = -12;  // the default font size GDI picks for "0"
    FontFile* f = rf->file;
    if (f) {
        int cell_units = f->win_ascent + f->win_descent;
        if (cell_units <= 0) cell_units = f->units_per_em;
        float scale;
        if (h < 0)
            scale = static_cast<float>(-h) / static_cast<float>(f->units_per_em);
        else
            scale = static_cast<float>(h) / static_cast<float>(cell_units);
        rf->scale = scale;
        rf->ascent = static_cast<int>(std::lround(f->win_ascent * scale));
        rf->descent = static_cast<int>(std::lround(f->win_descent * scale));
        rf->height = rf->ascent + rf->descent;
        int em = static_cast<int>(std::lround(f->units_per_em * scale));
        rf->internal_leading = std::max(0, rf->height - em);
        int avg = f->avg_char_width ? f->avg_char_width : f->units_per_em / 2;
        rf->ave_width = std::max(1, static_cast<int>(std::lround(avg * scale)));
        if (ja_mono || rf->monospace_ja) rf->ave_width = std::max(1, (em + 1) / 2);
        rf->max_width = std::max(rf->ave_width,
                                 static_cast<int>(std::lround(f->max_advance * scale)));
        if (rf->fallback) {
            int fcell = rf->fallback->win_ascent + rf->fallback->win_descent;
            if (h < 0)
                rf->fb_scale = static_cast<float>(-h) / rf->fallback->units_per_em;
            else
                rf->fb_scale = static_cast<float>(h) / (fcell > 0 ? fcell : rf->fallback->units_per_em);
        }
    } else {
        // No font files at all: a fixed-cell placeholder so layout still works.
        rf->height = h < 0 ? -h : h;
        rf->ascent = rf->height * 7 / 8;
        rf->descent = rf->height - rf->ascent;
        rf->ave_width = std::max(1, rf->height / 2);
        rf->max_width = rf->height;
    }
    // The MS Japanese faces (ＭＳ ゴシック, ＭＳ Ｐゴシック, ＭＳ 明朝, MS UI
    // Gothic...) have a cell exactly one em tall - usWinAscent 220 and
    // usWinDescent 36 of 256 - so tmHeight is the point size in pixels and a
    // positive lfHeight is the em as well.  The faces standing in for them
    // have taller cells, and every dialog laid out in their dialog units came
    // out a pixel or two too tall.
    std::u16string lface = lower_ascii(lf.face);
    if (f && (contains(lface, u"ＭＳ") || contains(lface, u"ｍｓ") || contains(lface, u"ms ")) &&
        (contains(lface, u"ゴシック") || contains(lface, u"明朝") || contains(lface, u"gothic") || contains(lface, u"mincho"))) {
        int em = h < 0 ? -h : h;
        rf->scale = static_cast<float>(em) / static_cast<float>(f->units_per_em);
        rf->height = em;
        rf->ascent = static_cast<int>(std::lround(em * 220.0 / 256.0));
        rf->descent = em - rf->ascent;
        rf->internal_leading = 0;
        if (rf->fallback) rf->fb_scale = static_cast<float>(em) / rf->fallback->units_per_em;
    }
    if (lf.width > 0 && rf->ave_width > 0) {
        rf->xscale = static_cast<float>(lf.width) / static_cast<float>(rf->ave_width);
        rf->width_override = lf.width;
        rf->max_width = static_cast<int>(rf->max_width * rf->xscale);
        rf->ave_width = lf.width;
    }
    RealFont* raw = rf.get();
    cache[key] = std::move(rf);
    return raw;
}

static bool is_fullwidth(uint32_t cp) {
    return (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) ||
           (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
           (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x2000 && cp <= 0x206F && cp != 0x2010 &&
                                              cp != 0x2013 && cp != 0x2014 && false) ||
           (cp >= 0x25A0 && cp <= 0x27BF) || (cp >= 0x2190 && cp <= 0x21FF) ||
           (cp >= 0x2200 && cp <= 0x22FF) || (cp >= 0x3000 && cp <= 0x303F) ||
           cp == 0x00A7 || cp == 0x00A8 || cp == 0x00B0 || cp == 0x00B1 || cp == 0x00B4 ||
           cp == 0x00B6 || cp == 0x00D7 || cp == 0x00F7 || (cp >= 0x0391 && cp <= 0x03C9) ||
           (cp >= 0x0401 && cp <= 0x0451) || cp == 0x2010 || cp == 0x2015 || cp == 0x2018 ||
           cp == 0x2019 || cp == 0x201C || cp == 0x201D || cp == 0x2020 || cp == 0x2021 ||
           cp == 0x2025 || cp == 0x2026 || cp == 0x2030 || cp == 0x2032 || cp == 0x2033 ||
           cp == 0x203B || cp == 0x2103 || cp == 0x212B;
}

int RealFont::advance(uint32_t cp) {
    return glyph(cp).advance;
}

const Glyph& RealFont::glyph(uint32_t cp) {
    auto it = cache.find(cp);
    if (it != cache.end()) return it->second;
    Glyph g;
    FontFile* f = file;
    float sc = scale;
    int gi = f ? stbtt_FindGlyphIndex(&f->info, static_cast<int>(cp)) : 0;
    if (f && gi == 0 && fallback && fallback != f) {
        int g2 = stbtt_FindGlyphIndex(&fallback->info, static_cast<int>(cp));
        if (g2) {
            f = fallback;
            sc = fb_scale;
            gi = g2;
        }
    }
    if (f) {
        int adv = 0, lsb = 0;
        stbtt_GetGlyphHMetrics(&f->info, gi, &adv, &lsb);
        float sx = sc * xscale, sy = sc;
        g.advance = static_cast<int>(std::lround(adv * sx));
        if (monospace_ja) {
            // A fixed-pitch Japanese face: ASCII and half-width kana take one
            // cell, everything else two, whatever the outline's own advance.
            int cell = static_cast<int>(std::lround(ave_width * (width_override ? 1.0f : 1.0f)));
            g.advance = is_fullwidth(cp) ? cell * 2 : cell;
        }
        if (cp == ' ' || cp == 0x3000) {
            // nothing to draw
        } else {
            int x0, y0, x1, y1;
            stbtt_GetGlyphBitmapBox(&f->info, gi, sx, sy, &x0, &y0, &x1, &y1);
            g.w = x1 - x0;
            g.h = y1 - y0;
            g.x0 = x0;
            g.y0 = y0;
            if (monospace_ja && g.advance) {
                // Centre the outline in its cell when the advances disagree.
                int natural = static_cast<int>(std::lround(adv * sx));
                g.x0 += (g.advance - natural) / 2;
            }
            if (g.w > 0 && g.h > 0 && g.w < 2048 && g.h < 2048) {
                g.alpha.resize(static_cast<size_t>(g.w) * g.h);
                stbtt_MakeGlyphBitmap(&f->info, g.alpha.data(), g.w, g.h, g.w, sx, sy, gi);
                if (weight >= 600) {
                    // Synthetic bold: smear one pixel to the right.
                    std::vector<uint8_t> b(static_cast<size_t>(g.w + 1) * g.h, 0);
                    for (int y = 0; y < g.h; ++y)
                        for (int x = 0; x < g.w; ++x) {
                            uint8_t a = g.alpha[static_cast<size_t>(y) * g.w + x];
                            uint8_t& d0 = b[static_cast<size_t>(y) * (g.w + 1) + x];
                            uint8_t& d1 = b[static_cast<size_t>(y) * (g.w + 1) + x + 1];
                            d0 = std::max(d0, a);
                            d1 = std::max(d1, a);
                        }
                    g.alpha.swap(b);
                    g.w += 1;
                    if (!monospace_ja) g.advance += 1;
                }
                if (italic) {
                    std::vector<uint8_t> b(static_cast<size_t>(g.w + g.h / 4 + 1) * g.h, 0);
                    int nw = g.w + g.h / 4 + 1;
                    for (int y = 0; y < g.h; ++y) {
                        int sh = (g.h - y) / 4;
                        for (int x = 0; x < g.w; ++x)
                            b[static_cast<size_t>(y) * nw + x + sh] =
                                g.alpha[static_cast<size_t>(y) * g.w + x];
                    }
                    g.alpha.swap(b);
                    g.w = nw;
                }
            } else {
                g.w = g.h = 0;
            }
        }
    } else {
        g.advance = is_fullwidth(cp) ? ave_width * 2 : ave_width;
    }
    return cache.emplace(cp, std::move(g)).first->second;
}

}  // namespace x86emu::win32
