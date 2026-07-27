# mshell — keyboard-driven WM shell replacement for Windows
#
# Cross-compile from Linux with mingw-w64.
# Lua 5.4 source must be present in vendor/lua/ (or adjust LUA_DIR below).

CC       = x86_64-w64-mingw32-gcc
WINDRES  = x86_64-w64-mingw32-windres

# --- Version (single source of truth; baked into the binary and the zip) ---
VERSION  = 0.11.0

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
           -Ivendor/lua/src \
           $(CFLAGS_EXTRA)

# CI passes -Werror through here. Kept out of CFLAGS proper so that a warning
# fails the build in CI without making a local tree unbuildable mid-edit.
CFLAGS_EXTRA ?=

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
# powrprof: SetSuspendState (sleep/hibernate actions in system.c).
# windowscodecs: WIC, which encodes screenshots to PNG. Not GDI+, whose headers
# are C++-only under mingw-w64.
LDFLAGS  = -luser32 -lgdi32 -lshell32 -lole32 -luuid -ldwmapi -lwtsapi32 \
           -ladvapi32 -lpowrprof -lwindowscodecs -lwinhttp -lm

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
              $(SRC_DIR)/session.c    \
              $(SRC_DIR)/helper.c     \
              $(SRC_DIR)/match.c      \
              $(SRC_DIR)/layout_math.c \
              $(SRC_DIR)/log.c \
              $(SRC_DIR)/overlay.c \
              $(SRC_DIR)/system.c \
              $(SRC_DIR)/screenshot.c \
              $(SRC_DIR)/notify.c \
              $(SRC_DIR)/mouse.c \
              $(SRC_DIR)/launcher.c \
              $(SRC_DIR)/layout_tree.c \
              $(SRC_DIR)/anim.c \
              $(SRC_DIR)/tweaks.c \
              $(SRC_DIR)/update.c

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

# The privileged helper: a second, tiny binary with no Lua and no config. See
# src/proto.h for why it exists and what was deliberately left out of it.
HELPER        = mshelld.exe
# log.c is shared with mshell.exe. It depends on nothing but the Win32 API —
# in particular not on the MShell global — precisely so it can link here.
HELPER_SRCS   = $(SRC_DIR)/mshelld.c $(SRC_DIR)/log.c
HELPER_OBJS   = $(HELPER_SRCS:.c=.o)
HELPER_LDLIBS = -luser32 -ladvapi32

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
.PHONY: all clean check-lua dist test regs msi

all: check-lua $(TARGET) $(HELPER)

$(TARGET): $(ALL_OBJS) $(RES_OBJ)
	@echo "  LINK  $@"
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(HELPER): $(HELPER_OBJS)
	@echo "  LINK  $@"
	$(CC) $(CFLAGS) -o $@ $^ $(HELPER_LDLIBS)

# The helper deliberately does NOT depend on mshell.h — it shares only proto.h,
# which is the point: it has no access to the shell's types or state.
$(SRC_DIR)/mshelld.o: $(SRC_DIR)/mshelld.c $(SRC_DIR)/proto.h $(SRC_DIR)/log.h
	@echo "  CC    $<"
	$(CC) $(CFLAGS) -c -o $@ $<

# log.c links into BOTH binaries, so like mshelld.o it must not pick up a
# dependency on mshell.h — it deliberately has no access to the shell's state.
$(SRC_DIR)/log.o: $(SRC_DIR)/log.c $(SRC_DIR)/log.h
	@echo "  CC    $<"
	$(CC) $(CFLAGS) -c -o $@ $<

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

# Regenerate the shipped .reg files from the table in src/tweaks.c, so the
# files and the in-process implementation cannot drift. Needs wine to run the
# cross-compiled binary; skipped with a clear message when it is absent, since
# the checked-in files are perfectly usable without regenerating them.
regs: $(TARGET)
	@if command -v wine >/dev/null 2>&1; then \
	    echo "  REGS  harden/debloat"; \
	    wine ./$(TARGET) --tweaks reg      input  > harden.reg; \
	    wine ./$(TARGET) --tweaks reg-undo input  > harden-undo.reg; \
	    wine ./$(TARGET) --tweaks reg      visual > debloat.reg; \
	    wine ./$(TARGET) --tweaks reg-undo visual > debloat-undo.reg; \
	else \
	    echo "  SKIP  regs — wine not installed (the .reg files are checked in)"; \
	fi

# An MSI, built on Linux with wixl (msitools) so CI stays single-platform.
# UNSIGNED: there is no code-signing certificate, so SmartScreen will warn. The
# zip remains the primary artifact; this is for people who want an installer
# that Add/Remove Programs knows about.
msi: $(TARGET) $(HELPER)
	@if command -v wixl >/dev/null 2>&1; then \
	    echo "  MSI   dist/mshell-$(VERSION).msi"; \
	    mkdir -p dist; \
	    wixl -D Version=$(VERSION) -o "dist/mshell-$(VERSION).msi" \
	         packaging/mshell.wxs; \
	else \
	    echo "  SKIP  msi — wixl not installed (apt install msitools)"; \
	fi

# Assemble dist/mshell-$(VERSION)-win64/ and zip it. Uses Python's zipfile so
# no `zip` binary is required. The archive keeps the versioned top-level folder.
dist: $(TARGET) $(HELPER)
	@echo "  DIST  $(DISTNAME)"
	rm -rf "$(DISTDIR)" "dist/$(DISTNAME).zip"
	mkdir -p "$(DISTDIR)/config"
	cp $(TARGET)          "$(DISTDIR)/"
	cp $(HELPER)          "$(DISTDIR)/"
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
	rm -f $(TARGET) $(HELPER) $(ALL_OBJS) $(HELPER_OBJS) $(RES_OBJ) $(TEST_BINS)
	# also remove artifacts left by Lua's own Makefile (Linux objects,
	# static lib, and the lua/luac binaries) so a stray `make` inside
	# vendor/lua can't poison our cross-compile link step.
	rm -f $(LUA_DIR)/lua.o $(LUA_DIR)/luac.o $(LUA_DIR)/liblua.a \
	      $(LUA_DIR)/lua $(LUA_DIR)/luac
