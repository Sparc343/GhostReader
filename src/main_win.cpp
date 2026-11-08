// main_win.cpp - Win32/GDI front end for the Markdown reader.
//
// Everything Windows-specific lives here.  Layout is done by layout.cpp; this
// file measures text with GDI, paints the resulting items, and handles input.
//
// Build: see Makefile (w64devkit / MinGW-w64, runs on Windows 7 and later).

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WINVER
#define WINVER 0x0601
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <cwchar>
#include <cwctype>
#include <map>
#include <string>
#include <vector>

extern "C" {
#include "cmark-gfm.h"
#include "cmark-gfm-core-extensions.h"
}

#include "layout.h"

using namespace mdr;

// GetProcAddress returns a generic function pointer; going through void* keeps
// GCC's -Wcast-function-type quiet.
template <typename T>
static T procAddress(HMODULE m, const char *name) {
    return reinterpret_cast<T>(reinterpret_cast<void *>(GetProcAddress(m, name)));
}

// ------------------------------------------------------------- constants ---

static const wchar_t *kAppName = L"GhostReader";
static const wchar_t *kClassName = L"GhostReaderWindow";

enum {
    IDM_OPEN = 100,
    IDM_RELOAD,
    IDM_CLOSE,
    IDM_BACK,
    IDM_FORWARD,
    IDM_EXIT,
    IDM_COPY,
    IDM_SELALL,
    IDM_FIND,
    IDM_FINDNEXT,
    IDM_FINDPREV,
    IDM_ZOOMIN,
    IDM_ZOOMOUT,
    IDM_ZOOMRESET,
    IDM_THEME,
    IDM_FULLSCREEN,
    IDM_NARROW,
    IDM_SETTINGS,
    IDM_ABOUT,
    IDC_FINDEDIT = 300
};

enum { TIMER_WATCH = 1, TIMER_AUTOSCROLL = 2 };
enum { WM_FIND_NEXT = WM_APP + 1, WM_FIND_PREV, WM_FIND_CLOSE };

static const int kScrollBarW = 10;

// ----------------------------------------------------------- GDI measurer --

static bool imageInfo(const std::string &url, int &w, int &h);

class GdiMeasurer : public Measurer {
public:
    GdiMeasurer() : dc(NULL) {}
    ~GdiMeasurer() {
        clear();
        if (dc) DeleteDC(dc);
    }
    void init() { dc = CreateCompatibleDC(NULL); }

    void setFaces(const std::wstring &body, const std::wstring &mono) {
        bodyFace = body;
        monoFace = mono;
        clear();
    }

    HFONT font(const FontSpec &f) {
        std::map<FontSpec, HFONT>::iterator it = fonts.find(f);
        if (it != fonts.end()) return it->second;
        LOGFONTW lf;
        std::memset(&lf, 0, sizeof lf);
        lf.lfHeight = -f.px;
        lf.lfWeight = (f.flags & F_BOLD) ? FW_BOLD : FW_NORMAL;
        lf.lfItalic = (f.flags & F_ITALIC) ? TRUE : FALSE;
        lf.lfUnderline = (f.flags & F_UNDERLINE) ? TRUE : FALSE;
        lf.lfStrikeOut = (f.flags & F_STRIKE) ? TRUE : FALSE;
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfOutPrecision = OUT_TT_PRECIS;
        lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
        lf.lfQuality = 5;  // CLEARTYPE_QUALITY
        lf.lfPitchAndFamily = (f.flags & F_MONO) ? (FIXED_PITCH | FF_MODERN) : (VARIABLE_PITCH | FF_ROMAN);
        const std::wstring &face = (f.flags & F_MONO) ? monoFace : bodyFace;
        std::wcsncpy(lf.lfFaceName, face.c_str(), LF_FACESIZE - 1);
        HFONT h = CreateFontIndirectW(&lf);
        fonts[f] = h;
        return h;
    }

    int textWidth(const FontSpec &f, const wchar_t *s, int n) override {
        if (n <= 0) return 0;
        HGDIOBJ old = SelectObject(dc, font(f));
        SIZE sz = {0, 0};
        GetTextExtentPoint32W(dc, s, n, &sz);
        SelectObject(dc, old);
        return sz.cx;
    }

    bool imageSize(const std::string &url, int &w, int &h) override { return imageInfo(url, w, h); }

    FontMetrics metrics(const FontSpec &f) override {
        std::map<FontSpec, FontMetrics>::iterator it = mets.find(f);
        if (it != mets.end()) return it->second;
        HGDIOBJ old = SelectObject(dc, font(f));
        TEXTMETRICW tm;
        GetTextMetricsW(dc, &tm);
        SelectObject(dc, old);
        FontMetrics m;
        m.ascent = tm.tmAscent;
        m.descent = tm.tmDescent;
        mets[f] = m;
        return m;
    }

private:
    void clear() {
        for (std::map<FontSpec, HFONT>::iterator it = fonts.begin(); it != fonts.end(); ++it)
            DeleteObject(it->second);
        fonts.clear();
        mets.clear();
    }
    HDC dc;
    std::wstring bodyFace, monoFace;
    std::map<FontSpec, HFONT> fonts;
    std::map<FontSpec, FontMetrics> mets;
};

// ------------------------------------------------------------------ state --

struct HistoryEntry {
    std::wstring path;
    int scrollY;
};

// What the user can change in the Settings window.  Stored in the registry.
enum { NCOLORS = 10 };

struct Prefs {
    bool dark = true;
    std::wstring bodyFace, codeFace;  // empty = pick the best installed font automatically
    int bodyPt = 12, codePt = 12;
    bool narrow = false;
    int narrowWidth = 760;
    bool hasColor[2][NCOLORS];  // [0] = dark theme, [1] = light theme: user changed this color?
    Color color[2][NCOLORS];
    Prefs() {
        std::memset(hasColor, 0, sizeof hasColor);
        std::memset(color, 0, sizeof color);
    }
};

struct App {
    HWND hwnd = NULL;
    HWND findEdit = NULL;
    HMENU menu = NULL;
    HINSTANCE inst = NULL;
    HACCEL accel = NULL;

    std::wstring path;
    FILETIME lastWrite = {0, 0};
    DWORD lastSize = 0;

    cmark_node *doc = NULL;
    Layout layout;
    GdiMeasurer meas;

    bool dark = true;
    Theme theme = Theme::dark();
    Options opt;
    double zoom = 1.0;
    int dpi = 96;
    int fontPt = 12;
    int codePt = 12;
    bool narrow = false;
    int narrowWidthPx = 760;
    std::wstring bodyFace, monoFace;

    int clientW = 0, clientH = 0;
    int scrollX = 0, scrollY = 0;

    // selection
    bool hasSel = false;
    TextPos selA, selB;
    bool dragging = false;
    bool moved = false;
    POINT downPt = {0, 0};
    DWORD lastDblClick = 0;

    // scrollbars
    bool hoverV = false, hoverH = false, dragV = false, dragH = false;
    int dragOffset = 0;
    bool trackingLeave = false;

    // find
    bool findOpen = false;
    std::wstring findText;
    std::vector<Match> matches;
    int curMatch = -1;

    // navigation history
    std::vector<HistoryEntry> back, forward;

    // full screen
    bool fullscreen = false;
    WINDOWPLACEMENT prevPlacement;
    LONG_PTR prevStyle = 0;

    Prefs prefs;
};

static App g;

// ------------------------------------------------------------- utilities ---

static std::wstring fileNameOf(const std::wstring &p) {
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? p : p.substr(s + 1);
}

static std::wstring dirOf(const std::wstring &p) {
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? L"" : p.substr(0, s + 1);
}

static std::wstring lowerCopy(std::wstring s) {
    for (size_t i = 0; i < s.size(); i++) s[i] = (wchar_t)towlower(s[i]);
    return s;
}

static std::wstring extOf(const std::wstring &p) {
    std::wstring f = fileNameOf(p);
    size_t d = f.find_last_of(L'.');
    return d == std::wstring::npos ? L"" : lowerCopy(f.substr(d));
}

static std::wstring utf8ToWideWin(const std::string &s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), NULL, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

static std::string wideToUtf8Win(const std::wstring &w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), NULL, 0, NULL, NULL);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, NULL, NULL);
    return s;
}

static bool validUtf8(const std::string &s) {
    size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        int extra = 0;
        if (c < 0x80) extra = 0;
        else if ((c & 0xE0) == 0xC0 && c >= 0xC2) extra = 1;
        else if ((c & 0xF0) == 0xE0) extra = 2;
        else if ((c & 0xF8) == 0xF0 && c <= 0xF4) extra = 3;
        else return false;
        for (int k = 1; k <= extra; k++) {
            if (i + k >= n || ((unsigned char)s[i + k] & 0xC0) != 0x80) return false;
        }
        i += extra + 1;
    }
    return true;
}

static bool readWholeFile(const std::wstring &path, std::string &out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart > 256LL * 1024 * 1024) {
        CloseHandle(h);
        return false;
    }
    std::string buf((size_t)sz.QuadPart, '\0');
    size_t got = 0;
    while (got < buf.size()) {
        DWORD r = 0;
        if (!ReadFile(h, &buf[got], (DWORD)std::min<size_t>(buf.size() - got, 1u << 24), &r, NULL) || r == 0) break;
        got += r;
    }
    CloseHandle(h);
    buf.resize(got);

    // Encoding: UTF-8 (with/without BOM), UTF-16 LE/BE BOM, else the ANSI code page.
    if (buf.size() >= 3 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF) {
        out = buf.substr(3);
    } else if (buf.size() >= 2 && (unsigned char)buf[0] == 0xFF && (unsigned char)buf[1] == 0xFE) {
        std::wstring w((const wchar_t *)(buf.data() + 2), (buf.size() - 2) / 2);
        out = wideToUtf8Win(w);
    } else if (buf.size() >= 2 && (unsigned char)buf[0] == 0xFE && (unsigned char)buf[1] == 0xFF) {
        std::wstring w((buf.size() - 2) / 2, L'\0');
        for (size_t i = 0; i < w.size(); i++)
            w[i] = (wchar_t)(((unsigned char)buf[2 + 2 * i] << 8) | (unsigned char)buf[3 + 2 * i]);
        out = wideToUtf8Win(w);
    } else if (validUtf8(buf)) {
        out = buf;
    } else {
        int n = MultiByteToWideChar(CP_ACP, 0, buf.data(), (int)buf.size(), NULL, 0);
        std::wstring w(n, L'\0');
        if (n) MultiByteToWideChar(CP_ACP, 0, buf.data(), (int)buf.size(), &w[0], n);
        out = wideToUtf8Win(w);
    }
    return true;
}

static bool getFileStamp(const std::wstring &path, FILETIME &ft, DWORD &size) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) return false;
    ft = d.ftLastWriteTime;
    size = d.nFileSizeLow;
    return true;
}

// ------------------------------------------------------------- settings ----
// All settings live in the registry: HKEY_CURRENT_USER\Software\GhostReader

static int CALLBACK faceProc(const LOGFONTW *, const TEXTMETRICW *, DWORD, LPARAM lp) {
    *(bool *)lp = true;
    return 0;
}

