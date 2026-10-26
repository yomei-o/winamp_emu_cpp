// Printing: one virtual printer, "PDF / PNG 出力", that turns each page into
// files instead of paper.
//
// A Windows program prints through GDI: PrintDlg (or MFC's defaults from it)
// hands back a DEVMODE and DEVNAMES, CreateDC makes a printer DC from them,
// and StartDoc / StartPage / ... / EndPage / EndDoc bracket the drawing.  Here
// the printer DC draws into a page-sized bitmap with the same rasterizer as
// the screen, at a resolution chosen by paper size, and EndDoc writes
//   C:\jww\印刷\<document>.pdf      every page, as images
//   C:\jww\印刷\<document>.png      (or _1.png, _2.png ... for several pages)
// The browser front end is told about the PDF (print_done_hook) and offers it
// for download; the PNGs are in the file panel.
#include <sys/stat.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <direct.h>
#endif

#include "emulator.h"
#include "files.h"
#include "gui.h"
#include "hookutil.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

namespace x86emu::win32 {

std::function<void(const std::string&)> print_done_hook;

namespace {

const char16_t kPrinterName[] = u"PDF / PNG 出力";

// Sizes in tenths of a millimetre, portrait.  The resolution drops for the
// big sheets so that a page stays near 35 million pixels.
struct Paper {
    const char16_t* name;
    int dmpaper;  // DMPAPER_*, or DMPAPER_USER (256) with the size given
    int w10, h10;
    int dpi;
};
const Paper kPapers[] = {
    {u"A4", 9, 2100, 2970, 300},   {u"A3", 8, 2970, 4200, 300},  {u"A2", 66, 4200, 5940, 200},
    {u"A1", 256, 5940, 8410, 150}, {u"A0", 256, 8410, 11890, 120}, {u"A5", 11, 1480, 2100, 300},
    {u"B4", 12, 2570, 3640, 300},  {u"B5", 13, 1820, 2570, 300},  {u"レター", 1, 2159, 2794, 300},
};
constexpr int kNumPapers = sizeof kPapers / sizeof kPapers[0];

// What the printer is set to: the default for the next DEVMODE handed out.
struct Settings {
    int paper = 0;
    bool landscape = false, color = true;
};
Settings g_settings;

int page_w(int paper, bool land) {
    const Paper& p = kPapers[paper];
    return static_cast<int>(std::lround((land ? p.h10 : p.w10) / 254.0 * p.dpi));
}
int page_h(int paper, bool land) {
    const Paper& p = kPapers[paper];
    return static_cast<int>(std::lround((land ? p.w10 : p.h10) / 254.0 * p.dpi));
}

// ---- DEVMODEW / DEVNAMES ------------------------------------------------------------
constexpr uint32_t kDevModeSize = 220;
constexpr uint32_t DM_ORIENTATION = 0x1, DM_PAPERSIZE = 0x2, DM_PAPERLENGTH = 0x4, DM_PAPERWIDTH = 0x8,
                   DM_SCALE = 0x10, DM_COPIES = 0x100, DM_PRINTQUALITY = 0x400, DM_COLOR = 0x800,
                   DM_YRESOLUTION = 0x2000, DM_FORMNAME = 0x10000;

void write_devmode(Emulator& e, uint64_t p, const Settings& s) {
    std::vector<uint8_t> z(kDevModeSize, 0);
    e.mem.write(p, z.data(), z.size());
    const Paper& pp = kPapers[s.paper];
    write_wstr(e, p, kPrinterName, 32);
    e.mem.write16(p + 64, 0x0401);  // dmSpecVersion
    e.mem.write16(p + 66, 0x0100);  // dmDriverVersion
    e.mem.write16(p + 68, kDevModeSize);
    e.mem.write16(p + 70, 0);       // dmDriverExtra
    e.mem.write32(p + 72, DM_ORIENTATION | DM_PAPERSIZE | DM_PAPERLENGTH | DM_PAPERWIDTH | DM_SCALE | DM_COPIES |
                              DM_PRINTQUALITY | DM_COLOR | DM_YRESOLUTION | DM_FORMNAME);
    e.mem.write16(p + 76, s.landscape ? 2 : 1);
    e.mem.write16(p + 78, static_cast<uint16_t>(pp.dmpaper));
    e.mem.write16(p + 80, static_cast<uint16_t>(pp.h10));
    e.mem.write16(p + 82, static_cast<uint16_t>(pp.w10));
    e.mem.write16(p + 84, 100);     // dmScale
    e.mem.write16(p + 86, 1);       // dmCopies
    e.mem.write16(p + 90, static_cast<uint16_t>(pp.dpi));
    e.mem.write16(p + 92, s.color ? 2 : 1);  // DMCOLOR_COLOR / DMCOLOR_MONOCHROME
    e.mem.write16(p + 96, static_cast<uint16_t>(pp.dpi));
    write_wstr(e, p + 102, pp.name, 32);
}

// Reads back what a program (or the user, through us) put in a DEVMODE.
void read_devmode(Emulator& e, uint64_t p, Settings& s) {
    if (!p) return;
    uint32_t fields = e.mem.read32(p + 72);
    if (fields & DM_ORIENTATION) s.landscape = e.mem.read16(p + 76) == 2;
    if (fields & DM_PAPERSIZE) {
        int dm = e.mem.read16(p + 78);
        int w10 = e.mem.read16(p + 82), h10 = e.mem.read16(p + 80);
        for (int i = 0; i < kNumPapers; ++i) {
            if (kPapers[i].dmpaper != dm) continue;
            if (dm == 256 && (kPapers[i].w10 != w10 || kPapers[i].h10 != h10)) continue;
            s.paper = i;
            break;
        }
    }
    if (fields & DM_COLOR) s.color = e.mem.read16(p + 92) != 1;
}

uint64_t new_devmode(Emulator& e, const Settings& s) {
    uint64_t p = e.heap_alloc(kDevModeSize);
    write_devmode(e, p, s);
    return p;
}

uint64_t new_devnames(Emulator& e) {
    // Offsets are in characters from the start of the block.
    std::u16string drv = u"winspool", dev = kPrinterName, out = u"PDF:";
    uint32_t o_drv = 4, o_dev = o_drv + static_cast<uint32_t>(drv.size()) + 1;
    uint32_t o_out = o_dev + static_cast<uint32_t>(dev.size()) + 1;
    uint32_t total = (o_out + static_cast<uint32_t>(out.size()) + 1) * 2;
    uint64_t p = e.heap_alloc(total);
    e.mem.write16(p, static_cast<uint16_t>(o_drv));
    e.mem.write16(p + 2, static_cast<uint16_t>(o_dev));
    e.mem.write16(p + 4, static_cast<uint16_t>(o_out));
    e.mem.write16(p + 6, 1);  // DN_DEFAULTPRN
    write_wstr(e, p + o_drv * 2, drv, static_cast<int>(drv.size()) + 1);
    write_wstr(e, p + o_dev * 2, dev, static_cast<int>(dev.size()) + 1);
    write_wstr(e, p + o_out * 2, out, static_cast<int>(out.size()) + 1);
    return p;
}

std::vector<std::u16string> paper_names() {
    std::vector<std::u16string> v;
    for (const Paper& p : kPapers) v.push_back(p.name);
    return v;
}

// ---- files ------------------------------------------------------------------------
bool make_dir(const std::string& host) {
#if defined(_WIN32)
    std::u16string w = from_utf8(host);
    return _wmkdir(reinterpret_cast<const wchar_t*>(w.c_str())) == 0;
#else
    return mkdir(host.c_str(), 0777) == 0;
#endif
}

bool write_file(const std::string& host, const void* data, size_t n) {
#if defined(_WIN32)
    std::u16string w = from_utf8(host);
    std::FILE* f = _wfopen(reinterpret_cast<const wchar_t*>(w.c_str()), L"wb");
#else
    std::FILE* f = std::fopen(host.c_str(), "wb");
#endif
    if (!f) return false;
    bool ok = std::fwrite(data, 1, n, f) == n;
    return std::fclose(f) == 0 && ok;
}

// ---- the job ------------------------------------------------------------------------
struct PageOut {
    int w = 0, h = 0;          // pixels
    double wpt = 0, hpt = 0;   // points
    bool gray = false;
    std::vector<uint8_t> flate;  // the pixels, zlib-compressed, for the PDF
    std::vector<uint8_t> png;
};
struct Job {
    bool active = false;
    std::u16string name;
    std::vector<PageOut> pages;
};
Job g_job;

std::u16string job_basename(std::u16string doc) {
    size_t sep = doc.find_last_of(u"\\/");
    if (sep != std::u16string::npos) doc = doc.substr(sep + 1);
    size_t dot = doc.find_last_of(u'.');
    if (dot != std::u16string::npos && dot > 0) doc = doc.substr(0, dot);
    std::u16string out;
    for (char16_t c : doc) out += (c < 0x20 || std::u16string(u"\\/:*?\"<>|").find(c) != std::u16string::npos) ? u'_' : c;
    return out.empty() ? u"jwcad" : out;
}

void encode_page(DC& d) {
    PageOut po;
    const PixelBuf& px = d.page->px;
    po.w = px.w;
    po.h = px.h;
    const Paper& pp = kPapers[d.prn_paper];
    po.wpt = (d.prn_landscape ? pp.h10 : pp.w10) / 254.0 * 72.0;
    po.hpt = (d.prn_landscape ? pp.w10 : pp.h10) / 254.0 * 72.0;
    po.gray = !d.prn_color;
    int ch = po.gray ? 1 : 3;
    std::vector<uint8_t> raw(static_cast<size_t>(po.w) * po.h * ch);
    for (int y = 0; y < po.h; ++y) {
        const uint8_t* s = px.bits + static_cast<ptrdiff_t>(y) * px.stride;
        uint8_t* o = raw.data() + static_cast<size_t>(y) * po.w * ch;
        for (int x = 0; x < po.w; ++x, s += 4) {
            uint8_t b = s[0], gr = s[1], r = s[2];
            if (po.gray) {
                *o++ = static_cast<uint8_t>((r * 299 + gr * 587 + b * 114) / 1000);
            } else {
                *o++ = r;
                *o++ = gr;
                *o++ = b;
            }
        }
    }
    int len = 0;
    if (unsigned char* z = stbi_zlib_compress(raw.data(), static_cast<int>(raw.size()), &len, 6)) {
        po.flate.assign(z, z + len);
        STBIW_FREE(z);
    }
    if (unsigned char* p = stbi_write_png_to_mem(raw.data(), po.w * ch, po.w, po.h, ch, &len)) {
        po.png.assign(p, p + len);
        STBIW_FREE(p);
    }
    g_job.pages.push_back(std::move(po));
}

std::string pdf_hex_utf16(const std::u16string& s) {
    std::string out = "<FEFF";
    char buf[8];
    for (char16_t c : s) {
        std::snprintf(buf, sizeof buf, "%04X", static_cast<unsigned>(c));
        out += buf;
    }
    return out + ">";
}

std::vector<uint8_t> build_pdf(const Job& job) {
    std::string out = "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
    std::vector<size_t> offsets;
    auto begin_obj = [&]() {
        offsets.push_back(out.size());
        out += std::to_string(offsets.size()) + " 0 obj\n";
    };
    size_t n = job.pages.size();
    // 1 catalog, 2 pages, 3 info, then three objects a page: page, content, image.
    begin_obj();
    out += "<< /Type /Catalog /Pages 2 0 R >>\nendobj\n";
    begin_obj();
    out += "<< /Type /Pages /Count " + std::to_string(n) + " /Kids [";
    for (size_t i = 0; i < n; ++i) out += std::to_string(4 + i * 3) + " 0 R ";
    out += "] >>\nendobj\n";
    begin_obj();
    out += "<< /Producer (jwemu) /Title " + pdf_hex_utf16(job.name) + " >>\nendobj\n";
    char buf[256];
    for (size_t i = 0; i < n; ++i) {
        const PageOut& p = job.pages[i];
        int page = static_cast<int>(4 + i * 3);
        begin_obj();
        std::snprintf(buf, sizeof buf,
                      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %.3f %.3f] /Contents %d 0 R "
                      "/Resources << /XObject << /Im0 %d 0 R >> >> >>\nendobj\n",
                      p.wpt, p.hpt, page + 1, page + 2);
        out += buf;
        std::snprintf(buf, sizeof buf, "q %.3f 0 0 %.3f 0 0 cm /Im0 Do Q\n", p.wpt, p.hpt);
        std::string content = buf;
        begin_obj();
        out += "<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content + "endstream\nendobj\n";
        begin_obj();
        std::snprintf(buf, sizeof buf,
                      "<< /Type /XObject /Subtype /Image /Width %d /Height %d /ColorSpace /%s "
                      "/BitsPerComponent 8 /Filter /FlateDecode /Length %zu >>\nstream\n",
                      p.w, p.h, p.gray ? "DeviceGray" : "DeviceRGB", p.flate.size());
        out += buf;
        out.append(reinterpret_cast<const char*>(p.flate.data()), p.flate.size());
        out += "\nendstream\nendobj\n";
    }
    size_t xref = out.size();
    out += "xref\n0 " + std::to_string(offsets.size() + 1) + "\n0000000000 65535 f \n";
    for (size_t off : offsets) {
        std::snprintf(buf, sizeof buf, "%010zu 00000 n \n", off);
        out += buf;
    }
    out += "trailer\n<< /Size " + std::to_string(offsets.size() + 1) + " /Root 1 0 R /Info 3 0 R >>\nstartxref\n" +
           std::to_string(xref) + "\n%%EOF\n";
    return std::vector<uint8_t>(out.begin(), out.end());
}

// Writes the job out; returns the guest path of the PDF, or "" on failure.
std::string finish_job() {
    std::string dir_guest = "C:\\jww\\" + to_utf8(u"印刷");
    std::string dir = FileTable::host_path(dir_guest);
    make_dir(dir);
    std::string base = to_utf8(job_basename(g_job.name));
    std::string pdf_guest = dir_guest + "\\" + base + ".pdf";
    std::vector<uint8_t> pdf = build_pdf(g_job);
    if (!write_file(FileTable::host_path(pdf_guest), pdf.data(), pdf.size())) {
        std::fprintf(stderr, "[print] could not write %s\n", pdf_guest.c_str());
        return "";
    }
    std::fprintf(stderr, "[print] wrote %s (%zu pages)\n", pdf_guest.c_str(), g_job.pages.size());
    for (size_t i = 0; i < g_job.pages.size(); ++i) {
        std::string png_guest = dir_guest + "\\" + base +
                                (g_job.pages.size() == 1 ? std::string() : "_" + std::to_string(i + 1)) + ".png";
        const auto& png = g_job.pages[i].png;
        if (write_file(FileTable::host_path(png_guest), png.data(), png.size()))
            std::fprintf(stderr, "[print] wrote %s\n", png_guest.c_str());
    }
    return pdf_guest;
}

void start_page(Gui& g, DC& d) {
    d.page = std::make_shared<Bitmap>();
    PixelBuf& px = d.page->px;
    px.w = page_w(d.prn_paper, d.prn_landscape);
    px.h = page_h(d.prn_paper, d.prn_landscape);
    px.bpp = 32;
    px.stride = px.w * 4;
    d.page->host.assign(static_cast<size_t>(px.stride) * px.h, 0xFF);
    px.bits = d.page->host.data();
}

uint64_t start_doc(Emulator& e, uint32_t hdc, const std::u16string& name) {
    DC* d = gui(e).dc(hdc);
    if (!d || d->kind != DC::Kind::Printer) return 0;
    g_job = Job{};
    g_job.active = true;
    g_job.name = name;
    return 1;  // the job id
}

}  // namespace

