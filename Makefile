# mshell — keyboard-driven WM shell replacement for Windows
#
# Cross-compile from Linux with mingw-w64.
# Lua 5.4 source must be present in vendor/lua/ (or adjust LUA_DIR below).

CC       = x86_64-w64-mingw32-gcc

# --- Version (single source of truth; baked into the binary and the zip) ---
VERSION  = 0.7.0

# --- Flags ---
CFLAGS   = -O2 -s -flto -mwindows \
           -DUNICODE -D_UNICODE \
           -DMSHELL_VERSION='"$(VERSION)"' \
           -Wall -Wextra -Wno-unused-parameter \
           -Ivendor/lua/src
# ole32 + uuid are for SHGetKnownFolderPath/FOLDERID_RoamingAppData (config
# path resolution in main.c): CoTaskMemFree lives in ole32, the FOLDERID_* GUID
# symbols in uuid.
LDFLAGS  = -luser32 -lgdi32 -lshell32 -lole32 -luuid -ldwmapi -lwtsapi32 -lm

# --- Paths ---
SRC_DIR  = src
LUA_DIR  = vendor/lua/src

# --- mshell sources ---
MSHELL_SRCS = $(SRC_DIR)/main.c       \
              $(SRC_DIR)/keyboard.c   \
              $(SRC_DIR)/window.c     \
              $(SRC_DIR)/tiling.c     \
              $(SRC_DIR)/desktop.c    \
              $(SRC_DIR)/events.c     \
              $(SRC_DIR)/config.c     \
              $(SRC_DIR)/lua_api.c    \
              $(SRC_DIR)/border.c     \
              $(SRC_DIR)/background.c \
              $(SRC_DIR)/whichkey.c

# --- Lua sources (amalgamated or individual) ---
# Lua 5.4 core source files:
LUA_SRCS  = $(LUA_DIR)/lapi.c       \
            $(LUA_DIR)/lauxlib.c    \
            $(LUA_DIR)/lbaselib.c   \
            $(LUA_DIR)/lcode.c      \
            $(LUA_DIR)/lcorolib.c   \
            $(LUA_DIR)/lctype.c     \
            $(LUA_DIR)/ldblib.c     \
            $(LUA_DIR)/ldebug.c     \
            $(LUA_DIR)/ldo.c        \
            $(LUA_DIR)/ldump.c      \
            $(LUA_DIR)/lfunc.c      \
            $(LUA_DIR)/lgc.c        \
            $(LUA_DIR)/linit.c      \
            $(LUA_DIR)/liolib.c     \
            $(LUA_DIR)/llex.c       \
            $(LUA_DIR)/lmathlib.c   \
            $(LUA_DIR)/lmem.c       \
            $(LUA_DIR)/loadlib.c    \
            $(LUA_DIR)/lobject.c    \
            $(LUA_DIR)/lopcodes.c   \
            $(LUA_DIR)/loslib.c     \
            $(LUA_DIR)/lparser.c    \
            $(LUA_DIR)/lstate.c     \
            $(LUA_DIR)/lstring.c    \
            $(LUA_DIR)/lstrlib.c    \
            $(LUA_DIR)/ltable.c     \
            $(LUA_DIR)/ltablib.c    \
            $(LUA_DIR)/ltm.c        \
            $(LUA_DIR)/lundump.c    \
            $(LUA_DIR)/lutf8lib.c   \
            $(LUA_DIR)/lvm.c        \
            $(LUA_DIR)/lzio.c

# If you use the Lua amalgamation (lua.c + luac.c → just lua.c), uncomment:
# LUA_SRCS  = $(LUA_DIR)/lua.c

ALL_SRCS = $(MSHELL_SRCS) $(LUA_SRCS)
ALL_OBJS = $(ALL_SRCS:.c=.o)

TARGET   = mshell.exe

# --- Release packaging ---
DISTNAME = mshell-$(VERSION)-win64
DISTDIR  = dist/$(DISTNAME)
# Files shipped in the release zip. The optional debloat/services tweaks are
# included because INSTALL.md step 5 tells you to import them — shipping the
# docs without the files they name leaves the release half-usable.
DIST_FILES = install.bat uninstall.bat \
             harden.reg harden-undo.reg \
             debloat.reg debloat-undo.reg \
             services.reg services-undo.reg \
             INSTALL.md README.md CHANGELOG.md LICENSE

# --- Rules ---
.PHONY: all clean check-lua dist

all: check-lua $(TARGET)

$(TARGET): $(ALL_OBJS)
	@echo "  LINK  $@"
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(SRC_DIR)/%.o: $(SRC_DIR)/%.c $(SRC_DIR)/mshell.h
	@echo "  CC    $<"
	$(CC) $(CFLAGS) -c -o $@ $<

$(LUA_DIR)/%.o: $(LUA_DIR)/%.c
	@echo "  CC    $<"
	$(CC) $(CFLAGS) -DLUA_COMPAT_5_3 -c -o $@ $<

check-lua:
	@if [ ! -d "$(LUA_DIR)" ]; then \
		echo ""; \
		echo "  ============================================================"; \
		echo "  Lua 5.4 source not found at $(LUA_DIR)"; \
		echo ""; \
		echo "  Download it:"; \
		echo "    mkdir -p vendor/lua"; \
		echo "    cd vendor/lua"; \
		echo "    curl -LO https://www.lua.org/ftp/lua-5.4.7.tar.gz"; \
		echo "    tar xzf lua-5.4.7.tar.gz --strip-components=1"; \
		echo "  ============================================================"; \
		echo ""; \
		exit 1; \
	fi

# Assemble dist/mshell-$(VERSION)-win64/ and zip it. Uses Python's zipfile so
# no `zip` binary is required. The archive keeps the versioned top-level folder.
dist: $(TARGET)
	@echo "  DIST  $(DISTNAME)"
	rm -rf "$(DISTDIR)" "dist/$(DISTNAME).zip"
	mkdir -p "$(DISTDIR)/config"
	cp $(TARGET)          "$(DISTDIR)/"
	cp config/init.lua    "$(DISTDIR)/config/"
	cp $(DIST_FILES)      "$(DISTDIR)/"
	cd dist && python3 -m zipfile -c "$(DISTNAME).zip" "$(DISTNAME)"
	@echo "  ->    dist/$(DISTNAME).zip"

clean:
	rm -f $(TARGET) $(ALL_OBJS)
	# also remove artifacts left by Lua's own Makefile (Linux objects,
	# static lib, and the lua/luac binaries) so a stray `make` inside
	# vendor/lua can't poison our cross-compile link step.
	rm -f $(LUA_DIR)/lua.o $(LUA_DIR)/luac.o $(LUA_DIR)/liblua.a \
	      $(LUA_DIR)/lua $(LUA_DIR)/luac