static bool faceExists(const wchar_t *face) {
    HDC dc = GetDC(NULL);
    LOGFONTW lf;
    std::memset(&lf, 0, sizeof lf);
    lf.lfCharSet = DEFAULT_CHARSET;
    std::wcsncpy(lf.lfFaceName, face, LF_FACESIZE - 1);
    bool found = false;
    EnumFontFamiliesExW(dc, &lf, (FONTENUMPROCW)faceProc, (LPARAM)&found, 0);
    ReleaseDC(NULL, dc);
    return found;
}

static std::wstring pickFace(const std::wstring &wanted, const wchar_t *const *candidates) {
    if (!wanted.empty() && faceExists(wanted.c_str())) return wanted;
    for (int i = 0; candidates[i]; i++)
        if (faceExists(candidates[i])) return candidates[i];
    return candidates[0];
}

static const wchar_t *kRegKey = L"Software\\GhostReader";

// Names used in the registry (no spaces) and in the Settings window.
static const wchar_t *kColorKeys[NCOLORS] = {L"Background", L"Text",       L"Headings",    L"Links",  L"Code",
                                             L"BlockQuotes", L"TableLines", L"TableStripes", L"Rules", L"Selection"};
static const wchar_t *kColorLabels[NCOLORS] = {L"Background",  L"Text",         L"Headings",         L"Links",
                                               L"Code",        L"Block quotes", L"Table lines",      L"Table stripes",
                                               L"Rules and borders", L"Selection"};

static Color &themeColorRef(Theme &t, int i) {
    switch (i) {
        case 0: return t.background;
        case 1: return t.text;
        case 2: return t.heading;
        case 3: return t.link;
        case 4: return t.code;
        case 5: return t.quote;
        case 6: return t.grid;
        case 7: return t.stripe;
        case 8: return t.separator;
        default: return t.selection;
    }
}

// The theme the reader actually uses: built-in colors plus the user's changes.
static Theme effectiveTheme(bool dark) {
    Theme t = dark ? Theme::dark() : Theme::light();
    int which = dark ? 0 : 1;
    for (int i = 0; i < NCOLORS; i++)
        if (g.prefs.hasColor[which][i]) themeColorRef(t, i) = g.prefs.color[which][i];
    return t;
}

class RegReader {
public:
    RegReader() : key(NULL) {
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) key = NULL;
    }
    ~RegReader() {
        if (key) RegCloseKey(key);
    }
    bool has(const wchar_t *name, DWORD &out) const {
        if (!key) return false;
        DWORD type = 0, size = sizeof out, v = 0;
        if (RegQueryValueExW(key, name, NULL, &type, (BYTE *)&v, &size) != ERROR_SUCCESS || type != REG_DWORD)
            return false;
        out = v;
        return true;
    }
    DWORD dword(const wchar_t *name, DWORD def) const {
        DWORD v = 0;
        return has(name, v) ? v : def;
    }
    std::wstring str(const wchar_t *name) const {
        if (!key) return L"";
        wchar_t buf[256];
        DWORD type = 0, size = sizeof buf - sizeof(wchar_t);
        std::memset(buf, 0, sizeof buf);
        if (RegQueryValueExW(key, name, NULL, &type, (BYTE *)buf, &size) != ERROR_SUCCESS || type != REG_SZ) return L"";
        return buf;
    }

private:
    HKEY key;
};

class RegWriter {
public:
    RegWriter() : key(NULL) {
        if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL) != ERROR_SUCCESS)
            key = NULL;
    }
    ~RegWriter() {
        if (key) RegCloseKey(key);
    }
    void dword(const wchar_t *name, DWORD v) {
        if (key) RegSetValueExW(key, name, 0, REG_DWORD, (const BYTE *)&v, sizeof v);
    }
    void str(const wchar_t *name, const std::wstring &s) {
        if (key) RegSetValueExW(key, name, 0, REG_SZ, (const BYTE *)s.c_str(), (DWORD)((s.size() + 1) * sizeof(wchar_t)));
    }
    void remove(const wchar_t *name) {
        if (key) RegDeleteValueW(key, name);  // fine if it does not exist
    }

private:
    HKEY key;
};

static std::wstring colorValueName(int which, int i) {
    return std::wstring(which == 0 ? L"DarkColor" : L"LightColor") + kColorKeys[i];
}

