# Build jl-git-helper against libgit2.
#
#   macOS (Homebrew):  brew install libgit2 pkg-config && make
#   Linux:             install libgit2-dev (Debian/Ubuntu) or libgit2-devel, then make
#   Windows (MSYS2 MINGW64 shell):
#     pacman -S --needed make mingw-w64-x86_64-gcc mingw-w64-x86_64-pkgconf mingw-w64-x86_64-libgit2
#     make dist      # jl-git-helper.exe plus the DLLs it needs, in dist/

CC         ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS     ?= -O2 -Wall -Wextra
LIBGIT2_CFLAGS := $(shell $(PKG_CONFIG) --cflags libgit2)
LIBGIT2_LIBS   := $(shell $(PKG_CONFIG) --libs libgit2)

ifeq ($(OS),Windows_NT)
EXE := jl-git-helper.exe
else
EXE := jl-git-helper
endif

all: $(EXE)

$(EXE): jl-git-helper.c
	$(CC) $(CFLAGS) $(LIBGIT2_CFLAGS) -o $@ $< $(LIBGIT2_LIBS)

# Windows: copy the executable and every MSYS2 DLL it loads into dist/,
# so it runs from a plain Windows Emacs without MSYS2 on PATH.
dist: $(EXE)
	rm -rf dist && mkdir dist
	cp $(EXE) dist/
	ldd $(EXE) | awk '/mingw64|ucrt64|clang64/ {print $$3}' | sort -u | xargs -r -I{} cp {} dist/

clean:
	rm -rf $(EXE) dist

.PHONY: all dist clean
