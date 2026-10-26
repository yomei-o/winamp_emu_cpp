// Declarations shared between the user32 files.
#pragma once

#include "gui.h"

namespace x86emu::win32 {

// Window messages used on the host side.
enum : uint32_t {
    WM_NULL = 0x0000, WM_CREATE = 0x0001, WM_DESTROY = 0x0002, WM_MOVE = 0x0003,
    WM_SIZE = 0x0005, WM_ACTIVATE = 0x0006, WM_SETFOCUS = 0x0007, WM_KILLFOCUS = 0x0008,
    WM_ENABLE = 0x000A, WM_SETREDRAW = 0x000B, WM_SETTEXT = 0x000C, WM_GETTEXT = 0x000D,
    WM_GETTEXTLENGTH = 0x000E, WM_PAINT = 0x000F, WM_CLOSE = 0x0010, WM_QUIT = 0x0012,
    WM_ERASEBKGND = 0x0014, WM_SYSCOLORCHANGE = 0x0015, WM_SHOWWINDOW = 0x0018,
    WM_ACTIVATEAPP = 0x001C, WM_CANCELMODE = 0x001F, WM_SETCURSOR = 0x0020,
    WM_MOUSEACTIVATE = 0x0021, WM_CHILDACTIVATE = 0x0022, WM_GETMINMAXINFO = 0x0024,
    WM_DRAWITEM = 0x002B, WM_MEASUREITEM = 0x002C, WM_DELETEITEM = 0x002D,
    WM_VKEYTOITEM = 0x002E, WM_CHARTOITEM = 0x002F, WM_SETFONT = 0x0030, WM_GETFONT = 0x0031,
    WM_QUERYDRAGICON = 0x0037, WM_COMPAREITEM = 0x0039, WM_WINDOWPOSCHANGING = 0x0046,
    WM_WINDOWPOSCHANGED = 0x0047, WM_NOTIFY = 0x004E, WM_INPUTLANGCHANGE = 0x0051,
    WM_HELP = 0x0053, WM_NOTIFYFORMAT = 0x0055, WM_CONTEXTMENU = 0x007B,
    WM_STYLECHANGING = 0x007C, WM_STYLECHANGED = 0x007D, WM_GETICON = 0x007F,
    WM_SETICON = 0x0080, WM_NCCREATE = 0x0081, WM_NCDESTROY = 0x0082, WM_NCCALCSIZE = 0x0083,
    WM_NCHITTEST = 0x0084, WM_NCPAINT = 0x0085, WM_NCACTIVATE = 0x0086, WM_GETDLGCODE = 0x0087,
    WM_NCMOUSEMOVE = 0x00A0, WM_NCLBUTTONDOWN = 0x00A1, WM_NCLBUTTONUP = 0x00A2,
    WM_NCLBUTTONDBLCLK = 0x00A3, WM_NCRBUTTONDOWN = 0x00A4, WM_NCRBUTTONUP = 0x00A5,
    WM_NCMBUTTONDOWN = 0x00A7, WM_NCMBUTTONUP = 0x00A8,
    WM_KEYDOWN = 0x0100, WM_KEYUP = 0x0101, WM_CHAR = 0x0102, WM_DEADCHAR = 0x0103,
    WM_SYSKEYDOWN = 0x0104, WM_SYSKEYUP = 0x0105, WM_SYSCHAR = 0x0106,
    WM_IME_STARTCOMPOSITION = 0x010D, WM_INITDIALOG = 0x0110, WM_COMMAND = 0x0111,
    WM_SYSCOMMAND = 0x0112, WM_TIMER = 0x0113, WM_HSCROLL = 0x0114, WM_VSCROLL = 0x0115,
    WM_INITMENU = 0x0116, WM_INITMENUPOPUP = 0x0117, WM_MENUSELECT = 0x011F,
    WM_MENUCHAR = 0x0120, WM_ENTERIDLE = 0x0121, WM_UNINITMENUPOPUP = 0x0125,
    WM_CHANGEUISTATE = 0x0127, WM_UPDATEUISTATE = 0x0128, WM_QUERYUISTATE = 0x0129,
    WM_CTLCOLORMSGBOX = 0x0132, WM_CTLCOLOREDIT = 0x0133, WM_CTLCOLORLISTBOX = 0x0134,
    WM_CTLCOLORBTN = 0x0135, WM_CTLCOLORDLG = 0x0136, WM_CTLCOLORSCROLLBAR = 0x0137,
    WM_CTLCOLORSTATIC = 0x0138,
    WM_MOUSEMOVE = 0x0200, WM_LBUTTONDOWN = 0x0201, WM_LBUTTONUP = 0x0202,
    WM_LBUTTONDBLCLK = 0x0203, WM_RBUTTONDOWN = 0x0204, WM_RBUTTONUP = 0x0205,
    WM_RBUTTONDBLCLK = 0x0206, WM_MBUTTONDOWN = 0x0207, WM_MBUTTONUP = 0x0208,
    WM_MBUTTONDBLCLK = 0x0209, WM_MOUSEWHEEL = 0x020A, WM_PARENTNOTIFY = 0x0210,
    WM_ENTERMENULOOP = 0x0211, WM_EXITMENULOOP = 0x0212, WM_NEXTMENU = 0x0213,
    WM_CAPTURECHANGED = 0x0215, WM_MOVING = 0x0216, WM_ENTERSIZEMOVE = 0x0231,
    WM_EXITSIZEMOVE = 0x0232, WM_DROPFILES = 0x0233, WM_MDIGETACTIVE = 0x0229,
    WM_IME_SETCONTEXT = 0x0281, WM_IME_NOTIFY = 0x0282, WM_MOUSEHOVER = 0x02A1,
    WM_MOUSELEAVE = 0x02A3, WM_PRINT = 0x0317, WM_PRINTCLIENT = 0x0318,
    WM_THEMECHANGED = 0x031A, WM_USER = 0x0400,
};

enum : uint32_t {
    WS_OVERLAPPED = 0, WS_POPUP = 0x80000000, WS_CHILD = 0x40000000, WS_MINIMIZE = 0x20000000,
    WS_VISIBLE = 0x10000000, WS_DISABLED = 0x08000000, WS_CLIPSIBLINGS = 0x04000000,
    WS_CLIPCHILDREN = 0x02000000, WS_MAXIMIZE = 0x01000000, WS_CAPTION = 0x00C00000,
    WS_BORDER = 0x00800000, WS_DLGFRAME = 0x00400000, WS_VSCROLL = 0x00200000,
    WS_HSCROLL = 0x00100000, WS_SYSMENU = 0x00080000, WS_THICKFRAME = 0x00040000,
    WS_GROUP = 0x00020000, WS_TABSTOP = 0x00010000, WS_MINIMIZEBOX = 0x00020000,
    WS_MAXIMIZEBOX = 0x00010000,
    WS_EX_DLGMODALFRAME = 0x1, WS_EX_NOPARENTNOTIFY = 0x4, WS_EX_TOPMOST = 0x8,
    WS_EX_TRANSPARENT = 0x20, WS_EX_MDICHILD = 0x40, WS_EX_TOOLWINDOW = 0x80,
    WS_EX_WINDOWEDGE = 0x100, WS_EX_CLIENTEDGE = 0x200, WS_EX_CONTEXTHELP = 0x400,
    WS_EX_STATICEDGE = 0x20000, WS_EX_APPWINDOW = 0x40000, WS_EX_LAYERED = 0x80000,
    WS_EX_CONTROLPARENT = 0x10000, WS_EX_NOACTIVATE = 0x08000000,
};

enum : int {
    HTERROR = -2, HTTRANSPARENT = -1, HTNOWHERE = 0, HTCLIENT = 1, HTCAPTION = 2,
    HTSYSMENU = 3, HTMENU = 5, HTHSCROLL = 6, HTVSCROLL = 7, HTMINBUTTON = 8,
    HTMAXBUTTON = 9, HTLEFT = 10, HTRIGHT = 11, HTTOP = 12, HTTOPLEFT = 13, HTTOPRIGHT = 14,
    HTBOTTOM = 15, HTBOTTOMLEFT = 16, HTBOTTOMRIGHT = 17, HTBORDER = 18, HTCLOSE = 20,
    HTHELP = 21,
};

enum : uint32_t {
    SWP_NOSIZE = 0x1, SWP_NOMOVE = 0x2, SWP_NOZORDER = 0x4, SWP_NOREDRAW = 0x8,
    SWP_NOACTIVATE = 0x10, SWP_FRAMECHANGED = 0x20, SWP_SHOWWINDOW = 0x40,
    SWP_HIDEWINDOW = 0x80, SWP_NOCOPYBITS = 0x100, SWP_NOOWNERZORDER = 0x200,
    SWP_NOSENDCHANGING = 0x400, SWP_DEFERERASE = 0x2000, SWP_ASYNCWINDOWPOS = 0x4000,
    SWP_STATECHANGED = 0x8000,
};

// Non-client metrics (Windows 11 at 96 DPI).
constexpr int kCaptionH = 23;
constexpr int kSmCaptionH = 23;
constexpr int kMenuH = 20;
constexpr int kThickFrame = 8;   // SM_CXFRAME + SM_CXPADDEDBORDER
constexpr int kDlgFrame = 3;
constexpr int kScrollW = 17;
constexpr int kDwmInvisible = 7;  // Windows 11's invisible resize border, outside the 1px visible one

// The frame a style gets.  On Windows 11 every top-level window with a thick
// or a dialog frame is framed by DWM: 7 invisible pixels for resizing and one
// visible border, 8 in all, whether it can be resized or not.  A child keeps
// the classic 3-pixel dialog frame.
inline int frame_for_style(uint32_t style, uint32_t exstyle) {
    if (style & WS_THICKFRAME) return kThickFrame;
    if ((style & WS_DLGFRAME) || (exstyle & WS_EX_DLGMODALFRAME)) return (style & WS_CHILD) ? kDlgFrame : kThickFrame;
    if (style & WS_BORDER) return 1;
    return 0;
}
// Whether DWM draws the frame: invisible edges, a 1px border, rounded corners.
inline bool dwm_frame(uint32_t style, uint32_t exstyle) {
    return !(style & WS_CHILD) && frame_for_style(style, exstyle) == kThickFrame;
}

// The window procedure the host provides for a built-in class, and the
// registration helper the control files call.
using HostProc = uint64_t (*)(Gui& g, uint32_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp);
void register_builtin_class(Emulator& e, const char16_t* name, uint32_t style, int wnd_extra,
                            uint64_t cursor_id, uint64_t brush, HostProc proc);

uint64_t def_window_proc(Gui& g, uint32_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp);
uint64_t def_dlg_proc(Gui& g, uint32_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp);
bool is_dialog_message(Gui& g, uint32_t hdlg, const Msg& m);
int nc_hittest(Gui& g, Window* w, Point screen);
void nc_paint(Gui& g, Window* w, const Region* rgn);
// The caption button under the mouse (or held down), for its hover colour.
void nc_set_hot(Gui& g, uint32_t hwnd, int ht, bool pressed);
void nc_calc(Gui& g, Window* w, Rect& client);  // window rect -> client rect, parent coords
void draw_scrollbars(Gui& g, Window* w, uint32_t hdc);

// Menus
void menu_bar_draw(Gui& g, Window* w, uint32_t hdc, const Rect& bar);
int menu_bar_layout(Gui& g, Window* w, int width);  // returns height
void menu_bar_track(Gui& g, Window* w, Point screen, bool keyboard, int key);
uint32_t track_popup_menu(Gui& g, uint32_t hmenu, uint32_t flags, int x, int y, uint32_t owner,
                          const Rect* exclude);
bool translate_accelerator(Gui& g, uint32_t hwnd, uint32_t haccel, const Msg& m);
uint32_t load_menu_template(Gui& g, uint64_t addr);

// Dialog creation (dialog.cpp)
uint32_t create_dialog(Gui& g, uint64_t hinst, uint64_t tmpl, uint32_t parent, uint64_t proc,
                       uint64_t param, bool ansi);
int64_t run_dialog_modal(Gui& g, uint32_t hdlg, uint32_t owner);

// Resources (res.cpp)
uint32_t load_bitmap_resource(Gui& g, uint64_t hinst, uint64_t name);
uint32_t load_icon_or_cursor(Gui& g, uint64_t hinst, uint64_t name, bool icon, int cx, int cy);
uint32_t system_cursor(Gui& g, int idc);
uint32_t system_icon(Gui& g, int idi);
std::u16string load_string(Gui& g, uint64_t hinst, uint32_t id);

// Painting helpers
uint32_t begin_paint(Gui& g, uint32_t hwnd, Rect& paint_rc, bool& erase);
void end_paint(Gui& g, uint32_t hwnd, uint32_t hdc);
uint32_t get_dc(Gui& g, uint32_t hwnd, bool window, const Region* clip, uint32_t flags = 0);
void release_dc(Gui& g, uint32_t hdc);
uint32_t ctl_color(Gui& g, uint32_t hwnd, uint32_t hdc, uint32_t msg);
uint32_t get_sys_color_brush(Gui& g, int idx);

// Window helpers
uint32_t get_parent_or_owner(Gui& g, Window* w);
bool is_child_of(Gui& g, uint32_t child, uint32_t parent);
uint32_t next_tab_item(Gui& g, uint32_t dlg, uint32_t from, bool prev);
std::u16string window_text(Gui& g, uint32_t hwnd);
void notify_parent(Gui& g, Window* w, uint16_t code);
uint32_t control_id(Window* w);
// Font of a window as the controls see it (WM_GETFONT answer or the default).
uint32_t window_font(Gui& g, Window* w);

// Text drawing (user.cpp: DrawText)
int draw_text_impl(Gui& g, uint32_t hdc, std::u16string s, Rect& rc, uint32_t fmt, int tab = 8);

// Carets
void caret_draw(Gui& g, bool on);

}  // namespace x86emu::win32