static DWORD clampDword(DWORD v, DWORD lo, DWORD hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void loadPrefs() {
    RegReader r;
    Prefs &p = g.prefs;
    p = Prefs();
    p.dark = r.dword(L"DarkTheme", 1) != 0;
    p.bodyFace = r.str(L"BodyFont");
    p.codeFace = r.str(L"CodeFont");
    p.bodyPt = (int)clampDword(r.dword(L"BodyPt", 12), 6, 72);
    p.codePt = (int)clampDword(r.dword(L"CodePt", 12), 6, 72);
    p.narrow = r.dword(L"Narrow", 0) != 0;
    p.narrowWidth = (int)clampDword(r.dword(L"NarrowWidth", 760), 300, 3000);
    for (int w = 0; w < 2; w++) {
        for (int i = 0; i < NCOLORS; i++) {
            DWORD v;
            if (r.has(colorValueName(w, i).c_str(), v)) {
                p.hasColor[w][i] = true;
                p.color[w][i] = (Color)(v & 0xFFFFFF);
            }
        }
    }
    g.zoom = clampDword(r.dword(L"Zoom", 100), 40, 400) / 100.0;
}

static void savePrefs() {
    RegWriter w;
    const Prefs &p = g.prefs;
    w.dword(L"DarkTheme", p.dark ? 1 : 0);
    if (p.bodyFace.empty()) w.remove(L"BodyFont");
    else w.str(L"BodyFont", p.bodyFace);
    if (p.codeFace.empty()) w.remove(L"CodeFont");
    else w.str(L"CodeFont", p.codeFace);
    w.dword(L"BodyPt", (DWORD)p.bodyPt);
    w.dword(L"CodePt", (DWORD)p.codePt);
    w.dword(L"Narrow", p.narrow ? 1 : 0);
    w.dword(L"NarrowWidth", (DWORD)p.narrowWidth);
    for (int which = 0; which < 2; which++) {
        for (int i = 0; i < NCOLORS; i++) {
            std::wstring name = colorValueName(which, i);
            if (p.hasColor[which][i]) w.dword(name.c_str(), (DWORD)p.color[which][i]);
            else w.remove(name.c_str());
        }
    }
    w.dword(L"Zoom", (DWORD)std::lround(g.zoom * 100));
    if (g.hwnd) {
        WINDOWPLACEMENT wp;
        wp.length = sizeof wp;
        if (g.fullscreen) wp = g.prevPlacement;
        else GetWindowPlacement(g.hwnd, &wp);
        w.dword(L"WinX", (DWORD)wp.rcNormalPosition.left);
        w.dword(L"WinY", (DWORD)wp.rcNormalPosition.top);
        w.dword(L"WinW", (DWORD)(wp.rcNormalPosition.right - wp.rcNormalPosition.left));
        w.dword(L"WinH", (DWORD)(wp.rcNormalPosition.bottom - wp.rcNormalPosition.top));
        w.dword(L"Maximized", wp.showCmd == SW_SHOWMAXIMIZED ? 1 : 0);
    }
}

// --------------------------------------------------------------- layout ----

static COLORREF rgb(Color c) { return RGB((c >> 16) & 255, (c >> 8) & 255, c & 255); }

static int maxScrollY() { return std::max(0, g.layout.contentH - g.clientH); }
static int maxScrollX() { return std::max(0, g.layout.contentW - g.clientW); }

static void clampScroll() {
    g.scrollY = std::min(std::max(0, g.scrollY), maxScrollY());
    g.scrollX = std::min(std::max(0, g.scrollX), maxScrollX());
}

static void computeOptions() {
    double dpiScale = g.dpi / 96.0;
    g.opt.bodyPx = std::max(6, (int)std::lround(g.fontPt * g.dpi / 72.0 * g.zoom));
    g.opt.codePx = std::max(6, (int)std::lround(g.codePt * g.dpi / 72.0 * g.zoom));
    g.opt.pageLeft = (int)std::lround(5 * dpiScale);
    g.opt.pageRight = (int)std::lround((5 + kScrollBarW + 2) * dpiScale);
    g.opt.pageTop = (int)std::lround(13 * dpiScale);
    g.opt.pageBottom = (int)std::lround(24 * dpiScale);
    g.opt.imageScale = dpiScale * g.zoom;
    g.opt.maxTextWidth = g.narrow ? (int)std::lround(g.narrowWidthPx * dpiScale * g.zoom) : 0;
}

static void clearSelection() {
    g.hasSel = false;
    g.selA = g.selB = TextPos();
}

static void recomputeFind();

static void relayout() {
    if (!g.doc || g.clientW <= 0) return;
    computeOptions();
    buildLayout(g.layout, g.doc, g.meas, g.theme, g.opt, g.clientW);
    clearSelection();
    recomputeFind();
    clampScroll();
}

static void parseSource(const std::string &src) {
    if (g.doc) {
        cmark_node_free(g.doc);
        g.doc = NULL;
    }
    cmark_parser *p = cmark_parser_new(CMARK_OPT_DEFAULT | CMARK_OPT_VALIDATE_UTF8);
    static const char *names[] = {"table", "strikethrough", "autolink", "tasklist"};
    for (int i = 0; i < 4; i++) {
        cmark_syntax_extension *ext = cmark_find_syntax_extension(names[i]);
        if (ext) cmark_parser_attach_syntax_extension(p, ext);
    }
    cmark_parser_feed(p, src.data(), src.size());
    g.doc = cmark_parser_finish(p);
    cmark_parser_free(p);
}

static void updateTitle() {
    std::wstring t;
    if (g.path.empty()) t = kAppName;
    else t = fileNameOf(g.path) + L" - " + kAppName;
    if (g.findOpen && !g.findText.empty()) {
        if (g.matches.empty()) t += L"   [no matches]";
        else t += L"   [" + std::to_wstring(g.curMatch + 1) + L" of " + std::to_wstring(g.matches.size()) + L"]";
    }
    SetWindowTextW(g.hwnd, t.c_str());
}

static void updateMenuState() {
    if (!g.menu) return;
    bool has = !g.path.empty();
    EnableMenuItem(g.menu, IDM_RELOAD, MF_BYCOMMAND | (has ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(g.menu, IDM_CLOSE, MF_BYCOMMAND | (has ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(g.menu, IDM_BACK, MF_BYCOMMAND | (g.back.empty() ? MF_GRAYED : MF_ENABLED));
    EnableMenuItem(g.menu, IDM_FORWARD, MF_BYCOMMAND | (g.forward.empty() ? MF_GRAYED : MF_ENABLED));
}

static void clearImages();

static const char *kWelcome =
    "# GhostReader\n\n"
    "Open a Markdown file with **Ctrl+O**, or drop one onto this window.\n\n"
    "- **Ctrl + mouse wheel** or **Ctrl +** / **Ctrl -** to zoom\n"
    "- **Ctrl+F** to find, **F3** for the next match\n"
    "- **Ctrl+T** switches between the dark and light theme\n"
    "- **F11** toggles full screen\n"
    "- **Ctrl+W** closes the document and returns to this page\n"
    "- **Ctrl+,** opens Settings: fonts, colors, theme and reading column\n"
    "- **F5** reloads; the page also reloads by itself when the file changes\n";

static void showText(const std::string &src, const std::wstring &path, bool keepScroll) {
    double ratio = 0;
    if (keepScroll && g.layout.contentH > 0) ratio = (double)g.scrollY / (double)g.layout.contentH;
    parseSource(src);
    g.path = path;
    if (!path.empty()) getFileStamp(path, g.lastWrite, g.lastSize);
    clearImages();  // pictures are re-read from disk whenever the page is (re)loaded
    relayout();
    g.scrollY = keepScroll ? (int)(ratio * g.layout.contentH) : 0;
    if (!keepScroll) g.scrollX = 0;
    clampScroll();
    updateTitle();
    updateMenuState();
    InvalidateRect(g.hwnd, NULL, FALSE);
}

// Close the current document and go back to the start page.
static void closeDocument() {
    if (g.path.empty()) return;
    g.back.clear();
    g.forward.clear();
    showText(kWelcome, L"", false);
}

static std::wstring fullPathOf(const std::wstring &p) {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetFullPathNameW(p.c_str(), MAX_PATH * 4, buf, NULL);
    return (n > 0 && n < MAX_PATH * 4) ? std::wstring(buf) : p;
}

static bool loadFile(const std::wstring &pathIn, bool keepScroll, bool recordHistory) {
    std::wstring path = fullPathOf(pathIn);
    std::string src;
    if (!readWholeFile(path, src)) return false;
    if (recordHistory && !g.path.empty() && g.path != path) {
        HistoryEntry e = {g.path, g.scrollY};
        g.back.push_back(e);
        g.forward.clear();
    }
    showText(src, path, keepScroll);
    return true;
}

// ------------------------------------------------------------ scrolling ----

static void setScroll(int x, int y) {
    int ox = g.scrollX, oy = g.scrollY;
    g.scrollX = x;
    g.scrollY = y;
    clampScroll();
    if (ox != g.scrollX || oy != g.scrollY) InvalidateRect(g.hwnd, NULL, FALSE);
}

static void scrollBy(int dx, int dy) { setScroll(g.scrollX + dx, g.scrollY + dy); }

static int lineStep() { return std::max(16, g.opt.bodyPx * 2); }

static bool vBar(RECT &track, RECT &thumb) {
    if (maxScrollY() <= 0) return false;
    bool h = maxScrollX() > 0;
    track.left = g.clientW - kScrollBarW;
    track.right = g.clientW;
    track.top = 0;
    track.bottom = g.clientH - (h ? kScrollBarW : 0);
    int len = track.bottom - track.top;
    int tl = std::max(40, (int)((double)len * g.clientH / g.layout.contentH));
    tl = std::min(tl, len);
    int pos = (int)((double)(len - tl) * g.scrollY / maxScrollY());
    thumb = track;
    thumb.top = track.top + pos;
    thumb.bottom = thumb.top + tl;
    return true;
}

static bool hBar(RECT &track, RECT &thumb) {
    if (maxScrollX() <= 0) return false;
    bool v = maxScrollY() > 0;
    track.top = g.clientH - kScrollBarW;
    track.bottom = g.clientH;
    track.left = 0;
    track.right = g.clientW - (v ? kScrollBarW : 0);
    int len = track.right - track.left;
    int tl = std::max(40, (int)((double)len * g.clientW / g.layout.contentW));
    tl = std::min(tl, len);
    int pos = (int)((double)(len - tl) * g.scrollX / maxScrollX());
    thumb = track;
    thumb.left = track.left + pos;
    thumb.right = thumb.left + tl;
    return true;
}

static bool ptIn(const RECT &r, int x, int y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

// ----------------------------------------------------------------- find ----

static void ensureVisible(int y, int h) {
    if (y < g.scrollY + 20) setScroll(g.scrollX, y - g.clientH / 3);
    else if (y + h > g.scrollY + g.clientH - 20) setScroll(g.scrollX, y - g.clientH / 3);
}

static void gotoMatch(int idx) {
    if (g.matches.empty()) return;
    int n = (int)g.matches.size();
    g.curMatch = ((idx % n) + n) % n;
    const Match &m = g.matches[g.curMatch];
    const Item &it = g.layout.items[m.item];
    int x1 = m.off == 0 ? 0 : g.meas.textWidth(it.font, it.text.c_str(), m.off);
    ensureVisible(it.y, it.h);
    // horizontal: bring the match into view when the page scrolls sideways
    if (it.x + x1 < g.scrollX || it.x + x1 > g.scrollX + g.clientW - 40)
        setScroll(std::max(0, it.x + x1 - 60), g.scrollY);
    updateTitle();
    InvalidateRect(g.hwnd, NULL, FALSE);
}

static void recomputeFind() {
    g.matches.clear();
    g.curMatch = -1;
    if (g.findOpen && !g.findText.empty()) findAll(g.layout, g.findText, g.matches);
}

static void runFind(bool jumpFromTop) {
    recomputeFind();
    if (!g.matches.empty()) {
        // choose the first match at or below the top of the window
        int chosen = 0;
        if (!jumpFromTop) {
            for (size_t i = 0; i < g.matches.size(); i++) {
                if (g.layout.items[g.matches[i].item].y + g.layout.items[g.matches[i].item].h >= g.scrollY) {
                    chosen = (int)i;
                    break;
                }
            }
        }
        gotoMatch(chosen);
    } else {
        updateTitle();
        InvalidateRect(g.hwnd, NULL, FALSE);
    }
}

static void layoutFindBox() {
    if (!g.findEdit) return;
    int w = std::min(300, std::max(120, g.clientW - 40));
    SetWindowPos(g.findEdit, HWND_TOP, g.clientW - w - kScrollBarW - 8, 6, w, 26, g.findOpen ? SWP_SHOWWINDOW : SWP_HIDEWINDOW);
}

static void openFind() {
    g.findOpen = true;
    layoutFindBox();
    SetFocus(g.findEdit);
    SendMessageW(g.findEdit, EM_SETSEL, 0, -1);
    // re-run with whatever is in the box
    wchar_t buf[256];
    GetWindowTextW(g.findEdit, buf, 256);
    g.findText = buf;
    runFind(false);
}

static void closeFind() {
    g.findOpen = false;
    ShowWindow(g.findEdit, SW_HIDE);
    g.matches.clear();
    g.curMatch = -1;
    SetFocus(g.hwnd);
    updateTitle();
    InvalidateRect(g.hwnd, NULL, FALSE);
}

static WNDPROC g_oldEditProc = NULL;

static LRESULT CALLBACK editProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_KEYDOWN:
            if (w == VK_RETURN) {
                PostMessageW(g.hwnd, (GetKeyState(VK_SHIFT) & 0x8000) ? WM_FIND_PREV : WM_FIND_NEXT, 0, 0);
                return 0;
            }
            if (w == VK_ESCAPE) {
                PostMessageW(g.hwnd, WM_FIND_CLOSE, 0, 0);
                return 0;
            }
            if (w == VK_F3) {
                PostMessageW(g.hwnd, (GetKeyState(VK_SHIFT) & 0x8000) ? WM_FIND_PREV : WM_FIND_NEXT, 0, 0);
                return 0;
            }
            break;
        case WM_CHAR:
            if (w == VK_RETURN || w == VK_ESCAPE) return 0;  // avoid the "ding"
            break;
    }
    return CallWindowProcW(g_oldEditProc, h, m, w, l);
}

// ------------------------------------------------------------ clipboard ----

static void copySelection() {
    if (!g.hasSel) return;
    std::wstring t = selectedText(g.layout, g.selA, g.selB);
    if (t.empty()) return;
    std::wstring crlf;
    for (size_t i = 0; i < t.size(); i++) {
        if (t[i] == L'\n') crlf += L"\r\n";
        else crlf.push_back(t[i]);
    }
    if (!OpenClipboard(g.hwnd)) return;
    EmptyClipboard();
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (crlf.size() + 1) * sizeof(wchar_t));
    if (mem) {
        wchar_t *p = (wchar_t *)GlobalLock(mem);
        if (p) {
            std::memcpy(p, crlf.c_str(), (crlf.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(mem);
            SetClipboardData(CF_UNICODETEXT, mem);
        } else {
            GlobalFree(mem);
        }
    }
    CloseClipboard();
}

static void selectAll() {
    TextPos a, b;
    if (startOfText(g.layout, a) && endOfText(g.layout, b)) {
        g.selA = a;
        g.selB = b;
        g.hasSel = true;
        InvalidateRect(g.hwnd, NULL, FALSE);
    }
}

// ---------------------------------------------------------------- links ----

static std::string percentDecode(const std::string &s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() + 0 && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
            char hex[3] = {s[i + 1], s[i + 2], 0};
            o.push_back((char)strtol(hex, NULL, 16));
            i += 2;
        } else {
            o.push_back(s[i]);
        }
    }
    return o;
}

static bool startsWithNoCase(const std::string &s, const char *prefix) {
    size_t n = std::strlen(prefix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; i++)
        if (tolower((unsigned char)s[i]) != tolower((unsigned char)prefix[i])) return false;
    return true;
}

// ---------------------------------------------------------------- images ---
// Pictures are loaded with GDI+ (gdiplus.dll ships with Windows XP and later).
// It is bound at run time, so nothing extra is needed to build, and if the DLL
// were ever missing the reader simply shows the alt text instead.

struct GdiplusStartupInputW {
    UINT32 GdiplusVersion;
    void *DebugEventCallback;
    BOOL SuppressBackgroundThread;
    BOOL SuppressExternalCodecs;
};

struct GdipApi {
    bool tried = false, ok = false;
    HMODULE dll = NULL;
    ULONG_PTR token = 0;
    int(WINAPI *startup)(ULONG_PTR *, const GdiplusStartupInputW *, void *) = NULL;
    void(WINAPI *shutdown)(ULONG_PTR) = NULL;
    int(WINAPI *loadFromFile)(const wchar_t *, void **) = NULL;
    int(WINAPI *getWidth)(void *, UINT *) = NULL;
    int(WINAPI *getHeight)(void *, UINT *) = NULL;
    int(WINAPI *disposeImage)(void *) = NULL;
    int(WINAPI *createFromHdc)(HDC, void **) = NULL;
    int(WINAPI *deleteGraphics)(void *) = NULL;
    int(WINAPI *setInterpolation)(void *, int) = NULL;
    int(WINAPI *setPixelOffset)(void *, int) = NULL;
    int(WINAPI *drawImageRect)(void *, void *, int, int, int, int) = NULL;
};
static GdipApi g_gdip;

static bool gdipInit() {
    if (g_gdip.tried) return g_gdip.ok;
    g_gdip.tried = true;
    g_gdip.dll = LoadLibraryW(L"gdiplus.dll");
    if (!g_gdip.dll) return false;
    HMODULE d = g_gdip.dll;
#define GDIP_BIND(field, name) g_gdip.field = procAddress<decltype(g_gdip.field)>(d, name)
    GDIP_BIND(startup, "GdiplusStartup");
    GDIP_BIND(shutdown, "GdiplusShutdown");
    GDIP_BIND(loadFromFile, "GdipLoadImageFromFile");
    GDIP_BIND(getWidth, "GdipGetImageWidth");
    GDIP_BIND(getHeight, "GdipGetImageHeight");
    GDIP_BIND(disposeImage, "GdipDisposeImage");
    GDIP_BIND(createFromHdc, "GdipCreateFromHDC");
    GDIP_BIND(deleteGraphics, "GdipDeleteGraphics");
    GDIP_BIND(setInterpolation, "GdipSetInterpolationMode");
    GDIP_BIND(setPixelOffset, "GdipSetPixelOffsetMode");
    GDIP_BIND(drawImageRect, "GdipDrawImageRectI");
#undef GDIP_BIND
    GdipApi &a = g_gdip;
    if (!a.startup || !a.shutdown || !a.loadFromFile || !a.getWidth || !a.getHeight || !a.disposeImage ||
        !a.createFromHdc || !a.deleteGraphics || !a.setInterpolation || !a.setPixelOffset || !a.drawImageRect)
        return false;
    GdiplusStartupInputW in;
    in.GdiplusVersion = 1;
    in.DebugEventCallback = NULL;
    in.SuppressBackgroundThread = FALSE;
    in.SuppressExternalCodecs = FALSE;
    if (a.startup(&a.token, &in, NULL) != 0) return false;
    a.ok = true;
    return true;
}

struct ImageEntry {
    void *img = NULL;  // GDI+ image (NULL = could not be loaded)
    int w = 0, h = 0;  // natural size in pixels
    HBITMAP scaled = NULL;  // cached copy at the size it is currently drawn
    int sw = 0, sh = 0;
    Color bg = 0;
};
static std::map<std::string, ImageEntry> g_images;  // keyed by the url as written in the document

static void clearImages() {
    for (std::map<std::string, ImageEntry>::iterator it = g_images.begin(); it != g_images.end(); ++it) {
        if (it->second.scaled) DeleteObject(it->second.scaled);
        if (it->second.img && g_gdip.ok) g_gdip.disposeImage(it->second.img);
    }
    g_images.clear();
}

static bool resolveImagePath(const std::string &url, std::wstring &out) {
    if (g.path.empty()) return false;
    std::string u = url;
    if (startsWithNoCase(u, "http://") || startsWithNoCase(u, "https://") || startsWithNoCase(u, "ftp://") ||
        startsWithNoCase(u, "data:"))
        return false;  // remote pictures are not fetched
    size_t q = u.find_first_of("?#");
    if (q != std::string::npos) u = u.substr(0, q);
    if (startsWithNoCase(u, "file:///")) u = u.substr(8);
    else if (startsWithNoCase(u, "file://")) u = u.substr(7);
    u = percentDecode(u);
    std::wstring w = utf8ToWideWin(u);
    if (w.empty()) return false;
    for (size_t i = 0; i < w.size(); i++)
        if (w[i] == L'/') w[i] = L'\\';
    bool absolute = (w.size() > 1 && w[1] == L':') || (w.size() > 1 && w[0] == L'\\' && w[1] == L'\\');
    std::wstring full = absolute ? w : dirOf(g.path) + w;
    DWORD attr = GetFileAttributesW(full.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) return false;
    out = full;
    return true;
}

static bool imageInfo(const std::string &url, int &w, int &h) {
    std::map<std::string, ImageEntry>::iterator it = g_images.find(url);
    if (it == g_images.end()) {
        ImageEntry e;
        std::wstring path;
        if (gdipInit() && resolveImagePath(url, path)) {
            void *img = NULL;
            UINT iw = 0, ih = 0;
            if (g_gdip.loadFromFile(path.c_str(), &img) == 0 && img) {
                if (g_gdip.getWidth(img, &iw) == 0 && g_gdip.getHeight(img, &ih) == 0 && iw > 0 && ih > 0) {
                    e.img = img;
                    e.w = (int)iw;
                    e.h = (int)ih;
                } else {
                    g_gdip.disposeImage(img);
                }
            }
        }
        it = g_images.insert(std::make_pair(url, e)).first;  // failures are cached too
    }
    if (!it->second.img) return false;
    w = it->second.w;
    h = it->second.h;
    return true;
}

static void drawImage(HDC dc, const std::string &url, int x, int y, int w, int h) {
    std::map<std::string, ImageEntry>::iterator it = g_images.find(url);
    if (it == g_images.end() || !it->second.img || w <= 0 || h <= 0) return;
    ImageEntry &e = it->second;
    if (!e.scaled || e.sw != w || e.sh != h || e.bg != g.theme.background) {
        if (e.scaled) {
            DeleteObject(e.scaled);
            e.scaled = NULL;
        }
        if ((long long)w * h > 100LL * 1000 * 1000) return;
        HDC sdc = GetDC(NULL);
        HBITMAP bmp = CreateCompatibleBitmap(sdc, w, h);
        HDC mdc = CreateCompatibleDC(sdc);
        ReleaseDC(NULL, sdc);
        if (!bmp || !mdc) {
            if (bmp) DeleteObject(bmp);
            if (mdc) DeleteDC(mdc);
            return;
        }
        HGDIOBJ old = SelectObject(mdc, bmp);
        RECT r = {0, 0, w, h};
        HBRUSH br = CreateSolidBrush(rgb(g.theme.background));
        FillRect(mdc, &r, br);
        DeleteObject(br);
        void *gfx = NULL;
        if (g_gdip.createFromHdc(mdc, &gfx) == 0 && gfx) {
            g_gdip.setInterpolation(gfx, 7);  // HighQualityBicubic
            g_gdip.setPixelOffset(gfx, 4);    // PixelOffsetModeHalf
            g_gdip.drawImageRect(gfx, e.img, 0, 0, w, h);
            g_gdip.deleteGraphics(gfx);
        }
        SelectObject(mdc, old);
        DeleteDC(mdc);
        e.scaled = bmp;
        e.sw = w;
        e.sh = h;
        e.bg = g.theme.background;  // transparent pictures are blended onto the page color
    }
    HDC mdc = CreateCompatibleDC(dc);
    HGDIOBJ old = SelectObject(mdc, e.scaled);
    BitBlt(dc, x, y, w, h, mdc, 0, 0, SRCCOPY);
    SelectObject(mdc, old);
    DeleteDC(mdc);
}

static void scrollToAnchor(const std::string &frag) {
    std::string f = percentDecode(frag);
    for (size_t i = 0; i < f.size(); i++) f[i] = (char)tolower((unsigned char)f[i]);
    std::map<std::string, int>::iterator it = g.layout.anchors.find(f);
    if (it != g.layout.anchors.end()) setScroll(g.scrollX, it->second - g.opt.pageTop);
}

static void activateLink(const std::string &url) {
    if (url.empty()) return;
    if (url[0] == '#') {
        scrollToAnchor(url.substr(1));
        return;
    }
    if (startsWithNoCase(url, "http://") || startsWithNoCase(url, "https://") || startsWithNoCase(url, "mailto:")) {
        ShellExecuteW(g.hwnd, L"open", utf8ToWideWin(url).c_str(), NULL, NULL, SW_SHOWNORMAL);
        return;
    }
    // Anything else is treated as a local file path, relative to the current document.
    std::string u = url;
    std::string frag;
    size_t hash = u.find('#');
    if (hash != std::string::npos) {
        frag = u.substr(hash + 1);
        u = u.substr(0, hash);
    }
    if (startsWithNoCase(u, "file:///")) u = u.substr(8);
    else if (startsWithNoCase(u, "file://")) u = u.substr(7);
    u = percentDecode(u);
    std::wstring w = utf8ToWideWin(u);
    for (size_t i = 0; i < w.size(); i++)
        if (w[i] == L'/') w[i] = L'\\';
    bool absolute = (w.size() > 1 && w[1] == L':') || (w.size() > 1 && w[0] == L'\\' && w[1] == L'\\');
    std::wstring full = absolute ? w : dirOf(g.path) + w;
    if (w.empty()) {
        if (!frag.empty()) scrollToAnchor(frag);
        return;
    }
    DWORD attr = GetFileAttributesW(full.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(g.hwnd, (L"Cannot find:\n" + full).c_str(), kAppName, MB_OK | MB_ICONINFORMATION);
        return;
    }
    std::wstring ext = extOf(full);
    if (ext == L".md" || ext == L".markdown" || ext == L".mdown" || ext == L".txt") {
        if (loadFile(full, false, true) && !frag.empty()) scrollToAnchor(frag);
        return;
    }
    static const wchar_t *safe[] = {L".png", L".jpg", L".jpeg", L".gif", L".bmp", L".webp", L".svg", L".pdf", NULL};
    for (int i = 0; safe[i]; i++) {
        if (ext == safe[i]) {
            ShellExecuteW(g.hwnd, L"open", full.c_str(), NULL, NULL, SW_SHOWNORMAL);
            return;
        }
    }
    MessageBoxW(g.hwnd,
                L"This link points to a file type that is not opened from inside a document "
                L"(for safety, only Markdown, text, images and PDFs are).",
                kAppName, MB_OK | MB_ICONWARNING);
}

static void goBack() {
    if (g.back.empty()) return;
    HistoryEntry cur = {g.path, g.scrollY};
    HistoryEntry e = g.back.back();
    g.back.pop_back();
    if (loadFile(e.path, false, false)) {
        g.forward.push_back(cur);
        setScroll(0, e.scrollY);
        updateMenuState();
    }
}

static void goForward() {
    if (g.forward.empty()) return;
    HistoryEntry cur = {g.path, g.scrollY};
    HistoryEntry e = g.forward.back();
    g.forward.pop_back();
    if (loadFile(e.path, false, false)) {
        g.back.push_back(cur);
        setScroll(0, e.scrollY);
        updateMenuState();
    }
}

// ----------------------------------------------------------------- misc ----

static void openDialog() {
    wchar_t buf[MAX_PATH * 4] = L"";
    OPENFILENAMEW ofn;
    std::memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = g.hwnd;
    ofn.lpstrFilter = L"Markdown and text (*.md;*.markdown;*.mdown;*.txt)\0*.md;*.markdown;*.mdown;*.txt\0All files (*.*)\0*.*\0\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH * 4;
    std::wstring dir = dirOf(g.path);
    if (!dir.empty()) ofn.lpstrInitialDir = dir.c_str();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER;
    if (GetOpenFileNameW(&ofn)) {
        if (!loadFile(buf, false, true)) MessageBoxW(g.hwnd, L"Could not read that file.", kAppName, MB_OK | MB_ICONERROR);
    }
}

static void setZoom(double z) {
    z = std::min(4.0, std::max(0.4, z));
    if (std::fabs(z - g.zoom) < 1e-6) return;
    double ratio = g.layout.contentH > 0 ? (double)g.scrollY / g.layout.contentH : 0;
    g.zoom = z;
    relayout();
    g.scrollY = (int)(ratio * g.layout.contentH);
    clampScroll();
    InvalidateRect(g.hwnd, NULL, FALSE);
}

static void updateMenuChecks() {
    if (!g.menu) return;
    CheckMenuItem(g.menu, IDM_THEME, MF_BYCOMMAND | (g.dark ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(g.menu, IDM_FULLSCREEN, MF_BYCOMMAND | (g.fullscreen ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(g.menu, IDM_NARROW, MF_BYCOMMAND | (g.narrow ? MF_CHECKED : MF_UNCHECKED));
}

// Make the reader match g.prefs (fonts, theme, reading column) and redraw.
static void applyPrefs(bool redraw) {
    const Prefs &p = g.prefs;
    g.dark = p.dark;
    g.narrow = p.narrow;
    g.narrowWidthPx = p.narrowWidth;
    g.fontPt = p.bodyPt;
    g.codePt = p.codePt;
    static const wchar_t *bodyCandidates[] = {L"Noto Serif", L"Cambria", L"Georgia", L"Times New Roman", NULL};
    static const wchar_t *monoCandidates[] = {L"DejaVu Sans Mono", L"Consolas", L"Lucida Console", L"Courier New", NULL};
    g.bodyFace = pickFace(p.bodyFace, bodyCandidates);
    g.monoFace = pickFace(p.codeFace, monoCandidates);
    g.meas.setFaces(g.bodyFace, g.monoFace);
    g.theme = effectiveTheme(p.dark);
    updateMenuChecks();
    if (redraw) {
        double ratio = g.layout.contentH > 0 ? (double)g.scrollY / g.layout.contentH : 0;
        relayout();
        g.scrollY = (int)(ratio * g.layout.contentH);
        clampScroll();
        InvalidateRect(g.hwnd, NULL, FALSE);
    }
}

// ------------------------------------------------------- settings window ---
// A small modal window built in code.  Every change is applied to the reader
// immediately so you can watch the page; Cancel puts everything back.

enum {
    IDC_S_DARK = 500,
    IDC_S_LIGHT,
    IDC_S_BODYTXT,
    IDC_S_BODYBTN,
    IDC_S_CODETXT,
    IDC_S_CODEBTN,
    IDC_S_NARROW,
    IDC_S_WIDTH,
    IDC_S_RESTORE,
    IDC_S_COLORGROUP,
    IDC_S_COLOR0 = 600  // .. IDC_S_COLOR0 + NCOLORS - 1
};

struct SettingsDlg {
    HWND hwnd;
    bool done;
    bool updating;
    Prefs original;
    COLORREF custom[16];
    HWND colorBtn[NCOLORS];
};
static SettingsDlg sd;

static HWND addControl(HWND parent, const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h,
                       int id) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, parent, (HMENU)(UINT_PTR)id,
                             g.inst, NULL);
    if (c) SendMessageW(c, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    return c;
}

static void refreshSettingsControls() {
    if (!sd.hwnd) return;
    sd.updating = true;
    CheckRadioButton(sd.hwnd, IDC_S_DARK, IDC_S_LIGHT, g.prefs.dark ? IDC_S_DARK : IDC_S_LIGHT);
    SetDlgItemTextW(sd.hwnd, IDC_S_BODYTXT, (g.bodyFace + L",  " + std::to_wstring(g.prefs.bodyPt) + L" pt").c_str());
    SetDlgItemTextW(sd.hwnd, IDC_S_CODETXT, (g.monoFace + L",  " + std::to_wstring(g.prefs.codePt) + L" pt").c_str());
    CheckDlgButton(sd.hwnd, IDC_S_NARROW, g.prefs.narrow ? BST_CHECKED : BST_UNCHECKED);
    SetDlgItemInt(sd.hwnd, IDC_S_WIDTH, (UINT)g.prefs.narrowWidth, FALSE);
    EnableWindow(GetDlgItem(sd.hwnd, IDC_S_WIDTH), g.prefs.narrow ? TRUE : FALSE);
    SetDlgItemTextW(sd.hwnd, IDC_S_COLORGROUP, g.prefs.dark ? L"Colors (dark theme)" : L"Colors (light theme)");
    for (int i = 0; i < NCOLORS; i++)
        if (sd.colorBtn[i]) InvalidateRect(sd.colorBtn[i], NULL, TRUE);
    sd.updating = false;
}

static void chooseFontFor(bool code) {
    CHOOSEFONTW cf;
    LOGFONTW lf;
    std::memset(&cf, 0, sizeof cf);
    std::memset(&lf, 0, sizeof lf);
    const std::wstring &face = code ? g.monoFace : g.bodyFace;
    int pt = code ? g.prefs.codePt : g.prefs.bodyPt;
    std::wcsncpy(lf.lfFaceName, face.c_str(), LF_FACESIZE - 1);
    lf.lfHeight = -(int)std::lround(pt * g.dpi / 72.0);
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    cf.lStructSize = sizeof cf;
    cf.hwndOwner = sd.hwnd;
    cf.lpLogFont = &lf;
    cf.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT | CF_TTONLY | CF_LIMITSIZE | (code ? CF_FIXEDPITCHONLY : 0);
    cf.nSizeMin = 6;
    cf.nSizeMax = 72;
    if (!ChooseFontW(&cf)) return;
    int newPt = std::min(72, std::max(6, (int)cf.iPointSize / 10));
    if (code) {
        g.prefs.codeFace = lf.lfFaceName;
        g.prefs.codePt = newPt;
    } else {
        g.prefs.bodyFace = lf.lfFaceName;
        g.prefs.bodyPt = newPt;
    }
    applyPrefs(true);
    refreshSettingsControls();
}

static void chooseColorFor(int idx) {
    Theme cur = effectiveTheme(g.prefs.dark);
    CHOOSECOLORW cc;
    std::memset(&cc, 0, sizeof cc);
    cc.lStructSize = sizeof cc;
    cc.hwndOwner = sd.hwnd;
    cc.rgbResult = rgb(themeColorRef(cur, idx));
    cc.lpCustColors = sd.custom;
    cc.Flags = CC_RGBINIT | CC_FULLOPEN | CC_ANYCOLOR;
    if (!ChooseColorW(&cc)) return;
    COLORREF c = cc.rgbResult;  // 0x00BBGGRR
    Color v = ((Color)(c & 0xFF) << 16) | ((Color)((c >> 8) & 0xFF) << 8) | (Color)((c >> 16) & 0xFF);
    int which = g.prefs.dark ? 0 : 1;
    g.prefs.hasColor[which][idx] = true;
    g.prefs.color[which][idx] = v;
    applyPrefs(true);
    refreshSettingsControls();
}

static LRESULT CALLBACK settingsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            const DWORD tab = WS_TABSTOP;
            // Theme
            addControl(hwnd, L"BUTTON", L"Theme", BS_GROUPBOX, 10, 8, 460, 52, 0);
            addControl(hwnd, L"BUTTON", L"Dark", BS_AUTORADIOBUTTON | WS_GROUP | tab, 28, 28, 120, 22, IDC_S_DARK);
            addControl(hwnd, L"BUTTON", L"Light", BS_AUTORADIOBUTTON | tab, 160, 28, 120, 22, IDC_S_LIGHT);
            // Fonts
            addControl(hwnd, L"BUTTON", L"Fonts", BS_GROUPBOX, 10, 66, 460, 84, 0);
            addControl(hwnd, L"STATIC", L"Body text:", 0, 28, 94, 74, 20, 0);
            addControl(hwnd, L"STATIC", L"", 0, 106, 94, 256, 20, IDC_S_BODYTXT);
            addControl(hwnd, L"BUTTON", L"Change...", BS_PUSHBUTTON | tab, 372, 90, 88, 26, IDC_S_BODYBTN);
            addControl(hwnd, L"STATIC", L"Code:", 0, 28, 124, 74, 20, 0);
            addControl(hwnd, L"STATIC", L"", 0, 106, 124, 256, 20, IDC_S_CODETXT);
            addControl(hwnd, L"BUTTON", L"Change...", BS_PUSHBUTTON | tab, 372, 120, 88, 26, IDC_S_CODEBTN);
            // Colors
            addControl(hwnd, L"BUTTON", L"Colors", BS_GROUPBOX, 10, 156, 460, 190, IDC_S_COLORGROUP);
            for (int i = 0; i < NCOLORS; i++) {
                int col = i / 5, row = i % 5;
                int x = col == 0 ? 28 : 250;
                int y = 182 + row * 32;
                addControl(hwnd, L"STATIC", kColorLabels[i], 0, x, y + 3, 112, 20, 0);
                sd.colorBtn[i] = addControl(hwnd, L"BUTTON", L"", BS_OWNERDRAW | tab, x + 114, y, 64, 24, IDC_S_COLOR0 + i);
            }
            // Reading column
            addControl(hwnd, L"BUTTON", L"Reading column", BS_GROUPBOX, 10, 352, 460, 56, 0);
            addControl(hwnd, L"BUTTON", L"Narrow reading column", BS_AUTOCHECKBOX | tab, 28, 376, 196, 22, IDC_S_NARROW);
            addControl(hwnd, L"STATIC", L"Width:", 0, 240, 379, 46, 20, 0);
            HWND ed = addControl(hwnd, L"EDIT", L"", WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL | tab, 290, 374, 70, 24, IDC_S_WIDTH);
            if (ed) SendMessageW(ed, EM_LIMITTEXT, 4, 0);
            addControl(hwnd, L"STATIC", L"pixels", 0, 366, 379, 60, 20, 0);
            // Buttons
            addControl(hwnd, L"BUTTON", L"Restore Defaults", BS_PUSHBUTTON | tab, 10, 422, 140, 28, IDC_S_RESTORE);
            addControl(hwnd, L"BUTTON", L"OK", BS_DEFPUSHBUTTON | tab, 300, 422, 80, 28, IDOK);
            addControl(hwnd, L"BUTTON", L"Cancel", BS_PUSHBUTTON | tab, 390, 422, 80, 28, IDCANCEL);
            return 0;
        }
        case WM_DRAWITEM: {
            const DRAWITEMSTRUCT *d = (const DRAWITEMSTRUCT *)lp;
            int idx = (int)d->CtlID - IDC_S_COLOR0;
            if (idx >= 0 && idx < NCOLORS) {
                Theme t = effectiveTheme(g.prefs.dark);
                HBRUSH br = CreateSolidBrush(rgb(themeColorRef(t, idx)));
                FillRect(d->hDC, &d->rcItem, br);
                DeleteObject(br);
                FrameRect(d->hDC, &d->rcItem, (HBRUSH)GetStockObject(BLACK_BRUSH));
                if (d->itemState & ODS_FOCUS) {
                    RECT r = d->rcItem;
                    r.left += 3;
                    r.top += 3;
                    r.right -= 3;
                    r.bottom -= 3;
                    DrawFocusRect(d->hDC, &r);
                }
                return TRUE;
            }
            break;
        }
        case WM_COMMAND: {
            int id = LOWORD(wp), code = HIWORD(wp);
            if ((id == IDC_S_DARK || id == IDC_S_LIGHT) && code == BN_CLICKED) {
                g.prefs.dark = (id == IDC_S_DARK);
                applyPrefs(true);
                refreshSettingsControls();
            } else if (id == IDC_S_BODYBTN && code == BN_CLICKED) {
                chooseFontFor(false);
            } else if (id == IDC_S_CODEBTN && code == BN_CLICKED) {
                chooseFontFor(true);
            } else if (id >= IDC_S_COLOR0 && id < IDC_S_COLOR0 + NCOLORS && code == BN_CLICKED) {
                chooseColorFor(id - IDC_S_COLOR0);
            } else if (id == IDC_S_NARROW && code == BN_CLICKED) {
                g.prefs.narrow = IsDlgButtonChecked(hwnd, IDC_S_NARROW) == BST_CHECKED;
                applyPrefs(true);
                refreshSettingsControls();
            } else if (id == IDC_S_WIDTH && code == EN_CHANGE && !sd.updating) {
                BOOL ok = FALSE;
                UINT v = GetDlgItemInt(hwnd, IDC_S_WIDTH, &ok, FALSE);
                if (ok && v >= 300 && v <= 3000) {
                    g.prefs.narrowWidth = (int)v;
                    if (g.prefs.narrow) applyPrefs(true);
                }
            } else if (id == IDC_S_RESTORE && code == BN_CLICKED) {
                g.prefs = Prefs();
                applyPrefs(true);
                refreshSettingsControls();
            } else if (id == IDOK) {
                savePrefs();
                sd.done = true;
            } else if (id == IDCANCEL) {
                g.prefs = sd.original;
                applyPrefs(true);
                sd.done = true;
            }
            return 0;
        }
        case WM_CLOSE:
            g.prefs = sd.original;
            applyPrefs(true);
            sd.done = true;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void openSettings() {
    static bool registered = false;
    if (sd.hwnd) return;
    if (!registered) {
        WNDCLASSEXW wc;
        std::memset(&wc, 0, sizeof wc);
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = settingsProc;
        wc.hInstance = g.inst;
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(UINT_PTR)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"GhostReaderSettings";
        wc.hIcon = LoadIconW(g.inst, MAKEINTRESOURCEW(1));
        if (!RegisterClassExW(&wc)) return;
        registered = true;
    }
    sd.original = g.prefs;
    sd.done = false;
    sd.updating = false;
    std::memset(sd.colorBtn, 0, sizeof sd.colorBtn);

    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    const DWORD exStyle = WS_EX_DLGMODALFRAME;
    RECT r = {0, 0, 480, 462};
    AdjustWindowRectEx(&r, style, FALSE, exStyle);
    int w = (int)(r.right - r.left), h = (int)(r.bottom - r.top);
    RECT o;
    GetWindowRect(g.hwnd, &o);
    int x = (int)o.left + ((int)(o.right - o.left) - w) / 2;
    int y = (int)o.top + ((int)(o.bottom - o.top) - h) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    EnableWindow(g.hwnd, FALSE);
    sd.hwnd = CreateWindowExW(exStyle, L"GhostReaderSettings", L"Settings", style, x, y, w, h, g.hwnd, NULL, g.inst, NULL);
    if (!sd.hwnd) {
        EnableWindow(g.hwnd, TRUE);
        return;
    }
    refreshSettingsControls();
    ShowWindow(sd.hwnd, SW_SHOW);
    UpdateWindow(sd.hwnd);

    MSG m;
    while (!sd.done && GetMessageW(&m, NULL, 0, 0) > 0) {
        if (!IsDialogMessage(sd.hwnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    if (!sd.done) PostQuitMessage(0);  // the program is closing: pass the quit along
    DestroyWindow(sd.hwnd);
    sd.hwnd = NULL;
    EnableWindow(g.hwnd, TRUE);
    SetForegroundWindow(g.hwnd);
    SetFocus(g.hwnd);
}

static void toggleTheme() {
    g.prefs.dark = !g.prefs.dark;
    applyPrefs(true);
}

static void toggleNarrow() {
    g.prefs.narrow = !g.prefs.narrow;
    applyPrefs(true);
}

static void toggleFullscreen() {
    if (!g.fullscreen) {
        g.prevStyle = GetWindowLongPtrW(g.hwnd, GWL_STYLE);
        g.prevPlacement.length = sizeof g.prevPlacement;
        GetWindowPlacement(g.hwnd, &g.prevPlacement);
        MONITORINFO mi;
        mi.cbSize = sizeof mi;
        GetMonitorInfoW(MonitorFromWindow(g.hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetMenu(g.hwnd, NULL);
        SetWindowLongPtrW(g.hwnd, GWL_STYLE, g.prevStyle & ~(LONG_PTR)WS_OVERLAPPEDWINDOW);
        SetWindowPos(g.hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g.fullscreen = true;
    } else {
        SetWindowLongPtrW(g.hwnd, GWL_STYLE, g.prevStyle);
        SetMenu(g.hwnd, g.menu);
        SetWindowPlacement(g.hwnd, &g.prevPlacement);
        SetWindowPos(g.hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g.fullscreen = false;
    }
    updateMenuChecks();
}

static HMENU buildMenu() {
    HMENU bar = CreateMenu();
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, IDM_OPEN, L"&Open...\tCtrl+O");
    AppendMenuW(file, MF_STRING, IDM_RELOAD, L"&Reload\tF5");
    AppendMenuW(file, MF_STRING, IDM_CLOSE, L"&Close\tCtrl+W");
    AppendMenuW(file, MF_SEPARATOR, 0, NULL);
    AppendMenuW(file, MF_STRING, IDM_BACK, L"&Back\tAlt+Left");
    AppendMenuW(file, MF_STRING, IDM_FORWARD, L"&Forward\tAlt+Right");
    AppendMenuW(file, MF_SEPARATOR, 0, NULL);
    AppendMenuW(file, MF_STRING, IDM_EXIT, L"E&xit\tCtrl+Q");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)file, L"&File");

    HMENU edit = CreatePopupMenu();
    AppendMenuW(edit, MF_STRING, IDM_COPY, L"&Copy\tCtrl+C");
    AppendMenuW(edit, MF_STRING, IDM_SELALL, L"Select &All\tCtrl+A");
    AppendMenuW(edit, MF_SEPARATOR, 0, NULL);
    AppendMenuW(edit, MF_STRING, IDM_FIND, L"&Find...\tCtrl+F");
    AppendMenuW(edit, MF_STRING, IDM_FINDNEXT, L"Find &Next\tF3");
    AppendMenuW(edit, MF_STRING, IDM_FINDPREV, L"Find &Previous\tShift+F3");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)edit, L"&Edit");

    HMENU view = CreatePopupMenu();
    AppendMenuW(view, MF_STRING, IDM_ZOOMIN, L"Zoom &In\tCtrl++");
    AppendMenuW(view, MF_STRING, IDM_ZOOMOUT, L"Zoom &Out\tCtrl+-");
    AppendMenuW(view, MF_STRING, IDM_ZOOMRESET, L"&Actual Size\tCtrl+0");
    AppendMenuW(view, MF_SEPARATOR, 0, NULL);
    AppendMenuW(view, MF_STRING, IDM_THEME, L"&Dark Theme\tCtrl+T");
    AppendMenuW(view, MF_STRING, IDM_NARROW, L"&Narrow Reading Column\tCtrl+N");
    AppendMenuW(view, MF_STRING, IDM_FULLSCREEN, L"&Full Screen\tF11");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)view, L"&View");

    HMENU settings = CreatePopupMenu();
    AppendMenuW(settings, MF_STRING, IDM_SETTINGS, L"&Preferences...\tCtrl+,");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)settings, L"&Settings");

    HMENU help = CreatePopupMenu();
    AppendMenuW(help, MF_STRING, IDM_ABOUT, L"&About GhostReader");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)help, L"&Help");
    return bar;
}

static HACCEL buildAccelerators() {
    ACCEL a[] = {
        {FVIRTKEY | FCONTROL, 'O', IDM_OPEN},
        {FVIRTKEY, VK_F5, IDM_RELOAD},
        {FVIRTKEY | FCONTROL, 'R', IDM_RELOAD},
        {FVIRTKEY | FCONTROL, 'W', IDM_CLOSE},
        {FVIRTKEY | FCONTROL, VK_F4, IDM_CLOSE},
        {FVIRTKEY | FALT, VK_LEFT, IDM_BACK},
        {FVIRTKEY | FALT, VK_RIGHT, IDM_FORWARD},
        {FVIRTKEY | FCONTROL, 'Q', IDM_EXIT},
        {FVIRTKEY | FCONTROL, 'C', IDM_COPY},
        {FVIRTKEY | FCONTROL, 'A', IDM_SELALL},
        {FVIRTKEY | FCONTROL, 'F', IDM_FIND},
        {FVIRTKEY, VK_F3, IDM_FINDNEXT},
        {FVIRTKEY | FSHIFT, VK_F3, IDM_FINDPREV},
        {FVIRTKEY | FCONTROL, VK_OEM_PLUS, IDM_ZOOMIN},
        {FVIRTKEY | FCONTROL | FSHIFT, VK_OEM_PLUS, IDM_ZOOMIN},
        {FVIRTKEY | FCONTROL, VK_ADD, IDM_ZOOMIN},
        {FVIRTKEY | FCONTROL, VK_OEM_MINUS, IDM_ZOOMOUT},
        {FVIRTKEY | FCONTROL, VK_SUBTRACT, IDM_ZOOMOUT},
        {FVIRTKEY | FCONTROL, '0', IDM_ZOOMRESET},
        {FVIRTKEY | FCONTROL, VK_NUMPAD0, IDM_ZOOMRESET},
        {FVIRTKEY | FCONTROL, 'T', IDM_THEME},
        {FVIRTKEY | FCONTROL, 'N', IDM_NARROW},
        {FVIRTKEY | FCONTROL, VK_OEM_COMMA, IDM_SETTINGS},
        {FVIRTKEY, VK_F11, IDM_FULLSCREEN},
    };
    return CreateAcceleratorTableW(a, sizeof a / sizeof a[0]);
}

// ------------------------------------------------------------- painting ----

static void fillRectColor(HDC dc, int x, int y, int w, int h, Color c) {
    if (w <= 0 || h <= 0) return;
    RECT r = {x, y, x + w, y + h};
    SetDCBrushColor(dc, rgb(c));
    FillRect(dc, &r, (HBRUSH)GetStockObject(DC_BRUSH));
}

static void drawCheck(HDC dc, int x, int y, int s, Color c, bool checked) {
    SetDCPenColor(dc, rgb(c));
    HGDIOBJ oldPen = SelectObject(dc, GetStockObject(DC_PEN));
    HGDIOBJ oldBr = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, x, y, x + s, y + s);
    if (checked) {
        MoveToEx(dc, x + s / 5, y + s / 2, NULL);
        LineTo(dc, x + s * 2 / 5, y + s * 7 / 10);
        LineTo(dc, x + s * 4 / 5, y + s / 4);
        MoveToEx(dc, x + s / 5, y + s / 2 + 1, NULL);
        LineTo(dc, x + s * 2 / 5, y + s * 7 / 10 + 1);
        LineTo(dc, x + s * 4 / 5, y + s / 4 + 1);
    }
    SelectObject(dc, oldBr);
    SelectObject(dc, oldPen);
}