bool is_printer_name(const std::u16string& name) { return name == kPrinterName; }

uint32_t create_printer_dc(Gui& g, uint64_t devmode) {
    Settings s = g_settings;
    read_devmode(g.emu, devmode, s);
    uint32_t h = g.create_dc(DC::Kind::Printer, 0, false);
    DC* d = g.dc(h);
    d->prn_paper = s.paper;
    d->prn_landscape = s.landscape;
    d->prn_color = s.color;
    return h;
}

uint64_t printer_device_caps(const DC& d, int index) {
    const Paper& p = kPapers[d.prn_paper];
    int w = page_w(d.prn_paper, d.prn_landscape), h = page_h(d.prn_paper, d.prn_landscape);
    int wmm = (d.prn_landscape ? p.h10 : p.w10) / 10, hmm = (d.prn_landscape ? p.w10 : p.h10) / 10;
    switch (index) {
        case 0: return 0x400;    // DRIVERVERSION
        case 2: return 2;        // TECHNOLOGY: DT_RASPRINTER
        case 4: return static_cast<uint64_t>(wmm);  // HORZSIZE
        case 6: return static_cast<uint64_t>(hmm);  // VERTSIZE
        case 8: case 110: return static_cast<uint64_t>(w);   // HORZRES, PHYSICALWIDTH
        case 10: case 111: return static_cast<uint64_t>(h);  // VERTRES, PHYSICALHEIGHT
        case 12: return 32;      // BITSPIXEL
        case 14: return 1;       // PLANES
        case 16: case 18: case 24: return 0xFFFFFFFF;  // NUMBRUSHES, NUMPENS, NUMCOLORS
        case 28: return 0xFF;    // CURVECAPS
        case 30: return 0xFE;    // LINECAPS
        case 32: return 0xFF;    // POLYGONALCAPS
        case 34: return 0x7807;  // TEXTCAPS
        case 36: return 1;       // CLIPCAPS
        case 38: return 0x7E99;  // RASTERCAPS
        case 40: case 42: return static_cast<uint64_t>(p.dpi);  // ASPECTX/Y
        case 44: return static_cast<uint64_t>(std::lround(p.dpi * 1.41421356));  // ASPECTXY
        case 88: case 90: return static_cast<uint64_t>(p.dpi);  // LOGPIXELSX/Y
        case 108: return 24;     // COLORRES
    }
    return 0;  // PHYSICALOFFSETX/Y, SCALINGFACTORX/Y and the rest
}

