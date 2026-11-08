// layout.h - Markdown -> positioned drawing items (no OS / GUI dependencies).
//
// The layout engine walks a cmark-gfm syntax tree and produces a flat list of
// "items" (text fragments, rectangles, bullets...) with absolute positions in
// document pixels.  A front end only has to (a) measure text, and (b) paint the
// items.  That keeps every Windows-specific line in one small file.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

struct cmark_node;

namespace mdr {

typedef uint32_t Color;  // 0x00RRGGBB

// ---------------------------------------------------------------- fonts ----
enum FontFlags {
    F_BOLD = 1,
    F_ITALIC = 2,
    F_MONO = 4,
    F_UNDERLINE = 8,
    F_STRIKE = 16
};

struct FontSpec {
    int px = 16;     // em height in pixels
    int flags = 0;   // FontFlags
};
inline bool operator<(const FontSpec &a, const FontSpec &b) {
    return a.px != b.px ? a.px < b.px : a.flags < b.flags;
}
inline bool operator==(const FontSpec &a, const FontSpec &b) {
    return a.px == b.px && a.flags == b.flags;
}

struct FontMetrics {
    int ascent = 0;
    int descent = 0;
};

// Implemented by the front end (GDI on Windows).
class Measurer {
public:
    virtual ~Measurer() {}
    virtual int textWidth(const FontSpec &f, const wchar_t *s, int n) = 0;
    virtual FontMetrics metrics(const FontSpec &f) = 0;
    // Natural pixel size of the image at `url` (as written in the document).
    // Return false if it cannot be shown; the alt text is used instead.
    virtual bool imageSize(const std::string &url, int &w, int &h) {
        (void)url; (void)w; (void)h;
        return false;
    }
};

// --------------------------------------------------------------- theme -----
struct Theme {
    Color background, text, heading, link, code, quote;
    Color separator;  // code-block border, block-quote rule, hr
    Color grid;       // table lines
    Color stripe;     // even table rows
    Color selection, selectionText;
    Color scrollThumb, scrollThumbHover;

    static Theme dark();
    static Theme light();
};

struct Options {
    int bodyPx = 16;     // body font size in pixels (already zoomed / dpi-scaled)
    int codePx = 16;     // code font size in pixels
    int pageLeft = 5;    // padding around the page
    int pageRight = 5;
    int pageTop = 13;
    int pageBottom = 24;
    int maxTextWidth = 0;  // 0 = use the full window width
    double imageScale = 1.0;  // dpi scale * zoom applied to images' natural size
};

// --------------------------------------------------------------- items -----
enum ItemKind {
    IK_TEXT = 0,
    IK_FILL,          // solid rectangle
    IK_ROUND_FRAME,   // 1px rounded rectangle outline (code blocks)
    IK_ELLIPSE_FILL,  // disc bullet
    IK_ELLIPSE_FRAME, // circle bullet
    IK_CHECK,         // task-list checkbox (aux = 1 when checked)
    IK_IMAGE          // picture; aux = index into Layout::images
};

// How a text item is separated from the previous one when copying to the
// clipboard.
enum Sep {
    SEP_NONE = 0,  // continues the same line
    SEP_WRAP = 1,  // soft-wrapped line -> single space
    SEP_LINE = 2,  // new line
    SEP_PARA = 3,  // new paragraph/block -> blank line
    SEP_TAB = 4    // next table cell
};

struct Item {
    uint8_t kind = IK_TEXT;
    uint8_t sep = SEP_NONE;
    uint8_t selectable = 0;
    int x = 0, y = 0, w = 0, h = 0;  // bounding box
    int asc = 0;                     // text: baseline = y + asc
    int aux = 0;
    int link = -1;  // index into Layout::links, or -1
    Color color = 0;
    FontSpec font;
    std::wstring text;
};

struct Heading {
    int level = 1;
    std::wstring text;
    int y = 0;
};

struct Layout {
    std::vector<Item> items;
    std::vector<std::string> links;
    std::vector<std::string> images;  // image urls, indexed by Item::aux
    std::vector<Heading> headings;
    std::map<std::string, int> anchors;  // github-style slug -> y
    int contentW = 0;                    // widest thing on the page (incl. padding)
    int contentH = 0;

    void clear();
};

// Build the layout for `doc` at the given viewport width.
void buildLayout(Layout &out, cmark_node *doc, Measurer &m, const Theme &theme,
                 const Options &opt, int viewportW);

// ------------------------------------------------- selection / hit tests ---
struct TextPos {
    int item = -1;  // index into Layout::items
    int off = 0;    // character offset inside the item's text
};
inline bool operator==(const TextPos &a, const TextPos &b) {
    return a.item == b.item && a.off == b.off;
}
inline bool operator<(const TextPos &a, const TextPos &b) {
    return a.item != b.item ? a.item < b.item : a.off < b.off;
}

struct Rect {
    int x, y, w, h;
};

// Nearest text position to a document point; false if the page has no text.
bool hitText(const Layout &L, Measurer &m, int x, int y, TextPos &out);
// Index into Layout::links under the point, or -1.
int linkAt(const Layout &L, int x, int y);
// Select the word around p.
void wordAt(const Layout &L, TextPos p, TextPos &a, TextPos &b);
// Select the whole text item line... (used for triple click): the paragraph.
void blockAt(const Layout &L, TextPos p, TextPos &a, TextPos &b);
// Text between two positions (order does not matter).
std::wstring selectedText(const Layout &L, TextPos a, TextPos b);
// Highlight rectangles between two positions.
void selectionRects(const Layout &L, Measurer &m, TextPos a, TextPos b,
                    std::vector<Rect> &out);
// Last text position on the page (for Ctrl+A).
bool endOfText(const Layout &L, TextPos &out);
bool startOfText(const Layout &L, TextPos &out);

// -------------------------------------------------------------- search -----
// Find all case-insensitive matches of `needle` (items are searched one at a
// time, so a match never spans two styled fragments).
struct Match {
    int item;
    int off;
    int len;
};
void findAll(const Layout &L, const std::wstring &needle, std::vector<Match> &out);

// ----------------------------------------------------------- utilities -----
std::wstring utf8ToWide(const char *s, size_t n);
std::string wideToUtf8(const std::wstring &w);

}  // namespace mdr