static void paintScrollbars(HDC dc) {
    RECT track, thumb;
    HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    HGDIOBJ oldBr = SelectObject(dc, GetStockObject(DC_BRUSH));
    if (vBar(track, thumb)) {
        SetDCBrushColor(dc, rgb((g.hoverV || g.dragV) ? g.theme.scrollThumbHover : g.theme.scrollThumb));
        RoundRect(dc, thumb.left + 2, thumb.top, thumb.right - 1, thumb.bottom, 6, 6);
    }
    if (hBar(track, thumb)) {
        SetDCBrushColor(dc, rgb((g.hoverH || g.dragH) ? g.theme.scrollThumbHover : g.theme.scrollThumb));
        RoundRect(dc, thumb.left, thumb.top + 2, thumb.right, thumb.bottom - 1, 6, 6);
    }
    SelectObject(dc, oldBr);
    SelectObject(dc, oldPen);
}

static void paintAll(HDC dc, int W, int H) {
    fillRectColor(dc, 0, 0, W, H, g.theme.background);
    SetBkMode(dc, TRANSPARENT);
    SetTextAlign(dc, TA_BASELINE | TA_LEFT | TA_NOUPDATECP);

    const int top = g.scrollY, bottom = g.scrollY + H;
    const int left = g.scrollX, right = g.scrollX + W;

    // find highlights (behind the text)
    if (g.findOpen && !g.matches.empty()) {
        for (size_t i = 0; i < g.matches.size(); i++) {
            const Match &m = g.matches[i];
            const Item &it = g.layout.items[m.item];
            if (it.y + it.h < top || it.y > bottom) continue;
            int x1 = m.off == 0 ? 0 : g.meas.textWidth(it.font, it.text.c_str(), m.off);
            int x2 = g.meas.textWidth(it.font, it.text.c_str(), m.off + m.len);
            Color c = ((int)i == g.curMatch) ? (g.dark ? 0x9a6b1f : 0xffc94d) : (g.dark ? 0x5c4b1e : 0xfff3a8);
            fillRectColor(dc, it.x + x1 - left, it.y - top, x2 - x1, it.h, c);
        }
    }
    // selection highlight
    if (g.hasSel) {
        std::vector<Rect> rects;
        selectionRects(g.layout, g.meas, g.selA, g.selB, rects);
        for (size_t i = 0; i < rects.size(); i++) {
            const Rect &r = rects[i];
            if (r.y + r.h < top || r.y > bottom) continue;
            fillRectColor(dc, r.x - left, r.y - top, r.w, r.h, g.theme.selection);
        }
    }

    HGDIOBJ oldFont = GetCurrentObject(dc, OBJ_FONT);
    for (size_t i = 0; i < g.layout.items.size(); i++) {
        const Item &it = g.layout.items[i];
        if (it.y + it.h < top || it.y > bottom || it.x + it.w < left || it.x > right) continue;
        int x = it.x - left, y = it.y - top;
        switch (it.kind) {
            case IK_TEXT:
                SelectObject(dc, g.meas.font(it.font));
                SetTextColor(dc, rgb(it.color));
                ExtTextOutW(dc, x, y + it.asc, 0, NULL, it.text.c_str(), (UINT)it.text.size(), NULL);
                break;
            case IK_FILL:
                fillRectColor(dc, x, y, it.w, it.h, it.color);
                break;
            case IK_ROUND_FRAME: {
                SetDCPenColor(dc, rgb(it.color));
                HGDIOBJ op = SelectObject(dc, GetStockObject(DC_PEN));
                HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
                RoundRect(dc, x, y, x + it.w, y + it.h, 6, 6);
                SelectObject(dc, ob);
                SelectObject(dc, op);
                break;
            }
            case IK_ELLIPSE_FILL:
            case IK_ELLIPSE_FRAME: {
                SetDCPenColor(dc, rgb(it.color));
                SetDCBrushColor(dc, rgb(it.color));
                HGDIOBJ op = SelectObject(dc, GetStockObject(DC_PEN));
                HGDIOBJ ob = SelectObject(dc, it.kind == IK_ELLIPSE_FILL ? GetStockObject(DC_BRUSH) : GetStockObject(NULL_BRUSH));
                Ellipse(dc, x, y, x + it.w + 1, y + it.h + 1);
                SelectObject(dc, ob);
                SelectObject(dc, op);
                break;
            }
            case IK_CHECK:
                drawCheck(dc, x, y, it.w, it.color, it.aux != 0);
                break;
            case IK_IMAGE:
                if (it.aux >= 0 && it.aux < (int)g.layout.images.size())
                    drawImage(dc, g.layout.images[it.aux], x, y, it.w, it.h);
                break;
        }
    }
    SelectObject(dc, oldFont);
    paintScrollbars(dc);
}

