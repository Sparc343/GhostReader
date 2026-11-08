GhostReader
===========
A read-only viewer for .md files that is inspired by KDE GhostWriter and looks like its preview pane.
No Qt, no Internet Explorer, no DLLs: one static .exe.

BUILD (w64devkit, Windows 7 or later)
  1. Open the w64devkit shell, cd into this folder.
  2. make
  3. Run GhostReader.exe  (or: GhostReader.exe C:\path\file.md)

  Eclipse: File > New > Makefile Project with Existing Code, pick this folder,
  toolchain "MinGW GCC", then build target "all" (Make Targets > Build).

  To make it the default for .md files: right-click a .md file > Open with >
  Choose default program > Browse > GhostReader.exe.

KEYS
  Ctrl+O open            F5 / Ctrl+R reload (it also reloads by itself when the file changes)
  Ctrl+W close the document and return to the start screen
  Ctrl+wheel, Ctrl +/-/0 zoom                Ctrl+T dark/light theme
  Ctrl+F find, F3 / Shift+F3 next/previous   F11 full screen (Esc leaves)
  Ctrl+C copy, Ctrl+A select all, double-click = word, click right after = paragraph
  Ctrl+N narrow reading column               Ctrl+, Settings window
  Alt+Left / Alt+Right (or Backspace, mouse side buttons) back / forward after
  following a link to another .md file.  Links to "#headings" scroll in the page.
  Drag a file onto the window to open it.

SETTINGS
  Settings > Preferences... (Ctrl+,) lets you change the theme, the body and code fonts and
  sizes, ten colors per theme and the narrow reading column.  Changes show live; OK keeps
  them, Cancel undoes them, Restore Defaults resets everything in the window.
  Everything is stored in the registry only (no files):
      HKEY_CURRENT_USER\Software\GhostReader
  A color or font you never changed has no entry there and follows the built-in default.
  To forget all settings run:  GhostReader.exe --reset   (or delete that registry key).

IMAGES
  Local pictures (PNG, JPEG, GIF [first frame], BMP, TIFF) are shown inline, scaled down to fit the
  window and scaled with the zoom.  Paths are relative to the .md file.  An image that cannot be
  shown (missing file, http:// address, WebP, SVG) appears as its alt text, [image: ...].
  Pictures are re-read whenever the page reloads.

NOT SUPPORTED YET
  Downloading remote (http) images, WebP/SVG, raw HTML (tags are dropped, their text is kept),
  math, footnotes, syntax coloring in code blocks, printing.

LAYOUT
  src/layout.cpp   Markdown tree -> positioned text/rectangles (no Windows code; tested separately)
  src/main_win.cpp window, GDI drawing, scrolling, selection, find, links
  third_party/cmark-gfm   Markdown parser (BSD-2-Clause), tables/strikethrough/task lists/autolinks
  test/            developer-only layout test (Linux + Python), not needed to build
