# Makefile for w64devkit (MinGW-w64 GCC + GNU make).
# Open the w64devkit shell in this folder and run:   make
# Result: GhostReader.exe  (a single static .exe, no DLLs needed)

CC      = gcc
CXX     = g++
WINDRES = windres

CMARK    = third_party/cmark-gfm
OBJDIR   = build

CFLAGS   = -O2 -std=gnu99 -w -I$(CMARK)
CXXFLAGS = -O2 -std=c++17 -Wall -Wextra -Isrc -I$(CMARK)
LDFLAGS  = -mwindows -static -s
LIBS     = -lgdi32 -luser32 -lcomdlg32 -lshell32 -ladvapi32

CMARK_SRCS = $(wildcard $(CMARK)/*.c)
CMARK_OBJS = $(patsubst $(CMARK)/%.c,$(OBJDIR)/cmark/%.o,$(CMARK_SRCS))
APP_OBJS   = $(OBJDIR)/layout.o $(OBJDIR)/main_win.o $(OBJDIR)/app_res.o

all: GhostReader.exe

GhostReader.exe: $(CMARK_OBJS) $(APP_OBJS)
	$(CXX) -o $@ $^ $(LDFLAGS) $(LIBS)

$(OBJDIR)/cmark/%.o: $(CMARK)/%.c | $(OBJDIR)/cmark
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/%.o: src/%.cpp src/layout.h | $(OBJDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJDIR)/app_res.o: res/app.rc res/app.ico | $(OBJDIR)
	$(WINDRES) -O coff -i res/app.rc -o $@

$(OBJDIR) $(OBJDIR)/cmark:
	mkdir -p $@

clean:
	rm -rf $(OBJDIR) GhostReader.exe

.PHONY: all clean