// ------------------------------------------------------------ mouse/kbd ----

static void docPoint(int cx, int cy, int &dx, int &dy) {
    dx = cx + g.scrollX;
    dy = cy + g.scrollY;
}

static void updateDragSelection(int cx, int cy) {
    int dx, dy;
    docPoint(cx, cy, dx, dy);
    TextPos p;
    if (hitText(g.layout, g.meas, dx, dy, p)) {
        g.selB = p;
        g.hasSel = true;
        InvalidateRect(g.hwnd, NULL, FALSE);
    }
}

static LRESULT onSetCursor(HWND hwnd, LPARAM lp) {
    if (LOWORD(lp) != HTCLIENT) return DefWindowProcW(hwnd, WM_SETCURSOR, (WPARAM)hwnd, lp);
    POINT pt;
    GetCursorPos(&pt);
    ScreenToClient(hwnd, &pt);
    RECT t, th;
    if ((vBar(t, th) && ptIn(t, pt.x, pt.y)) || (hBar(t, th) && ptIn(t, pt.x, pt.y))) {
        SetCursor(LoadCursorW(NULL, IDC_ARROW));
        return TRUE;
    }
    int dx, dy;
    docPoint(pt.x, pt.y, dx, dy);
    if (linkAt(g.layout, dx, dy) >= 0) {
        SetCursor(LoadCursorW(NULL, IDC_HAND));
        return TRUE;
    }
    // I-beam only when really over text
    for (size_t i = 0; i < g.layout.items.size(); i++) {
        const Item &it = g.layout.items[i];
        if (it.kind == IK_TEXT && it.selectable && dx >= it.x && dx < it.x + it.w && dy >= it.y && dy < it.y + it.h) {
            SetCursor(LoadCursorW(NULL, IDC_IBEAM));
            return TRUE;
        }
    }
    SetCursor(LoadCursorW(NULL, IDC_ARROW));
    return TRUE;
}

