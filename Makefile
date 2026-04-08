# Makefile for sqlite-clone
#
# Targets
#   make cli   → sqlite_clone_cli.exe   (console REPL, existing behaviour)
#   make gui   → sqlite_clone.exe       (Win32 GUI, no console window)
#   make all   → both
#   make clean → remove build artefacts
#
# Requires MinGW-w64 (gcc, windres) on Windows.
# Install from https://www.mingw-w64.org/ or via MSYS2:
#   pacman -S mingw-w64-x86_64-gcc

CC      = gcc
CFLAGS  = -std=c99 -Wall -Wextra -O2
LDFLAGS =

# Common engine object
ENGINE  = db.c

.PHONY: all cli gui clean

all: cli gui

# ── CLI ──────────────────────────────────────────────────────────────
cli: sqlite_clone_cli.exe

sqlite_clone_cli.exe: $(ENGINE) main.c
	$(CC) $(CFLAGS) -o $@ $^

# ── GUI ──────────────────────────────────────────────────────────────
gui: sqlite_clone.exe

sqlite_clone.exe: $(ENGINE) gui.c app.res
	$(CC) $(CFLAGS) -mwindows -o $@ $(ENGINE) gui.c app.res -lcomdlg32

# Compile the Windows resource file (icon + version info)
app.res: app.rc
	windres app.rc -O coff -o app.res

# ── Clean ─────────────────────────────────────────────────────────────
clean:
	rm -f sqlite_clone_cli.exe sqlite_clone.exe app.res
