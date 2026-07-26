# mshell — keyboard-driven WM shell replacement for Windows
#
# Cross-compile from Linux with mingw-w64.
# Lua 5.4 source must be present in vendor/lua/ (or adjust LUA_DIR below).

CC       = x86_64-w64-mingw32-gcc
WINDRES  = x86_64-w64-mingw32-windres

# --- Version (single source of truth; baked into the binary and the zip) ---
VERSION  = 0.10.0

# VERSIONINFO needs the parts as separate numbers, so split them out here
# rather than making anyone maintain the version in two shapes.
VER_MAJOR := $(word 1,$(subst ., ,$(VERSION)))
VER_MINOR := $(word 2,$(subst ., ,$(VERSION)))
VER_PATCH := $(word 3,$(subst ., ,$(VERSION)))

# --- Flags ---
CFLAGS   = -O2 -s -flto -mwindows \
           -DUNICODE -D_UNICODE \
           -DMSHELL_VERSION='"$(VERSION)"' \
           -Wall -Wextra -Wno-unused-parameter \
           -Ivendor/lua/src

# Only integers are passed to windres. It re-invokes a shell to run the
# preprocessor, so a -D carrying a quoted string has its quotes stripped twice
# and arrives as a bare token — mshell.rc builds the display string from these
# three numbers itself instead.
RCFLAGS  = -DVER_MAJOR=$(VER_MAJOR) \
           -DVER_MINOR=$(VER_MINOR) \
           -DVER_PATCH=$(VER_PATCH)
# ole32 + uuid are for SHGetKnownFolderPath/FOLDERID_RoamingAppData (config
# path resolution in main.c): CoTaskMemFree lives in ole32, the FOLDERID_* GUID
# symbols in uuid.
# advapi32: the IPC pipe's DACL (ConvertSidToStringSid,
# ConvertStringSecurityDescriptorToSecurityDescriptor).
LDFLAGS  = -luser32 -lgdi32 -lshell32 -lole32 -luuid -ldwmapi -lwtsapi32 \
           -ladvapi32 -lm

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
              $(SRC_DIR)/whichkey.c   \
              $(SRC_DIR)/bar.c        \
              $(SRC_DIR)/ipc.c        \
              $(SRC_DIR)/match.c      \
              $(SRC_DIR)/layout_math.c

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

# Resources: the application manifest (DPI awareness) + VERSIONINFO.
RES_OBJ  = $(SRC_DIR)/mshell.res.o

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
             INSTALL.md README.md CHANGELOG.md MANUAL-TESTS.md LICENSE

# --- Host-side tests ---
# mshell itself cross-compiles to Windows and cannot run here, but the logic
# with no Windows in it can: match.c (rule patterns) and layout_math.c (the
# proportional split). Those are built with the HOST compiler and run directly,
# so `make test` needs no emulator and no Windows machine. Everything else is
# covered by MANUAL-TESTS.md.
HOST_CC   = cc
TEST_DIR  = test
TEST_BINS = $(TEST_DIR)/test_match $(TEST_DIR)/test_layout_math

# --- Rules ---
.PHONY: all clean check-lua dist test

all: check-lua $(TARGET)

$(TARGET): $(ALL_OBJS) $(RES_OBJ)
	@echo "  LINK  $@"
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(SRC_DIR)/%.o: $(SRC_DIR)/%.c $(SRC_DIR)/mshell.h
	@echo "  CC    $<"
	$(CC) $(CFLAGS) -c -o $@ $<

# Resource script -> linkable object. Depends on the manifest too, so editing
# the manifest alone still rebuilds.
$(RES_OBJ): $(SRC_DIR)/mshell.rc $(SRC_DIR)/mshell.exe.manifest
	@echo "  RC    $<"
	$(WINDRES) $(RCFLAGS) -I$(SRC_DIR) -O coff -i $< -o $@

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
	cp config/init.lua      "$(DISTDIR)/config/"
	cp config/init.full.lua "$(DISTDIR)/config/"
	cp $(DIST_FILES)      "$(DISTDIR)/"
	cd dist && python3 -m zipfile -c "$(DISTNAME).zip" "$(DISTNAME)"
	@echo "  ->    dist/$(DISTNAME).zip"

$(TEST_DIR)/test_match: $(TEST_DIR)/test_match.c $(SRC_DIR)/match.c $(SRC_DIR)/match.h
	@echo "  HOSTCC $@"
	$(HOST_CC) -O1 -Wall -Wextra -o $@ $(TEST_DIR)/test_match.c $(SRC_DIR)/match.c

$(TEST_DIR)/test_layout_math: $(TEST_DIR)/test_layout_math.c $(SRC_DIR)/layout_math.c $(SRC_DIR)/layout_math.h
	@echo "  HOSTCC $@"
	$(HOST_CC) -O1 -Wall -Wextra -o $@ $(TEST_DIR)/test_layout_math.c $(SRC_DIR)/layout_math.c

test: $(TEST_BINS)
	@echo "  TEST"
	@fail=0; for t in $(TEST_BINS); do ./$$t || fail=1; done; \
	 if [ $$fail -ne 0 ]; then echo "  TESTS FAILED"; exit 1; fi; \
	 echo "  all tests passed"

clean:
	rm -f $(TARGET) $(ALL_OBJS) $(RES_OBJ) $(TEST_BINS)
	# also remove artifacts left by Lua's own Makefile (Linux objects,
	# static lib, and the lua/luac binaries) so a stray `make` inside
	# vendor/lua can't poison our cross-compile link step.
	rm -f $(LUA_DIR)/lua.o $(LUA_DIR)/luac.o $(LUA_DIR)/liblua.a \
	      $(LUA_DIR)/lua $(LUA_DIR)/luac