static void onLButtonDown(HWND hwnd, int x, int y, bool dbl) {
    SetFocus(hwnd);
    RECT t, th;
    if (vBar(t, th) && ptIn(t, x, y)) {
        SetCapture(hwnd);
        if (ptIn(th, x, y)) {
            g.dragV = true;
            g.dragOffset = y - th.top;
        } else {
            scrollBy(0, y < th.top ? -(g.clientH - 40) : (g.clientH - 40));
        }
        InvalidateRect(hwnd, NULL, FALSE);
        return;
    }
    if (hBar(t, th) && ptIn(t, x, y)) {
        SetCapture(hwnd);
        if (ptIn(th, x, y)) {
            g.dragH = true;
            g.dragOffset = x - th.left;
        } else {
            scrollBy(x < th.left ? -(g.clientW - 40) : (g.clientW - 40), 0);
        }
        InvalidateRect(hwnd, NULL, FALSE);
        return;
    }
    int dx, dy;
    docPoint(x, y, dx, dy);
    TextPos p;
    if (!hitText(g.layout, g.meas, dx, dy, p)) {
        clearSelection();
        InvalidateRect(hwnd, NULL, FALSE);
        return;
    }
    if (dbl) {
        TextPos a, b;
        wordAt(g.layout, p, a, b);
        g.lastDblClick = (DWORD)GetMessageTime();
        g.selA = a;
        g.selB = b;
        g.hasSel = !(a == b);
        InvalidateRect(hwnd, NULL, FALSE);
        return;
    }
    // Windows sends no triple-click message: a click right after a double-click selects the paragraph.
    if (g.lastDblClick != 0 && (DWORD)GetMessageTime() - g.lastDblClick <= GetDoubleClickTime()) {
        TextPos a, b;
        blockAt(g.layout, p, a, b);
        g.lastDblClick = 0;
        g.selA = a;
        g.selB = b;
        g.hasSel = !(a == b);
        InvalidateRect(hwnd, NULL, FALSE);
        return;
    }
    g.selA = g.selB = p;
    g.hasSel = false;
    g.dragging = true;
    g.moved = false;
    g.downPt.x = x;
    g.downPt.y = y;
    SetCapture(hwnd);
    SetTimer(hwnd, TIMER_AUTOSCROLL, 40, NULL);
    InvalidateRect(hwnd, NULL, FALSE);
}