void install_print(Emulator& e) {
    // ---- comdlg32 --------------------------------------------------------------------
    // PRINTDLGW is byte-packed on 32-bit Windows: hDevMode at 8, hDevNames 12,
    // hDC 16, Flags 20.
    api(e, "PrintDlgW", 1, [](Emulator& e) {
        Gui& g = gui(e);
        uint64_t pd = A(e, 0);
        uint32_t flags = e.mem.read32(pd + 20);
        uint64_t hdm = e.mem.read32(pd + 8);
        Settings s = g_settings;
        read_devmode(e, hdm, s);
        constexpr uint32_t PD_PRINTSETUP = 0x40, PD_RETURNDC = 0x100, PD_RETURNIC = 0x200, PD_RETURNDEFAULT = 0x400;
        if (!(flags & PD_RETURNDEFAULT)) {
            if (!print_dialog(g, e.mem.read32(pd + 4), (flags & PD_PRINTSETUP) != 0, paper_names(), s.paper,
                              s.landscape, s.color))
                return ret(e, 0);
            g_settings = s;
        }
        if (hdm && e.heap_block_size(hdm) >= kDevModeSize)
            write_devmode(e, hdm, s);
        else
            e.mem.write32(pd + 8, static_cast<uint32_t>(new_devmode(e, s)));
        if (!e.mem.read32(pd + 12)) e.mem.write32(pd + 12, static_cast<uint32_t>(new_devnames(e)));
        if (flags & (PD_RETURNDC | PD_RETURNIC)) {
            uint32_t h = g.create_dc(DC::Kind::Printer, 0, false);
            DC* d = g.dc(h);
            d->prn_paper = s.paper;
            d->prn_landscape = s.landscape;
            d->prn_color = s.color;
            e.mem.write32(pd + 16, h);
        }
        e.mem.write16(pd + 32, 1);  // nCopies
        ret(e, 1);
    });
    api(e, "PrintDlgExW", 1, [](Emulator& e) { ret(e, 0x80004005); });  // E_FAIL: MFC falls back to PrintDlg
    api(e, "PageSetupDlgW", 1, [](Emulator& e) { ret(e, 0); });

    // ---- winspool ----------------------------------------------------------------------
    api(e, "OpenPrinterW", 3, [](Emulator& e) {
        std::u16string name = A(e, 0) ? read_wstr(e, A(e, 0)) : u"";
        if (A(e, 1)) e.mem.write32(A(e, 1), is_printer_name(name) ? 0x5052544E : 0);  // 'PRTN'
        if (!is_printer_name(name)) {
            e.set_last_error(1801);  // ERROR_INVALID_PRINTER_NAME
            return ret(e, 0);
        }
        ret(e, 1);
    });
    api(e, "OpenPrinterA", 3, [](Emulator& e) {
        std::u16string name = A(e, 0) ? cp932_to_utf16(e.mem.read_cstring(A(e, 0))) : u"";
        if (A(e, 1)) e.mem.write32(A(e, 1), is_printer_name(name) ? 0x5052544E : 0);
        ret(e, is_printer_name(name) ? 1 : 0);
    });
    api(e, "ClosePrinter", 1, [](Emulator& e) { ret(e, 1); });
    // (hWnd, hPrinter, pDeviceName, pDevModeOutput, pDevModeInput, fMode)
    api(e, "DocumentPropertiesW", 6, [](Emulator& e) {
        Gui& g = gui(e);
        uint32_t mode = U(e, 5);
        if (mode == 0) return ret(e, kDevModeSize);
        constexpr uint32_t DM_OUT_BUFFER = 2, DM_IN_PROMPT = 4, DM_IN_BUFFER = 8;
        Settings s = g_settings;
        if (mode & DM_IN_BUFFER) read_devmode(e, A(e, 4), s);
        if (mode & DM_IN_PROMPT) {
            if (!print_dialog(g, U(e, 0), true, paper_names(), s.paper, s.landscape, s.color)) return ret(e, 2);  // IDCANCEL
            g_settings = s;
        }
        if ((mode & DM_OUT_BUFFER) && A(e, 3)) write_devmode(e, A(e, 3), s);
        ret(e, 1);  // IDOK
    });
    api(e, "GetJobW", 6, [](Emulator& e) {
        if (A(e, 5)) e.mem.write32(A(e, 5), 0);
        ret(e, 0);
    });
    api(e, "GetDefaultPrinterW", 2, [](Emulator& e) {
        uint64_t buf = A(e, 0), pn = A(e, 1);
        std::u16string name = kPrinterName;
        uint32_t need = static_cast<uint32_t>(name.size()) + 1;
        uint32_t have = pn ? e.mem.read32(pn) : 0;
        if (pn) e.mem.write32(pn, need);
        if (!buf || have < need) {
            e.set_last_error(122);  // ERROR_INSUFFICIENT_BUFFER
            return ret(e, 0);
        }
        write_wstr(e, buf, name, static_cast<int>(need));
        ret(e, 1);
    });
    // (Flags, Name, Level, pPrinterEnum, cbBuf, pcbNeeded, pcReturned): levels
    // 1, 2, 4 and 5 all start with enough for a name; level 4 (name, server,
    // attributes) is what most callers ask for, and the others get it padded.
    api(e, "EnumPrintersW", 7, [](Emulator& e) {
        uint32_t level = U(e, 2);
        uint64_t buf = A(e, 3);
        uint32_t cb = U(e, 4);
        std::u16string name = kPrinterName;
        uint32_t fixed = level == 2 ? 84 : level == 1 ? 16 : level == 5 ? 20 : 12;
        uint32_t need = fixed + static_cast<uint32_t>(name.size() + 1) * 2;
        if (A(e, 5)) e.mem.write32(A(e, 5), need);
        if (A(e, 6)) e.mem.write32(A(e, 6), 0);
        if (!buf || cb < need) {
            e.set_last_error(122);
            return ret(e, 0);
        }
        std::vector<uint8_t> z(fixed, 0);
        e.mem.write(buf, z.data(), z.size());
        uint64_t str = buf + fixed;
        write_wstr(e, str, name, static_cast<int>(name.size()) + 1);
        switch (level) {
            case 1: e.mem.write32(buf + 8, static_cast<uint32_t>(str)); break;   // pName
            case 2: e.mem.write32(buf + 4, static_cast<uint32_t>(str)); break;   // pPrinterName
            default: e.mem.write32(buf, static_cast<uint32_t>(str)); break;      // 4, 5: pPrinterName
        }
        if (A(e, 6)) e.mem.write32(A(e, 6), 1);
        ret(e, 1);
    });
    // (pDevice, pPort, fwCapability, pOutput, pDevMode)
    api(e, "DeviceCapabilitiesW", 5, [](Emulator& e) {
        uint32_t cap = U(e, 2);
        uint64_t out = A(e, 3);
        constexpr uint32_t DC_PAPERS = 2, DC_PAPERSIZE = 3, DC_COPIES = 18, DC_ORIENTATION = 17, DC_PAPERNAMES = 16,
                           DC_COLORDEVICE = 32;
        switch (cap) {
            case DC_PAPERS:
                if (out)
                    for (int i = 0; i < kNumPapers; ++i) e.mem.write16(out + i * 2, static_cast<uint16_t>(kPapers[i].dmpaper));
                return ret(e, kNumPapers);
            case DC_PAPERSIZE:  // POINTs in tenths of a millimetre
                if (out)
                    for (int i = 0; i < kNumPapers; ++i) {
                        e.mem.write32(out + i * 8, static_cast<uint32_t>(kPapers[i].w10));
                        e.mem.write32(out + i * 8 + 4, static_cast<uint32_t>(kPapers[i].h10));
                    }
                return ret(e, kNumPapers);
            case DC_PAPERNAMES:  // 64 characters each
                if (out)
                    for (int i = 0; i < kNumPapers; ++i) write_wstr(e, out + i * 128, kPapers[i].name, 64);
                return ret(e, kNumPapers);
            case DC_ORIENTATION: return ret(e, 90);
            case DC_COPIES: return ret(e, 1);
            case DC_COLORDEVICE: return ret(e, 1);
        }
        ret(e, static_cast<uint64_t>(-1) & 0xFFFFFFFF);
    });

    // ---- gdi32: the job --------------------------------------------------------------
    // DOCINFOW: cbSize, lpszDocName, lpszOutput, ...
    api(e, "StartDocW", 2, [](Emulator& e) {
        uint64_t di = A(e, 1);
        uint64_t name = di ? e.mem.read32(di + 4) : 0;
        ret(e, start_doc(e, U(e, 0), name ? read_wstr(e, name) : u""));
    });
    api(e, "StartDocA", 2, [](Emulator& e) {
        uint64_t di = A(e, 1);
        uint64_t name = di ? e.mem.read32(di + 4) : 0;
        ret(e, start_doc(e, U(e, 0), name ? cp932_to_utf16(e.mem.read_cstring(name)) : u""));
    });
    api(e, "StartPage", 1, [](Emulator& e) {
        Gui& g = gui(e);
        DC* d = g.dc(U(e, 0));
        if (!d || d->kind != DC::Kind::Printer) return ret(e, 0);
        start_page(g, *d);
        ret(e, 1);
    });
    api(e, "EndPage", 1, [](Emulator& e) {
        DC* d = gui(e).dc(U(e, 0));
        if (!d || d->kind != DC::Kind::Printer || !d->page) return ret(e, 0);
        encode_page(*d);
        d->page.reset();
        ret(e, 1);
    });
    api(e, "EndDoc", 1, [](Emulator& e) {
        DC* d = gui(e).dc(U(e, 0));
        if (!d || d->kind != DC::Kind::Printer || !g_job.active) return ret(e, 0);
        if (d->page) {  // a last page nobody ended
            encode_page(*d);
            d->page.reset();
        }
        std::string pdf = g_job.pages.empty() ? std::string() : finish_job();
        g_job = Job{};
        if (!pdf.empty() && print_done_hook) print_done_hook(pdf);
        ret(e, 1);
    });
    api(e, "AbortDoc", 1, [](Emulator& e) {
        if (DC* d = gui(e).dc(U(e, 0))) d->page.reset();
        g_job = Job{};
        ret(e, 1);
    });
    api(e, "SetAbortProc", 2, [](Emulator& e) { ret(e, 1); });
    // (hdc, nEscape, cbInput, lpvInData, lpvOutData)
    api(e, "Escape", 5, [](Emulator& e) {
        DC* d = gui(e).dc(U(e, 0));
        if (!d || d->kind != DC::Kind::Printer) return ret(e, 0);
        constexpr uint32_t QUERYESCSUPPORT = 8, GETPHYSPAGESIZE = 12, GETPRINTINGOFFSET = 13, GETSCALINGFACTOR = 14;
        uint32_t esc = U(e, 1);
        uint64_t in = A(e, 3), out = A(e, 4);
        switch (esc) {
            case QUERYESCSUPPORT: {
                uint32_t q = in ? e.mem.read32(in) : 0;
                return ret(e, q == GETPHYSPAGESIZE || q == GETPRINTINGOFFSET || q == GETSCALINGFACTOR);
            }
            case GETPHYSPAGESIZE:
                write_point(e, out, {page_w(d->prn_paper, d->prn_landscape), page_h(d->prn_paper, d->prn_landscape)});
                return ret(e, 1);
            case GETPRINTINGOFFSET:
            case GETSCALINGFACTOR:
                write_point(e, out, {0, 0});
                return ret(e, 1);
        }
        ret(e, 0);
    });
    api(e, "ExtEscape", 6, [](Emulator& e) { ret(e, 0); });
}

}  // namespace x86emu::win32
