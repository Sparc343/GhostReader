// layout.cpp - see layout.h
#include "layout.h"

#include <algorithm>
#include <cstring>
#include <cwctype>

extern "C" {
#include "cmark-gfm.h"
#include "cmark-gfm-core-extensions.h"
}

namespace mdr {

// ============================================================ utilities ====

std::wstring utf8ToWide(const char *s, size_t n) {
    std::wstring out;
    out.reserve(n);
    size_t i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        uint32_t cp = 0xFFFD;
        int extra = 0;
        if (c < 0x80) {
            cp = c;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1F;
            extra = 1;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0F;
            extra = 2;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07;
            extra = 3;
        }
        i++;
        for (int k = 0; k < extra; k++) {
            if (i < n && ((unsigned char)s[i] & 0xC0) == 0x80) {
                cp = (cp << 6) | ((unsigned char)s[i] & 0x3F);
                i++;
            } else {
                cp = 0xFFFD;
                break;
            }
        }
        if (sizeof(wchar_t) == 2 && cp > 0xFFFF) {
            cp -= 0x10000;
            out.push_back((wchar_t)(0xD800 + (cp >> 10)));
            out.push_back((wchar_t)(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back((wchar_t)cp);
        }
    }
    return out;
}

std::string wideToUtf8(const std::wstring &w) {
    std::string out;
    for (size_t i = 0; i < w.size(); i++) {
        uint32_t cp = (uint32_t)w[i];
        if (sizeof(wchar_t) == 2 && cp >= 0xD800 && cp < 0xDC00 && i + 1 < w.size()) {
            uint32_t lo = (uint32_t)w[i + 1];
            if (lo >= 0xDC00 && lo < 0xE000) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                i++;
            }
        }
        if (cp < 0x80) {
            out.push_back((char)cp);
        } else if (cp < 0x800) {
            out.push_back((char)(0xC0 | (cp >> 6)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back((char)(0xE0 | (cp >> 12)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        } else {
            out.push_back((char)(0xF0 | (cp >> 18)));
            out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

// ================================================================ themes ====

Theme Theme::dark() {
    Theme t;
    // Colors sampled from the KDE GhostWriter dark theme preview pane.
    t.background = 0x151719;
    t.text = 0xbdc3c7;
    t.heading = 0xbdc3c7;
    t.link = 0x4aa3df;
    t.code = 0xd2b48c;
    t.quote = 0x9aa5ab;
    t.separator = 0x3b4144;
    t.grid = 0x2c2f31;
    t.stripe = 0x202325;
    t.selection = 0x2a4a63;
    t.selectionText = 0xffffff;
    t.scrollThumb = 0x3a3f42;
    t.scrollThumbHover = 0x4aa3df;
    return t;
}

Theme Theme::light() {
    Theme t;
    t.background = 0xfafafa;
    t.text = 0x24292e;
    t.heading = 0x24292e;
    t.link = 0x0366d6;
    t.code = 0x8a4b08;
    t.quote = 0x6a737d;
    t.separator = 0xd0d7de;
    t.grid = 0xd0d7de;
    t.stripe = 0xf0f1f3;
    t.selection = 0xb3d4fc;
    t.selectionText = 0x000000;
    t.scrollThumb = 0xc4c8cc;
    t.scrollThumbHover = 0x0366d6;
    return t;
}

void Layout::clear() {
    items.clear();
    links.clear();
    images.clear();
    headings.clear();
    anchors.clear();
    contentW = contentH = 0;
}

// ================================================================ builder ===

namespace {

struct Atom {
    enum Kind { WORD, SPACE, BREAK, CHECK, IMAGE };
    Kind k = WORD;
    std::wstring s;
    FontSpec f;
    Color color = 0;
    int link = -1;
    int w = 0;
    int asc = 0, desc = 0, lh = 0;
    int aux = 0;
};

struct Line {
    std::vector<Atom> a;
    int w = 0;
    bool empty() const { return a.empty(); }
};

struct Ctx {
    int x = 0, w = 0;     // content box
    int px = 16;          // font size for this block
    double lh = 1.5;      // line-height multiple; <= 0 means "normal"
    Color color = 0;
    int flags = 0;        // base font flags
    int align = 0;        // 0 left, 1 center, 2 right
    bool tight = false;   // paragraph inside a tight list item
    int listDepth = 0;
    int taskState = -1;   // -1 none, 0 unchecked, 1 checked (consumed once)
    int *firstTop = nullptr;
    int *firstBase = nullptr;
};

struct St {  // inline style state
    int flags = 0;
    Color color = 0;
    int link = -1;
    bool upper = false;
};

static bool isSpaceCh(wchar_t c) {
    return c == L' ' || c == L'\t' || c == L'\n' || c == L'\r' || c == L'\f';
}

class Builder {
public:
    Builder(Layout &l, Measurer &m, const Theme &t, const Options &o)
        : L(l), M(m), T(t), O(o) {}

    void run(cmark_node *doc, int viewportW);

private:
    Layout &L;
    Measurer &M;
    const Theme &T;
    Options O;
    int y = 0;
    int pend = 0;  // pending bottom margin of the previous block
    int sepNext = SEP_PARA;
    std::map<std::string, int> slugCount;

    // ---- helpers
    static int em(int px, double k) { return (int)(px * k + 0.5); }
    // Fixed CSS-style pixel constants scale with the body font size (16px = 1x).
    int u(int v) const { return std::max(1, (O.bodyPx * v + 8) / 16); }
    int monoPx(int px) const {
        return O.bodyPx > 0 ? std::max(6, px * O.codePx / O.bodyPx) : px;
    }
    int tw(const FontSpec &f, const std::wstring &s) {
        return s.empty() ? 0 : M.textWidth(f, s.c_str(), (int)s.size());
    }
    void noteContentRight(int r) {
        r += O.pageRight;
        if (r > L.contentW) L.contentW = r;
    }
    void placeTop(Ctx &c, int margin) {
        y += std::max(pend, margin);
        pend = 0;
        if (c.firstTop && *c.firstTop < 0) *c.firstTop = y;
    }
    int addLink(const char *url) {
        std::string u = url ? url : "";
        L.links.push_back(u);
        return (int)L.links.size() - 1;
    }
    void fill(int x, int yy, int w, int h, Color col) {
        Item it;
        it.kind = IK_FILL;
        it.x = x; it.y = yy; it.w = w; it.h = h; it.color = col;
        L.items.push_back(it);
    }

    // ---- blocks
    void blocks(cmark_node *parent, Ctx &c);
    void block(cmark_node *n, Ctx &c);
    void paragraph(cmark_node *n, Ctx &c);
    void heading(cmark_node *n, Ctx &c);
    void blockQuote(cmark_node *n, Ctx &c);
    void list(cmark_node *n, Ctx &c);
    void codeBlock(cmark_node *n, Ctx &c);
    void htmlBlock(cmark_node *n, Ctx &c);
    void thematicBreak(Ctx &c);
    void table(cmark_node *n, Ctx &c);

    // ---- inlines
    void collect(cmark_node *parent, const St &st, const Ctx &c, std::vector<Atom> &out);
    void addText(const char *utf8, const St &st, const Ctx &c, std::vector<Atom> &out);
    Atom makeAtom(Atom::Kind k, const std::wstring &s, const St &st, const Ctx &c);
    void inlines(cmark_node *parent, Ctx &c, bool upper, bool bold);
    void emitLine(Line &ln, const Ctx &c, bool hardEnd);
    int fitChars(const Atom &a, int avail);
    void measureAtoms(const std::vector<Atom> &atoms, int &minW, int &maxW);

    static std::wstring plainText(cmark_node *n);
    std::string slugFor(const std::wstring &text);
};

// ------------------------------------------------------------------ run ----

void Builder::run(cmark_node *doc, int viewportW) {
    L.clear();
    Ctx c;
    c.px = O.bodyPx;
    c.lh = 1.5;
    c.color = T.text;
    c.x = O.pageLeft;
    int w = viewportW - O.pageLeft - O.pageRight;
    if (O.maxTextWidth > 0 && w > O.maxTextWidth) {
        c.x += (w - O.maxTextWidth) / 2;  // center a narrow reading column
        w = O.maxTextWidth;
    }
    c.w = std::max(60, w);
    y = O.pageTop;
    pend = 0;
    sepNext = SEP_PARA;
    L.contentW = viewportW;
    slugCount.clear();

    blocks(doc, c);

    L.contentH = y + pend + O.pageBottom;
}

void Builder::blocks(cmark_node *parent, Ctx &c) {
    for (cmark_node *ch = cmark_node_first_child(parent); ch; ch = cmark_node_next(ch)) {
        block(ch, c);
    }
}

void Builder::block(cmark_node *n, Ctx &c) {
    switch (cmark_node_get_type(n)) {
        case CMARK_NODE_PARAGRAPH: paragraph(n, c); break;
        case CMARK_NODE_HEADING: heading(n, c); break;
        case CMARK_NODE_BLOCK_QUOTE: blockQuote(n, c); break;
        case CMARK_NODE_LIST: list(n, c); break;
        case CMARK_NODE_CODE_BLOCK: codeBlock(n, c); break;
        case CMARK_NODE_HTML_BLOCK: htmlBlock(n, c); break;
        case CMARK_NODE_THEMATIC_BREAK: thematicBreak(c); break;
        default: {
            const char *ts = cmark_node_get_type_string(n);
            if (ts && std::strcmp(ts, "table") == 0) {
                table(n, c);
            } else {
                blocks(n, c);  // unknown container: just descend
            }
        }
    }
}

// ------------------------------------------------------------ paragraph ----

void Builder::paragraph(cmark_node *n, Ctx &c) {
    int margin = c.tight ? 0 : em(c.px, 1.0);
    placeTop(c, margin);
    sepNext = SEP_PARA;
    inlines(n, c, false, false);
    pend = margin;
}

// -------------------------------------------------------------- heading ----

std::wstring Builder::plainText(cmark_node *n) {
    std::wstring out;
    for (cmark_node *ch = cmark_node_first_child(n); ch; ch = cmark_node_next(ch)) {
        cmark_node_type t = cmark_node_get_type(ch);
        if (t == CMARK_NODE_TEXT || t == CMARK_NODE_CODE) {
            const char *lit = cmark_node_get_literal(ch);
            if (lit) out += utf8ToWide(lit, std::strlen(lit));
        } else if (t == CMARK_NODE_SOFTBREAK || t == CMARK_NODE_LINEBREAK) {
            out += L' ';
        } else {
            out += plainText(ch);
        }
    }
    return out;
}

std::string Builder::slugFor(const std::wstring &text) {
    std::string u = wideToUtf8(text);
    std::string s;
    for (size_t i = 0; i < u.size(); i++) {
        unsigned char ch = (unsigned char)u[i];
        if (ch >= 'A' && ch <= 'Z') s.push_back((char)(ch + 32));
        else if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch >= 0x80)
            s.push_back((char)ch);
        else if (ch == ' ')
            s.push_back('-');
    }
    int &count = slugCount[s];
    std::string result = s;
    if (count > 0) result += "-" + std::to_string(count);
    count++;
    return result;
}

void Builder::heading(cmark_node *n, Ctx &c) {
    static const double scale[7] = {1, 2.0, 1.5, 1.17, 1.0, 0.83, 0.67};
    static const double marg[7] = {1, 0.67, 0.83, 1.0, 1.33, 1.67, 2.33};
    int lvl = std::min(6, std::max(1, cmark_node_get_heading_level(n)));
    Ctx h = c;
    h.px = std::max(8, em(c.px, scale[lvl]));
    h.color = T.heading;
    h.flags |= F_BOLD;
    int margin = em(h.px, marg[lvl]);
    placeTop(c, margin);
    sepNext = SEP_PARA;

    Heading hd;
    hd.level = lvl;
    hd.text = plainText(n);
    hd.y = y;
    L.headings.push_back(hd);
    std::string slug = slugFor(hd.text);
    if (!L.anchors.count(slug)) L.anchors[slug] = y;

    inlines(n, h, lvl == 1, true);
    pend = margin;
}

// ------------------------------------------------------------ blockquote ---

void Builder::blockQuote(cmark_node *n, Ctx &c) {
    int m = em(c.px, 1.0);
    pend = std::max(pend, m);
    Ctx q = c;
    q.x = c.x + u(40) + u(4) + u(15);
    q.w = std::max(40, c.w - (u(40) + u(4) + u(15)) - (u(40) + u(15)));
    q.color = T.quote;
    int top = -1;
    q.firstTop = &top;
    blocks(n, q);
    int bottom = y;
    if (top >= 0 && bottom > top) fill(c.x + u(40), top, u(4), bottom - top, T.separator);
    if (top >= 0 && c.firstTop && *c.firstTop < 0) *c.firstTop = top;
    pend = std::max(pend, m);
}

// ------------------------------------------------------------------ list ---

void Builder::list(cmark_node *n, Ctx &c) {
    bool ordered = cmark_node_get_list_type(n) == CMARK_ORDERED_LIST;
    bool tight = cmark_node_get_list_tight(n) != 0;
    int number = cmark_node_get_list_start(n);
    if (number < 1 && ordered) number = 1;
    int margin = c.listDepth == 0 ? em(c.px, 1.0) : 0;
    pend = std::max(pend, margin);

    Ctx lc = c;
    lc.x = c.x + u(40);
    lc.w = std::max(40, c.w - u(40));
    lc.lh = 2.0;
    lc.listDepth = c.listDepth + 1;
    lc.tight = tight;

    FontSpec bodyFont;
    bodyFont.px = c.px;

    for (cmark_node *item = cmark_node_first_child(n); item; item = cmark_node_next(item)) {
        Ctx ic = lc;
        int base = -1;
        ic.firstBase = &base;
        const char *ts = cmark_node_get_type_string(item);
        if (ts && std::strcmp(ts, "tasklist") == 0)
            ic.taskState = cmark_gfm_extensions_get_tasklist_item_checked(item) ? 1 : 0;
        int topY = y;
        int topPend = pend;
        blocks(item, ic);

        FontMetrics fm = M.metrics(bodyFont);
        if (base < 0) base = topY + std::max(pend, topPend) + fm.ascent;  // item with no text of its own

        // ---- marker
        if (ordered) {
            std::wstring s = std::to_wstring(number) + L".";
            Item it;
            it.kind = IK_TEXT;
            it.font = bodyFont;
            it.text = s;
            it.color = c.color;
            it.asc = fm.ascent;
            it.h = fm.ascent + fm.descent;
            it.w = tw(bodyFont, s);
            int spaceW = tw(bodyFont, L" ");
            it.x = lc.x - spaceW - it.w;
            it.y = base - fm.ascent;
            L.items.push_back(it);
        } else {
            int d = std::max(4, em(c.px, 0.31));
            Item it;
            it.kind = c.listDepth == 0 ? IK_ELLIPSE_FILL : (c.listDepth == 1 ? IK_ELLIPSE_FRAME : IK_FILL);
            it.w = it.h = d;
            it.x = lc.x - em(c.px, 0.75) - d / 2;
            it.y = base - em(c.px, 0.30) - d / 2;
            it.color = c.color;
            L.items.push_back(it);
        }
        number++;
    }
    pend = std::max(pend, margin);
}

// ----------------------------------------------------------- code block ----

void Builder::codeBlock(cmark_node *n, Ctx &c) {
    const char *lit = cmark_node_get_literal(n);
    std::string text = lit ? lit : "";
    if (!text.empty() && text.back() == '\n') text.pop_back();

    FontSpec f;
    f.px = monoPx(c.px);
    f.flags = F_MONO;
    FontMetrics fm = M.metrics(f);
    int lineH = fm.ascent + fm.descent;
    int margin = em(f.px, 1.0);
    placeTop(c, margin);

    // split into lines, expanding tabs
    std::vector<std::wstring> lines;
    {
        size_t start = 0;
        while (true) {
            size_t e = text.find('\n', start);
            std::string ln = text.substr(start, e == std::string::npos ? std::string::npos : e - start);
            if (!ln.empty() && ln.back() == '\r') ln.pop_back();
            std::wstring w = utf8ToWide(ln.data(), ln.size());
            std::wstring x;
            for (size_t i = 0; i < w.size(); i++) {
                if (w[i] == L'\t') {
                    size_t pad = 4 - (x.size() % 4);
                    x.append(pad, L' ');
                } else {
                    x.push_back(w[i]);
                }
            }
            lines.push_back(x);
            if (e == std::string::npos) break;
            start = e + 1;
        }
    }

    int boxTop = y;
    int pad = u(10);
    int textX = c.x + 1 + pad;
    int textY = boxTop + 1 + pad;
    int maxW = 0;
    std::vector<Item> tmp;
    for (size_t i = 0; i < lines.size(); i++) {
        if (!lines[i].empty()) {
            Item it;
            it.kind = IK_TEXT;
            it.selectable = 1;
            it.sep = i == 0 ? SEP_PARA : SEP_LINE;
            it.font = f;
            it.color = T.code;
            it.text = lines[i];
            it.x = textX;
            it.y = textY + (int)i * lineH;
            it.asc = fm.ascent;
            it.h = lineH;
            it.w = tw(f, lines[i]);
            maxW = std::max(maxW, it.w);
            tmp.push_back(it);
        } else if (i == 0) {
            // keep separator semantics sane for an empty first line
        }
    }
    int boxH = 2 + 2 * pad + (int)lines.size() * lineH;
    int boxW = std::max(c.w, maxW + 2 + 2 * pad);

    Item frame;
    frame.kind = IK_ROUND_FRAME;
    frame.x = c.x; frame.y = boxTop; frame.w = boxW; frame.h = boxH;
    frame.color = T.separator;
    L.items.push_back(frame);
    for (size_t i = 0; i < tmp.size(); i++) L.items.push_back(tmp[i]);
    noteContentRight(c.x + boxW);

    y = boxTop + boxH;
    pend = margin;
}

// ------------------------------------------------------------ html block ---

void Builder::htmlBlock(cmark_node *n, Ctx &c) {
    const char *lit = cmark_node_get_literal(n);
    if (!lit) return;
    std::string s = lit;
    // Drop comments entirely.
    for (size_t p; (p = s.find("<!--")) != std::string::npos;) {
        size_t e = s.find("-->", p);
        s.erase(p, e == std::string::npos ? std::string::npos : e + 3 - p);
    }
    // Strip tags; show whatever text is left as a plain paragraph.
    std::string out;
    bool inTag = false;
    for (char ch : s) {
        if (ch == '<') inTag = true;
        else if (ch == '>' && inTag) { inTag = false; out.push_back(' '); }
        else if (!inTag) out.push_back(ch);
    }
    std::wstring w = utf8ToWide(out.data(), out.size());
    bool any = false;
    for (wchar_t ch : w) if (!isSpaceCh(ch)) { any = true; break; }
    if (!any) return;

    int margin = c.tight ? 0 : em(c.px, 1.0);
    placeTop(c, margin);
    sepNext = SEP_PARA;
    // Re-use the paragraph machinery through a temporary node.
    cmark_node *tmp = cmark_node_new(CMARK_NODE_PARAGRAPH);
    cmark_node *t = cmark_node_new(CMARK_NODE_TEXT);
    cmark_node_set_literal(t, out.c_str());
    cmark_node_append_child(tmp, t);
    inlines(tmp, c, false, false);
    cmark_node_free(tmp);
    pend = margin;
}

void Builder::thematicBreak(Ctx &c) {
    int margin = em(c.px, 0.5);
    placeTop(c, margin);
    fill(c.x, y, c.w, 1, T.separator);
    y += 1;
    pend = margin;
}

// ----------------------------------------------------------------- table ---

void Builder::measureAtoms(const std::vector<Atom> &atoms, int &minW, int &maxW) {
    int cur = 0, chunk = 0;
    minW = maxW = 0;
    for (const Atom &a : atoms) {
        if (a.k == Atom::BREAK) {
            maxW = std::max(maxW, cur);
            minW = std::max(minW, chunk);
            cur = chunk = 0;
        } else if (a.k == Atom::SPACE) {
            cur += a.w;
            minW = std::max(minW, chunk);
            chunk = 0;
        } else {
            cur += a.w;
            chunk += a.w;
        }
    }
    maxW = std::max(maxW, cur);
    minW = std::max(minW, chunk);
}

void Builder::table(cmark_node *t, Ctx &c) {
    int n = (int)cmark_gfm_extensions_get_table_columns(t);
    if (n <= 0) return;
    uint8_t *aligns = cmark_gfm_extensions_get_table_alignments(t);

    std::vector<cmark_node *> rows;
    for (cmark_node *r = cmark_node_first_child(t); r; r = cmark_node_next(r)) rows.push_back(r);
    if (rows.empty()) return;

    Ctx cell = c;
    cell.lh = 0;  // "line-height: normal" inside tables
    cell.tight = false;
    cell.firstBase = nullptr;
    cell.firstTop = nullptr;
    cell.taskState = -1;

    // ---- column widths
    std::vector<int> mn(n, 0), mx(n, 0);
    for (cmark_node *r : rows) {
        bool header = cmark_gfm_extensions_get_table_row_is_header(r) != 0;
        int k = 0;
        for (cmark_node *cn = cmark_node_first_child(r); cn && k < n; cn = cmark_node_next(cn), k++) {
            std::vector<Atom> atoms;
            St st;
            st.flags = cell.flags | (header ? F_BOLD : 0);
            st.color = cell.color;
            collect(cn, st, cell, atoms);
            int a, b;
            measureAtoms(atoms, a, b);
            mn[k] = std::max(mn[k], a);
            mx[k] = std::max(mx[k], b);
        }
    }
    long sumMn = 0, sumMx = 0;
    for (int k = 0; k < n; k++) { sumMn += mn[k]; sumMx += mx[k]; }
    const int padX = u(5), padY = u(2);
    long avail = (long)c.w - (n + 1) - 2L * padX * n;
    std::vector<int> cw(n);
    if (sumMx <= avail) {
        long extra = avail - sumMx, used = 0;
        for (int k = 0; k < n; k++) {
            cw[k] = mx[k] + (sumMx > 0 ? (int)(extra * mx[k] / sumMx) : (int)(extra / n));
            used += cw[k];
        }
        cw[n - 1] += (int)(avail - used);
    } else if (sumMn <= avail) {
        long d = avail - sumMn, range = sumMx - sumMn, used = 0;
        for (int k = 0; k < n; k++) {
            cw[k] = mn[k] + (range > 0 ? (int)(d * (mx[k] - mn[k]) / range) : 0);
            used += cw[k];
        }
        cw[n - 1] += (int)(avail - used);
    } else {
        for (int k = 0; k < n; k++) cw[k] = std::max(mn[k], 8);
    }
    // x positions of the vertical grid lines
    std::vector<int> lx(n + 1);
    lx[0] = c.x;
    for (int k = 0; k < n; k++) lx[k + 1] = lx[k] + 1 + cw[k] + 2 * padX;
    int tableW = lx[n] - c.x + 1;

    placeTop(c, 0);
    int tableTop = y;
    FontSpec base;
    base.px = c.px;
    FontMetrics bm = M.metrics(base);
    int minLine = bm.ascent + bm.descent;

    std::vector<int> rowY;  // y of the horizontal line above each row
    int bodyIndex = 0;
    int rowTop = tableTop;
    for (size_t ri = 0; ri < rows.size(); ri++) {
        cmark_node *r = rows[ri];
        bool header = cmark_gfm_extensions_get_table_row_is_header(r) != 0;
        if (!header) bodyIndex++;
        rowY.push_back(rowTop);

        size_t stripeIdx = L.items.size();  // insert the stripe before the text
        int maxEnd = rowTop + 1 + padY + minLine;
        int k = 0;
        for (cmark_node *cn = cmark_node_first_child(r); cn && k < n; cn = cmark_node_next(cn), k++) {
            Ctx cc = cell;
            cc.x = lx[k] + 1 + padX;
            cc.w = cw[k];
            cc.flags = cell.flags;
            cc.align = aligns ? (aligns[k] == 'c' ? 1 : (aligns[k] == 'r' ? 2 : 0)) : 0;
            y = rowTop + 1 + padY;
            sepNext = (k == 0) ? (ri == 0 ? SEP_PARA : SEP_LINE) : SEP_TAB;
            inlines(cn, cc, false, header);
            maxEnd = std::max(maxEnd, y);
        }
        int rowH = (maxEnd - (rowTop + 1 + padY)) + 2 * padY;
        if (!header && bodyIndex % 2 == 0) {
            Item st;
            st.kind = IK_FILL;
            st.x = c.x; st.y = rowTop + 1; st.w = tableW; st.h = rowH;
            st.color = T.stripe;
            L.items.insert(L.items.begin() + stripeIdx, st);
        }
        rowTop = rowTop + 1 + rowH;
    }
    int tableBottom = rowTop;  // y of the last horizontal line
    // grid lines
    for (int ry : rowY) fill(c.x, ry, tableW, 1, T.grid);
    fill(c.x, tableBottom, tableW, 1, T.grid);
    for (int k = 0; k <= n; k++) fill(lx[k], tableTop, 1, tableBottom - tableTop + 1, T.grid);
    noteContentRight(c.x + tableW);

    y = tableBottom + 1;
    pend = 0;
}

// ---------------------------------------------------------------- inlines --

Atom Builder::makeAtom(Atom::Kind k, const std::wstring &s, const St &st, const Ctx &c) {
    Atom a;
    a.k = k;
    a.s = s;
    a.f.flags = st.flags;
    a.f.px = (st.flags & F_MONO) ? monoPx(c.px) : c.px;
    a.color = st.color;
    a.link = st.link;
    FontMetrics fm = M.metrics(a.f);
    a.asc = fm.ascent;
    a.desc = fm.descent;
    a.lh = c.lh > 0 ? em(a.f.px, c.lh) : fm.ascent + fm.descent;
    a.w = (k == Atom::WORD || k == Atom::SPACE) ? tw(a.f, s) : 0;
    return a;
}

void Builder::addText(const char *utf8, const St &st, const Ctx &c, std::vector<Atom> &out) {
    if (!utf8) return;
    std::wstring w = utf8ToWide(utf8, std::strlen(utf8));
    if (st.upper) {
        for (size_t i = 0; i < w.size(); i++) w[i] = (wchar_t)std::towupper((wint_t)w[i]);
    }
    size_t i = 0;
    while (i < w.size()) {
        if (isSpaceCh(w[i])) {
            while (i < w.size() && isSpaceCh(w[i])) i++;
            if (out.empty() || out.back().k != Atom::SPACE)
                out.push_back(makeAtom(Atom::SPACE, L" ", st, c));
        } else {
            size_t j = i;
            while (j < w.size() && !isSpaceCh(w[j])) j++;
            out.push_back(makeAtom(Atom::WORD, w.substr(i, j - i), st, c));
            i = j;
        }
    }
}

void Builder::collect(cmark_node *parent, const St &st, const Ctx &c, std::vector<Atom> &out) {
    for (cmark_node *ch = cmark_node_first_child(parent); ch; ch = cmark_node_next(ch)) {
        cmark_node_type t = cmark_node_get_type(ch);
        switch (t) {
            case CMARK_NODE_TEXT:
                addText(cmark_node_get_literal(ch), st, c, out);
                break;
            case CMARK_NODE_SOFTBREAK:
                if (out.empty() || out.back().k != Atom::SPACE)
                    out.push_back(makeAtom(Atom::SPACE, L" ", st, c));
                break;
            case CMARK_NODE_LINEBREAK:
                out.push_back(makeAtom(Atom::BREAK, L"", st, c));
                break;
            case CMARK_NODE_CODE: {
                St s2 = st;
                s2.flags |= F_MONO;
                s2.color = T.code;
                addText(cmark_node_get_literal(ch), s2, c, out);
                break;
            }
            case CMARK_NODE_HTML_INLINE: {
                const char *lit = cmark_node_get_literal(ch);
                if (lit && (std::strncmp(lit, "<br", 3) == 0 || std::strncmp(lit, "<BR", 3) == 0))
                    out.push_back(makeAtom(Atom::BREAK, L"", st, c));
                break;  // other inline tags are dropped, their text content is kept
            }
            case CMARK_NODE_EMPH: {
                St s2 = st;
                s2.flags |= F_ITALIC;
                collect(ch, s2, c, out);
                break;
            }
            case CMARK_NODE_STRONG: {
                St s2 = st;
                s2.flags |= F_BOLD;
                collect(ch, s2, c, out);
                break;
            }
            case CMARK_NODE_LINK: {
                St s2 = st;
                s2.flags |= F_UNDERLINE;
                s2.color = T.link;
                s2.link = addLink(cmark_node_get_url(ch));
                collect(ch, s2, c, out);
                break;
            }
            case CMARK_NODE_IMAGE: {
                const char *url = cmark_node_get_url(ch);
                int nw = 0, nh = 0;
                if (url && *url && M.imageSize(url, nw, nh) && nw > 0 && nh > 0) {
                    // A real picture: natural size times zoom, shrunk to fit the column.
                    int w = std::max(1, (int)(nw * O.imageScale + 0.5));
                    int h = std::max(1, (int)(nh * O.imageScale + 0.5));
                    int maxW = std::max(20, c.w);
                    if (w > maxW) {
                        h = std::max(1, (int)((long long)h * maxW / w));
                        w = maxW;
                    }
                    Atom a;
                    a.k = Atom::IMAGE;
                    a.w = w;
                    a.asc = h;  // sits on the baseline
                    a.desc = 0;
                    a.lh = h;
                    a.link = st.link;
                    a.color = st.color;
                    L.images.push_back(url);
                    a.aux = (int)L.images.size() - 1;
                    out.push_back(a);
                    break;
                }
                // Not loadable (missing file, remote url...): show the alt text, clickable.
                St s2 = st;
                s2.flags |= F_ITALIC | F_UNDERLINE;
                s2.color = T.link;
                s2.link = addLink(url);
                std::vector<Atom> alt;
                collect(ch, s2, c, alt);
                std::vector<Atom> tmp;
                addText("[image: ", s2, c, tmp);
                out.insert(out.end(), tmp.begin(), tmp.end());
                out.insert(out.end(), alt.begin(), alt.end());
                tmp.clear();
                addText("]", s2, c, tmp);
                out.insert(out.end(), tmp.begin(), tmp.end());
                break;
            }
            case CMARK_NODE_FOOTNOTE_REFERENCE:
                break;
            default: {
                const char *ts = cmark_node_get_type_string(ch);
                if (ts && std::strcmp(ts, "strikethrough") == 0) {
                    St s2 = st;
                    s2.flags |= F_STRIKE;
                    collect(ch, s2, c, out);
                } else {
                    collect(ch, st, c, out);
                }
            }
        }
    }
}

int Builder::fitChars(const Atom &a, int avail) {
    int lo = 1, hi = (int)a.s.size();  // largest n in [1,hi] with width(n) <= avail (min 1)
    int best = 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (M.textWidth(a.f, a.s.c_str(), mid) <= avail) {
            best = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    if (sizeof(wchar_t) == 2 && best > 1 && best < (int)a.s.size()) {
        wchar_t ch = a.s[best - 1];
        if (ch >= 0xD800 && ch < 0xDC00) best--;  // do not split a surrogate pair
    }
    return best;
}

void Builder::emitLine(Line &ln, const Ctx &c, bool hardEnd) {
    while (!ln.a.empty() && ln.a.back().k == Atom::SPACE) {
        ln.w -= ln.a.back().w;
        ln.a.pop_back();
    }
    // The block's own "strut" always participates in the line box.
    FontSpec sf;
    sf.px = c.px;
    sf.flags = c.flags & (F_BOLD | F_ITALIC);
    FontMetrics sm = M.metrics(sf);
    int slh = c.lh > 0 ? em(c.px, c.lh) : sm.ascent + sm.descent;
    int above = (slh - (sm.ascent + sm.descent)) / 2 + sm.ascent;
    int below = slh - above;
    for (const Atom &a : ln.a) {
        int ab = (a.lh - (a.asc + a.desc)) / 2 + a.asc;
        above = std::max(above, ab);
        below = std::max(below, a.lh - ab);
    }
    int lineH = above + below;
    int baseline = y + above;
    if (c.firstBase && *c.firstBase < 0) *c.firstBase = baseline;

    int x = c.x;
    if (c.align == 1) x += (c.w - ln.w) / 2;
    else if (c.align == 2) x += c.w - ln.w;
    if (x < c.x) x = c.x;

    size_t i = 0;
    bool first = true;
    while (i < ln.a.size()) {
        const Atom &a = ln.a[i];
        if (a.k == Atom::CHECK) {
            Item it;
            it.kind = IK_CHECK;
            it.aux = a.aux;
            it.w = it.h = a.asc;  // checkbox is square, sits on the baseline
            it.x = x;
            it.y = baseline - a.asc;
            it.color = a.color;
            L.items.push_back(it);
            x += a.w;
            i++;
            continue;
        }
        if (a.k == Atom::IMAGE) {
            Item im;
            im.kind = IK_IMAGE;
            im.aux = a.aux;
            im.link = a.link;
            im.w = a.w;
            im.h = a.asc;
            im.x = x;
            im.y = baseline - a.asc;
            L.items.push_back(im);
            x += a.w;
            i++;
            continue;
        }
        Item it;
        it.kind = IK_TEXT;
        it.selectable = 1;
        it.font = a.f;
        it.color = a.color;
        it.link = a.link;
        it.asc = a.asc;
        it.h = a.asc + a.desc;
        it.x = x;
        it.y = baseline - a.asc;
        it.sep = first ? (uint8_t)sepNext : (uint8_t)SEP_NONE;
        first = false;
        it.text = a.s;
        int w = a.w;
        size_t j = i + 1;
        // merge neighbours that share the exact same look
        while (j < ln.a.size() && (ln.a[j].k == Atom::WORD || ln.a[j].k == Atom::SPACE) && ln.a[j].f == a.f &&
               ln.a[j].color == a.color && ln.a[j].link == a.link) {
            it.text += ln.a[j].s;
            w += ln.a[j].w;
            j++;
        }
        it.w = w;
        // different atoms of the same style can still differ in metrics only by font, which is equal here
        L.items.push_back(it);
        x += w;
        i = j;
    }
    noteContentRight(c.x + std::max(ln.w, 0));
    y += lineH;
    sepNext = hardEnd ? SEP_LINE : SEP_WRAP;
    ln.a.clear();
    ln.w = 0;
}

void Builder::inlines(cmark_node *parent, Ctx &c, bool upper, bool bold) {
    std::vector<Atom> atoms;
    St st;
    st.flags = c.flags | (bold ? F_BOLD : 0);
    st.color = c.color;
    st.upper = upper;

    if (c.taskState >= 0) {
        Atom a;
        a.k = Atom::CHECK;
        a.aux = c.taskState;
        a.color = c.color;
        FontSpec f;
        f.px = c.px;
        FontMetrics fm = M.metrics(f);
        int side = std::max(10, em(c.px, 0.8));
        a.asc = side;  // we store the box size in asc (box sits on the baseline)
        a.desc = 0;
        a.lh = c.lh > 0 ? em(c.px, c.lh) : fm.ascent + fm.descent;
        a.w = side + std::max(4, em(c.px, 0.35));
        atoms.push_back(a);
        c.taskState = -1;
    }
    collect(parent, st, c, atoms);

    int avail = std::max(20, c.w);
    Line line;
    bool pendSpace = false;
    Atom pendSpaceAtom;

    auto addAtom = [&](const Atom &a) {
        line.a.push_back(a);
        line.w += a.w;
    };

    size_t i = 0;
    while (i < atoms.size()) {
        const Atom &a = atoms[i];
        if (a.k == Atom::BREAK) {
            emitLine(line, c, true);
            pendSpace = false;
            i++;
            continue;
        }
        if (a.k == Atom::SPACE) {
            if (!line.empty()) {
                pendSpace = true;
                pendSpaceAtom = a;
            }
            i++;
            continue;
        }
        // a chunk of non-breakable atoms
        size_t j = i;
        int chunkW = 0;
        while (j < atoms.size() && atoms[j].k != Atom::SPACE && atoms[j].k != Atom::BREAK) {
            chunkW += atoms[j].w;
            j++;
        }
        int spaceW = pendSpace ? pendSpaceAtom.w : 0;
        if (!line.empty() && line.w + spaceW + chunkW > avail) {
            emitLine(line, c, false);
            pendSpace = false;
            spaceW = 0;
        }
        if (line.w + spaceW + chunkW <= avail || (line.empty() && chunkW <= avail)) {
            if (pendSpace) addAtom(pendSpaceAtom);
            pendSpace = false;
            for (size_t k = i; k < j; k++) addAtom(atoms[k]);
        } else {
            // The chunk alone is wider than a whole line: break it by characters.
            if (pendSpace && !line.empty()) addAtom(pendSpaceAtom);
            pendSpace = false;
            for (size_t k = i; k < j; k++) {
                Atom rem = atoms[k];
                while (true) {
                    if (rem.k != Atom::WORD || line.w + rem.w <= avail) {
                        addAtom(rem);
                        break;
                    }
                    if (!line.empty()) {
                        emitLine(line, c, false);
                        continue;
                    }
                    int nfit = fitChars(rem, avail);
                    if (nfit >= (int)rem.s.size()) {
                        addAtom(rem);
                        break;
                    }
                    Atom head = rem;
                    head.s = rem.s.substr(0, nfit);
                    head.w = tw(head.f, head.s);
                    addAtom(head);
                    emitLine(line, c, false);
                    rem.s = rem.s.substr(nfit);
                    rem.w = tw(rem.f, rem.s);
                }
            }
        }
        i = j;
    }
    if (!line.empty()) emitLine(line, c, false);
}

}  // namespace

// ======================================================= public interface ===

void buildLayout(Layout &out, cmark_node *doc, Measurer &m, const Theme &theme,
                 const Options &opt, int viewportW) {
    Builder b(out, m, theme, opt);
    b.run(doc, viewportW);
}

// ============================================================ hit testing ===

static bool isTextItem(const Item &it) { return it.kind == IK_TEXT && it.selectable; }

bool startOfText(const Layout &L, TextPos &out) {
    for (size_t i = 0; i < L.items.size(); i++)
        if (isTextItem(L.items[i])) {
            out.item = (int)i;
            out.off = 0;
            return true;
        }
    return false;
}

bool endOfText(const Layout &L, TextPos &out) {
    for (size_t i = L.items.size(); i-- > 0;)
        if (isTextItem(L.items[i])) {
            out.item = (int)i;
            out.off = (int)L.items[i].text.size();
            return true;
        }
    return false;
}

bool hitText(const Layout &L, Measurer &m, int x, int y, TextPos &out) {
    long best = -1;
    int bi = -1;
    for (size_t i = 0; i < L.items.size(); i++) {
        const Item &it = L.items[i];
        if (!isTextItem(it)) continue;
        long dy = y < it.y ? it.y - y : (y >= it.y + it.h ? y - (it.y + it.h) + 1 : 0);
        long dx = x < it.x ? it.x - x : (x > it.x + it.w ? x - (it.x + it.w) : 0);
        long score = (dy / 8) * 100000L + dx;  // items on one line tie vertically
        if (best < 0 || score < best) {
            best = score;
            bi = (int)i;
            if (score == 0) break;
        }
    }
    if (bi < 0) return false;
    const Item &it = L.items[bi];
    int n = (int)it.text.size();
    int off;
    if (x <= it.x) {
        off = 0;
    } else if (x >= it.x + it.w) {
        off = n;
    } else {
        int lo = 0, hi = n;  // largest k with width(k) <= x - it.x
        int dx = x - it.x;
        while (lo < hi) {
            int mid = (lo + hi + 1) / 2;
            if (m.textWidth(it.font, it.text.c_str(), mid) <= dx) lo = mid;
            else hi = mid - 1;
        }
        off = lo;
        if (off < n) {
            int w0 = m.textWidth(it.font, it.text.c_str(), off);
            int w1 = m.textWidth(it.font, it.text.c_str(), off + 1);
            if (dx - w0 > (w1 - w0) / 2) off++;
        }
    }
    out.item = bi;
    out.off = off;
    return true;
}

int linkAt(const Layout &L, int x, int y) {
    for (size_t i = 0; i < L.items.size(); i++) {
        const Item &it = L.items[i];
        if ((it.kind != IK_TEXT && it.kind != IK_IMAGE) || it.link < 0) continue;
        if (x >= it.x && x < it.x + it.w && y >= it.y && y < it.y + it.h) return it.link;
    }
    return -1;
}

static bool isWordCh(wchar_t c) {
    return std::iswalnum((wint_t)c) || c == L'_' || c == L'\'' || (unsigned)c > 0x7F;
}

void wordAt(const Layout &L, TextPos p, TextPos &a, TextPos &b) {
    a = b = p;
    if (p.item < 0 || p.item >= (int)L.items.size()) return;
    const std::wstring &s = L.items[p.item].text;
    int n = (int)s.size();
    int i = std::min(p.off, n);
    if (i >= n || !isWordCh(s[i])) {
        if (i > 0 && isWordCh(s[i - 1])) i--;
        else { a.off = b.off = i; return; }
    }
    int st = i, en = i;
    while (st > 0 && isWordCh(s[st - 1])) st--;
    while (en < n && isWordCh(s[en])) en++;
    a.item = b.item = p.item;
    a.off = st;
    b.off = en;
}

void blockAt(const Layout &L, TextPos p, TextPos &a, TextPos &b) {
    a = b = p;
    if (p.item < 0 || p.item >= (int)L.items.size()) return;
    int s = p.item;
    while (s > 0 && !(isTextItem(L.items[s]) && L.items[s].sep == SEP_PARA)) s--;
    int e = p.item;
    int n = (int)L.items.size();
    while (e + 1 < n) {
        // find the next selectable text item
        int k = e + 1;
        while (k < n && !isTextItem(L.items[k])) k++;
        if (k >= n || L.items[k].sep == SEP_PARA) break;
        e = k;
    }
    a.item = s;
    a.off = 0;
    b.item = e;
    b.off = (int)L.items[e].text.size();
}

std::wstring selectedText(const Layout &L, TextPos a, TextPos b) {
    if (b < a) std::swap(a, b);
    std::wstring out;
    if (a.item < 0 || b.item < 0) return out;
    bool firstOut = true;
    for (int i = a.item; i <= b.item && i < (int)L.items.size(); i++) {
        const Item &it = L.items[i];
        if (!isTextItem(it)) continue;
        int s = (i == a.item) ? a.off : 0;
        int e = (i == b.item) ? b.off : (int)it.text.size();
        if (e <= s && i != b.item) continue;
        if (!firstOut) {
            switch (it.sep) {
                case SEP_WRAP: out += L' '; break;
                case SEP_LINE: out += L'\n'; break;
                case SEP_PARA: out += L"\n\n"; break;
                case SEP_TAB: out += L'\t'; break;
                default: break;
            }
        }
        out += it.text.substr(s, std::max(0, e - s));
        firstOut = false;
    }
    return out;
}

void selectionRects(const Layout &L, Measurer &m, TextPos a, TextPos b, std::vector<Rect> &out) {
    out.clear();
    if (b < a) std::swap(a, b);
    if (a.item < 0 || b.item < 0) return;
    for (int i = a.item; i <= b.item && i < (int)L.items.size(); i++) {
        const Item &it = L.items[i];
        if (!isTextItem(it)) continue;
        int n = (int)it.text.size();
        int s = (i == a.item) ? a.off : 0;
        int e = (i == b.item) ? b.off : n;
        if (e <= s) continue;
        int x1 = s == 0 ? 0 : m.textWidth(it.font, it.text.c_str(), s);
        int x2 = e >= n ? it.w : m.textWidth(it.font, it.text.c_str(), e);
        Rect r = {it.x + x1, it.y, x2 - x1, it.h};
        out.push_back(r);
    }
}

void findAll(const Layout &L, const std::wstring &needle, std::vector<Match> &out) {
    out.clear();
    if (needle.empty()) return;
    std::wstring nd = needle;
    for (auto &ch : nd) ch = (wchar_t)std::towlower((wint_t)ch);
    for (size_t i = 0; i < L.items.size(); i++) {
        const Item &it = L.items[i];
        if (!isTextItem(it)) continue;
        std::wstring hay = it.text;
        for (auto &ch : hay) ch = (wchar_t)std::towlower((wint_t)ch);
        size_t pos = 0;
        while ((pos = hay.find(nd, pos)) != std::wstring::npos) {
            Match mt = {(int)i, (int)pos, (int)nd.size()};
            out.push_back(mt);
            pos += nd.size();
        }
    }
}

}  // namespace mdr