static void onMouseMove(HWND hwnd, int x, int y) {
    if (!g.trackingLeave) {
        TRACKMOUSEEVENT tme;
        tme.cbSize = sizeof tme;
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = hwnd;
        tme.dwHoverTime = 0;
        TrackMouseEvent(&tme);
        g.trackingLeave = true;
    }
    RECT t, th;
    bool hv = vBar(t, th) && ptIn(t, x, y);
    bool hh = hBar(t, th) && ptIn(t, x, y);
    if (hv != g.hoverV || hh != g.hoverH) {
        g.hoverV = hv;
        g.hoverH = hh;
        InvalidateRect(hwnd, NULL, FALSE);
    }
    if (g.dragV && vBar(t, th)) {
        int len = (int)((t.bottom - t.top) - (th.bottom - th.top));
        if (len > 0) {
            int pos = std::min(std::max(0, y - g.dragOffset - (int)t.top), len);
            setScroll(g.scrollX, (int)((double)pos / len * maxScrollY()));
        }
        return;
    }
    if (g.dragH && hBar(t, th)) {
        int len = (int)((t.right - t.left) - (th.right - th.left));
        if (len > 0) {
            int pos = std::min(std::max(0, x - g.dragOffset - (int)t.left), len);
            setScroll((int)((double)pos / len * maxScrollX()), g.scrollY);
        }
        return;
    }
    if (g.dragging) {
        if (std::abs(x - g.downPt.x) > 3 || std::abs(y - g.downPt.y) > 3) g.moved = true;
        if (g.moved) updateDragSelection(x, y);
    }
}

static void onLButtonUp(HWND hwnd, int x, int y) {
    bool wasDrag = g.dragging;
    if (g.dragging || g.dragV || g.dragH) ReleaseCapture();
    g.dragV = g.dragH = false;
    g.dragging = false;
    KillTimer(hwnd, TIMER_AUTOSCROLL);
    if (wasDrag && !g.moved) {
        // plain click: follow a link, otherwise clear the selection
        int dx, dy;
        docPoint(x, y, dx, dy);
        int lk = linkAt(g.layout, dx, dy);
        clearSelection();
        InvalidateRect(hwnd, NULL, FALSE);
        if (lk >= 0) activateLink(g.layout.links[lk]);
    }
    InvalidateRect(hwnd, NULL, FALSE);
}

static void onKeyDown(WPARAM vk) {
    bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    switch (vk) {
        case VK_UP: scrollBy(0, -lineStep()); break;
        case VK_DOWN: scrollBy(0, lineStep()); break;
        case VK_LEFT: scrollBy(-lineStep(), 0); break;
        case VK_RIGHT: scrollBy(lineStep(), 0); break;
        case VK_PRIOR: scrollBy(0, -(g.clientH - lineStep())); break;
        case VK_NEXT: scrollBy(0, g.clientH - lineStep()); break;
        case VK_SPACE: scrollBy(0, (shift ? -1 : 1) * (g.clientH - lineStep())); break;
        case VK_HOME: setScroll(g.scrollX, 0); break;
        case VK_END: setScroll(g.scrollX, maxScrollY()); break;
        case VK_BACK: goBack(); break;
        case VK_ESCAPE:
            if (g.findOpen) closeFind();
            else if (g.fullscreen) toggleFullscreen();
            else if (g.hasSel) {
                clearSelection();
                InvalidateRect(g.hwnd, NULL, FALSE);
            }
            break;
        default:
            break;
    }
}

