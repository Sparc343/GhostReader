// Test-only: lets Python (ctypes + PIL) supply text metrics, runs the layout
// engine and returns the items as JSON.  Not part of the Windows build.
#include <cstdio>
#include <cstring>
#include <string>
extern "C" {
#include "cmark-gfm.h"
#include "cmark-gfm-core-extensions.h"
}
#include "../src/layout.h"
using namespace mdr;

typedef int (*WidthFn)(int px, int flags, const wchar_t *s, int n);
typedef void (*MetricsFn)(int px, int flags, int *asc, int *desc);

struct PyMeasurer : Measurer {
    WidthFn wf; MetricsFn mf;
    int textWidth(const FontSpec &f, const wchar_t *s, int n) override { return wf(f.px, f.flags, s, n); }
    FontMetrics metrics(const FontSpec &f) override { FontMetrics m; mf(f.px, f.flags, &m.ascent, &m.descent); return m; }
    bool imageSize(const std::string &u, int &w, int &h) override {
        if (u.find("missing") != std::string::npos) return false;
        if (u.find("big") != std::string::npos) { w = 2400; h = 1200; return true; }
        if (u.find("small") != std::string::npos) { w = 48; h = 48; return true; }
        w = 300; h = 200; return true;
    }
};

static std::string esc(const std::string &s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o.push_back('\\'); o.push_back(c); }
        else if (c < 0x20) { char b[8]; snprintf(b, 8, "\\u%04x", c); o += b; }
        else o.push_back(c);
    }
    return o;
}

static std::string g_out;
static Layout g_L;
static PyMeasurer *g_m = nullptr;
static std::string g_sel;

extern "C" const char *bridge_layout(const char *md, int len, int viewportW, int bodyPx, int dark,
                                     WidthFn wf, MetricsFn mf, int *contentH) {
    cmark_gfm_core_extensions_ensure_registered();
    cmark_parser *p = cmark_parser_new(CMARK_OPT_DEFAULT | CMARK_OPT_VALIDATE_UTF8);
    const char *names[] = {"table", "strikethrough", "autolink", "tasklist"};
    for (auto n : names) cmark_parser_attach_syntax_extension(p, cmark_find_syntax_extension(n));
    cmark_parser_feed(p, md, len);
    cmark_node *doc = cmark_parser_finish(p);
    cmark_parser_free(p);

    static PyMeasurer m; m.wf = wf; m.mf = mf; g_m = &m;
    Layout &L = g_L; L.clear();
    Options o; o.bodyPx = bodyPx; o.codePx = bodyPx; o.imageScale = bodyPx / 16.0;
    Theme t = dark ? Theme::dark() : Theme::light();
    buildLayout(L, doc, m, t, o, viewportW);
    cmark_node_free(doc);
    *contentH = L.contentH;

    g_out = "{\"bg\":" + std::to_string(t.background) + ",\"items\":[";
    bool first = true;
    for (auto &it : L.items) {
        if (!first) g_out += ",";
        first = false;
        char b[256];
        snprintf(b, sizeof b, "{\"k\":%d,\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"asc\":%d,\"aux\":%d,\"c\":%u,\"px\":%d,\"fl\":%d,\"sep\":%d,\"t\":\"",
                 it.kind, it.x, it.y, it.w, it.h, it.asc, it.aux, it.color, it.font.px, it.font.flags, it.sep);
        g_out += b;
        g_out += esc(wideToUtf8(it.text));
        g_out += "\"}";
    }
    g_out += "]}";
    return g_out.c_str();
}

extern "C" const char *bridge_select(int x1, int y1, int x2, int y2) {
    TextPos a, b;
    if (!hitText(g_L, *g_m, x1, y1, a) || !hitText(g_L, *g_m, x2, y2, b)) return "";
    g_sel = wideToUtf8(selectedText(g_L, a, b));
    return g_sel.c_str();
}
extern "C" int bridge_find(const char *needle) {
    std::vector<Match> m;
    findAll(g_L, utf8ToWide(needle, strlen(needle)), m);
    return (int)m.size();
}
extern "C" int bridge_link(int x, int y) { return linkAt(g_L, x, y); }
extern "C" int bridge_anchor(const char *slug) {
    auto it = g_L.anchors.find(slug); return it == g_L.anchors.end() ? -1 : it->second;
}
extern "C" int bridge_headings() { return (int)g_L.headings.size(); }