// ------------------------------------------------------------ window proc --

static void showAbout() {
    MessageBoxW(g.hwnd,
                L"GhostReader  0.3.0\n"
                L"A read-only Markdown viewer, inspired by KDE GhostWriter.\n\n"
                L"\u00A9 2026 Sparc343\n\n"
                L"Markdown parsing: cmark-gfm (BSD-2-Clause).\n\n"
                L"Settings are stored in the registry:\n"
                L"HKEY_CURRENT_USER\\Software\\GhostReader\n"
                L"(Settings > Preferences, or Ctrl+,)",
                L"About GhostReader", MB_OK | MB_ICONINFORMATION);
}

static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            g.hwnd = hwnd;
            g.findEdit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_BORDER | ES_AUTOHSCROLL, 0, 0, 10, 10, hwnd,
                                         (HMENU)(UINT_PTR)IDC_FINDEDIT, g.inst, NULL);
            g_oldEditProc = (WNDPROC)SetWindowLongPtrW(g.findEdit, GWLP_WNDPROC, (LONG_PTR)editProc);
            HFONT ui = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
            SendMessageW(g.findEdit, WM_SETFONT, (WPARAM)ui, TRUE);
            DragAcceptFiles(hwnd, TRUE);
            SetTimer(hwnd, TIMER_WATCH, 1000, NULL);
            return 0;
        }
        case WM_SIZE: {
            if (wp == SIZE_MINIMIZED) return 0;
            g.clientW = LOWORD(lp);
            g.clientH = HIWORD(lp);
            double ratio = g.layout.contentH > 0 ? (double)g.scrollY / g.layout.contentH : 0;
            relayout();
            g.scrollY = (int)(ratio * g.layout.contentH);
            clampScroll();
            layoutFindBox();
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            int W = rc.right, H = rc.bottom;
            if (W > 0 && H > 0) {
                HDC mem = CreateCompatibleDC(hdc);
                HBITMAP bmp = CreateCompatibleBitmap(hdc, W, H);
                HGDIOBJ old = SelectObject(mem, bmp);
                paintAll(mem, W, H);
                BitBlt(hdc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
                SelectObject(mem, old);
                DeleteObject(bmp);
                DeleteDC(mem);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_CTLCOLOREDIT: {
            static HBRUSH br = NULL;
            static Color brColor = 0xFFFFFFFF;
            Color c = g.dark ? g.theme.stripe : 0xffffff;
            if (!br || brColor != c) {
                if (br) DeleteObject(br);
                br = CreateSolidBrush(rgb(c));
                brColor = c;
            }
            SetTextColor((HDC)wp, rgb(g.theme.text));
            SetBkColor((HDC)wp, rgb(c));
            return (LRESULT)br;
        }
        case WM_COMMAND: {
            int id = LOWORD(wp);
            if (id == IDC_FINDEDIT && HIWORD(wp) == EN_CHANGE) {
                wchar_t buf[256];
                GetWindowTextW(g.findEdit, buf, 256);
                g.findText = buf;
                runFind(false);
                return 0;
            }
            switch (id) {
                case IDM_OPEN: openDialog(); break;
                case IDM_RELOAD:
                    if (!g.path.empty()) loadFile(g.path, true, false);
                    break;
                case IDM_CLOSE: closeDocument(); break;
                case IDM_BACK: goBack(); break;
                case IDM_FORWARD: goForward(); break;
                case IDM_EXIT: DestroyWindow(hwnd); break;
                case IDM_COPY: copySelection(); break;
                case IDM_SELALL: selectAll(); break;
                case IDM_FIND: openFind(); break;
                case IDM_FINDNEXT:
                    if (g.matches.empty()) openFind();
                    else gotoMatch(g.curMatch + 1);
                    break;
                case IDM_FINDPREV:
                    if (g.matches.empty()) openFind();
                    else gotoMatch(g.curMatch - 1);
                    break;
                case IDM_ZOOMIN: setZoom(g.zoom * 1.1); break;
                case IDM_ZOOMOUT: setZoom(g.zoom / 1.1); break;
                case IDM_ZOOMRESET: setZoom(1.0); break;
                case IDM_THEME: toggleTheme(); break;
                case IDM_NARROW: toggleNarrow(); break;
                case IDM_SETTINGS: openSettings(); break;
                case IDM_FULLSCREEN: toggleFullscreen(); break;
                case IDM_ABOUT: showAbout(); break;
            }
            return 0;
        }
        case WM_FIND_NEXT: gotoMatch(g.curMatch + 1); return 0;
        case WM_FIND_PREV: gotoMatch(g.curMatch - 1); return 0;
        case WM_FIND_CLOSE: closeFind(); return 0;
        case WM_SETFOCUS:
            return 0;
        case WM_SETCURSOR:
            return onSetCursor(hwnd, lp);
        case WM_LBUTTONDOWN: onLButtonDown(hwnd, (short)LOWORD(lp), (short)HIWORD(lp), false); return 0;
        case WM_LBUTTONDBLCLK: onLButtonDown(hwnd, (short)LOWORD(lp), (short)HIWORD(lp), true); return 0;
        case WM_LBUTTONUP: onLButtonUp(hwnd, (short)LOWORD(lp), (short)HIWORD(lp)); return 0;
        case WM_MOUSEMOVE: onMouseMove(hwnd, (short)LOWORD(lp), (short)HIWORD(lp)); return 0;
        case WM_MOUSELEAVE:
            g.trackingLeave = false;
            if (g.hoverV || g.hoverH) {
                g.hoverV = g.hoverH = false;
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        case WM_XBUTTONUP:
            if (GET_XBUTTON_WPARAM(wp) == XBUTTON1) goBack();
            else if (GET_XBUTTON_WPARAM(wp) == XBUTTON2) goForward();
            return TRUE;
        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            if (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL) {
                setZoom(delta > 0 ? g.zoom * 1.1 : g.zoom / 1.1);
            } else if (GET_KEYSTATE_WPARAM(wp) & MK_SHIFT) {
                scrollBy(-delta * lineStep() * 3 / 120, 0);
            } else {
                scrollBy(0, -delta * lineStep() * 2 / 120);
            }
            return 0;
        }
        case WM_MOUSEHWHEEL:
            scrollBy(GET_WHEEL_DELTA_WPARAM(wp) * lineStep() * 3 / 120, 0);
            return 0;
        case WM_KEYDOWN:
            onKeyDown(wp);
            return 0;
        case WM_TIMER:
            if (wp == TIMER_WATCH) {
                if (!g.path.empty() && !g.dragging) {
                    FILETIME ft;
                    DWORD sz;
                    if (getFileStamp(g.path, ft, sz) && (CompareFileTime(&ft, &g.lastWrite) != 0 || sz != g.lastSize)) {
                        std::string src;
                        if (readWholeFile(g.path, src)) {
                            showText(src, g.path, true);
                        }
                    }
                }
            } else if (wp == TIMER_AUTOSCROLL && g.dragging) {
                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);
                int dy = 0;
                if (pt.y < 0) dy = std::max(-lineStep(), (int)pt.y);
                else if (pt.y > g.clientH) dy = std::min(lineStep(), (int)pt.y - g.clientH);
                if (dy != 0) {
                    scrollBy(0, dy);
                    if (g.moved) updateDragSelection(pt.x, pt.y);
                }
            }
            return 0;
        case WM_DROPFILES: {
            wchar_t buf[MAX_PATH * 4];
            if (DragQueryFileW((HDROP)wp, 0, buf, MAX_PATH * 4)) loadFile(buf, false, true);
            DragFinish((HDROP)wp);
            return 0;
        }
        case WM_DESTROY:
            savePrefs();
            KillTimer(hwnd, TIMER_WATCH);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ------------------------------------------------------------------- main --

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int show) {
    // Crisp text on high-DPI screens (Vista and later).
    typedef BOOL(WINAPI * SetDpiAwareFn)(void);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        SetDpiAwareFn fn = procAddress<SetDpiAwareFn>(user32, "SetProcessDPIAware");
        if (fn) fn();
    }

    g.inst = inst;
    cmark_gfm_core_extensions_ensure_registered();

    // Command line: a file to open, or --reset to forget all saved settings.
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring startFile;
    bool reset = false;
    for (int i = 1; argv && i < argc; i++) {
        if (_wcsicmp(argv[i], L"--reset") == 0) reset = true;
        else if (startFile.empty()) startFile = argv[i];
    }
    if (argv) LocalFree(argv);
    if (reset) {
        LONG r = RegDeleteKeyW(HKEY_CURRENT_USER, kRegKey);
        MessageBoxW(NULL,
                    (r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND)
                        ? L"All GhostReader settings were removed from the registry.\n"
                          L"(HKEY_CURRENT_USER\\Software\\GhostReader)"
                        : L"Could not remove the settings from the registry.",
                    kAppName, MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    HDC sdc = GetDC(NULL);
    g.dpi = GetDeviceCaps(sdc, LOGPIXELSY);
    ReleaseDC(NULL, sdc);
    if (g.dpi < 72) g.dpi = 96;

    loadPrefs();
    g.meas.init();
    applyPrefs(false);

    WNDCLASSEXW wc;
    std::memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = kClassName;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = LoadIconW(inst, MAKEINTRESOURCEW(1));
    if (!RegisterClassExW(&wc)) return 1;

    g.menu = buildMenu();
    g.accel = buildAccelerators();

    RegReader rr;
    int w = (int)rr.dword(L"WinW", 1000), h = (int)rr.dword(L"WinH", 800);
    int x = (int)rr.dword(L"WinX", (DWORD)CW_USEDEFAULT), y = (int)rr.dword(L"WinY", (DWORD)CW_USEDEFAULT);
    bool wasMaximized = rr.dword(L"Maximized", 0) != 0;
    if (w < 300 || h < 200) {
        w = 1000;
        h = 800;
    }
    HWND hwnd = CreateWindowExW(0, kClassName, kAppName, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, w, h, NULL, g.menu, inst, NULL);
    if (!hwnd) return 1;
    updateMenuChecks();
    updateMenuState();

    RECT rc;
    GetClientRect(hwnd, &rc);
    g.clientW = rc.right;
    g.clientH = rc.bottom;

    bool loaded = false;
    if (!startFile.empty()) loaded = loadFile(startFile, false, false);
    if (!loaded) showText(kWelcome, L"", false);
    if (!startFile.empty() && !loaded)
        MessageBoxW(hwnd, (L"Could not read:\n" + startFile).c_str(), kAppName, MB_OK | MB_ICONERROR);

    ShowWindow(hwnd, wasMaximized ? SW_SHOWMAXIMIZED : show);
    UpdateWindow(hwnd);
    SetFocus(hwnd);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        // While the find box has focus, let it have its own keys (Ctrl+C etc.).
        bool inFind = g.findEdit && GetFocus() == g.findEdit;
        if (!inFind && TranslateAcceleratorW(hwnd, g.accel, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    clearImages();
    if (g_gdip.ok) g_gdip.shutdown(g_gdip.token);
    if (g.doc) cmark_node_free(g.doc);
    return (int)m.wParam;
}
