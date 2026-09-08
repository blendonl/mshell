CC       = x86_64-w64-mingw32-gcc
WINDRES  = x86_64-w64-mingw32-windres

VERSION  = 0.15.4

VER_MAJOR := $(word 1,$(subst ., ,$(VERSION)))
VER_MINOR := $(word 2,$(subst ., ,$(VERSION)))
VER_PATCH := $(word 3,$(subst ., ,$(VERSION)))

CFLAGS   = -O2 -s -flto -mwindows \
           -DUNICODE -D_UNICODE \
           -DMSHELL_VERSION='"$(VERSION)"' \
           -Wall -Wextra -Wno-unused-parameter \
           -Ivendor/lua/src \
           $(CFLAGS_EXTRA)

CFLAGS_EXTRA ?=

RCFLAGS  = -DVER_MAJOR=$(VER_MAJOR) \
           -DVER_MINOR=$(VER_MINOR) \
           -DVER_PATCH=$(VER_PATCH)
LDFLAGS  = -luser32 -lgdi32 -lshell32 -lole32 -luuid -ldwmapi -lwtsapi32 \
           -ladvapi32 -lpowrprof -lwindowscodecs -lwinhttp -lbcrypt -lm

SRC_DIR  = src
LUA_DIR  = vendor/lua/src

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
              $(SRC_DIR)/helper.c     \
              $(SRC_DIR)/match.c      \
              $(SRC_DIR)/layout_math.c \
              $(SRC_DIR)/desktop_list.c \
              $(SRC_DIR)/api_spec.c \
              $(SRC_DIR)/whichkey_math.c \
              $(SRC_DIR)/log.c \
              $(SRC_DIR)/pipe_sd.c \
              $(SRC_DIR)/overlay.c \
              $(SRC_DIR)/system.c \
              $(SRC_DIR)/screenshot.c \
              $(SRC_DIR)/notify.c \
              $(SRC_DIR)/mouse.c \
              $(SRC_DIR)/launcher.c \
              $(SRC_DIR)/layout_tree.c \
              $(SRC_DIR)/anim.c \
              $(SRC_DIR)/tweaks.c \
              $(SRC_DIR)/display.c \
              $(SRC_DIR)/update_parse.c \
              $(SRC_DIR)/update.c

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

ALL_SRCS = $(MSHELL_SRCS) $(LUA_SRCS)
ALL_OBJS = $(ALL_SRCS:.c=.o)

MSHELL_OBJS = $(MSHELL_SRCS:.c=.o)

RES_OBJ  = $(SRC_DIR)/mshell.res.o

TARGET   = mshell.exe

HELPER        = mshelld.exe
HELPER_SRCS   = $(SRC_DIR)/mshelld.c $(SRC_DIR)/log.c $(SRC_DIR)/pipe_sd.c
HELPER_OBJS   = $(HELPER_SRCS:.c=.o)
HELPER_LDLIBS = -luser32 -ladvapi32 -ldwmapi

DISTNAME = mshell-$(VERSION)-win64
DISTDIR  = dist/$(DISTNAME)
DIST_FILES = install.bat uninstall.bat \
             harden.reg harden-undo.reg \
             debloat.reg debloat-undo.reg \
             services.reg services-undo.reg \
             INSTALL.md README.md CHANGELOG.md MANUAL-TESTS.md LICENSE

HOST_CC   = cc
TEST_DIR  = test
TEST_BINS = $(TEST_DIR)/test_match $(TEST_DIR)/test_layout_math \
            $(TEST_DIR)/test_whichkey_math $(TEST_DIR)/test_update_parse \
            $(TEST_DIR)/test_desktop_list $(TEST_DIR)/test_api_spec

.PHONY: all clean check-lua dist test regs msi print-version probe meta check-config

all: check-lua $(TARGET) $(HELPER)

print-version:
	@echo $(VERSION)

VERSION_STAMP = .version-$(VERSION)

$(VERSION_STAMP):
	@rm -f .version-*
	@touch $@

$(MSHELL_OBJS) $(HELPER_OBJS) $(RES_OBJ): $(VERSION_STAMP)

$(TARGET): $(ALL_OBJS) $(RES_OBJ)
	@echo "  LINK  $@"
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(HELPER): $(HELPER_OBJS)
	@echo "  LINK  $@"
	$(CC) $(CFLAGS) -o $@ $^ $(HELPER_LDLIBS)

$(SRC_DIR)/mshelld.o: $(SRC_DIR)/mshelld.c $(SRC_DIR)/proto.h $(SRC_DIR)/log.h
	@echo "  CC    $<"
	$(CC) $(CFLAGS) -c -o $@ $<

$(SRC_DIR)/log.o: $(SRC_DIR)/log.c $(SRC_DIR)/log.h
	@echo "  CC    $<"
	$(CC) $(CFLAGS) -c -o $@ $<

$(SRC_DIR)/pipe_sd.o: $(SRC_DIR)/pipe_sd.c $(SRC_DIR)/pipe_sd.h
	@echo "  CC    $<"
	$(CC) $(CFLAGS) -c -o $@ $<

$(SRC_DIR)/%.o: $(SRC_DIR)/%.c $(SRC_DIR)/mshell.h
	@echo "  CC    $<"
	$(CC) $(CFLAGS) -c -o $@ $<

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

regs: $(TARGET)
	@if command -v wine >/dev/null 2>&1; then \
	    echo "  REGS  harden/debloat"; \
	    set -e; \
	    tmp=$$(mktemp -d); \
	    wine ./$(TARGET) --tweaks reg      input  > "$$tmp/harden.reg"; \
	    wine ./$(TARGET) --tweaks reg-undo input  > "$$tmp/harden-undo.reg"; \
	    wine ./$(TARGET) --tweaks reg      visual > "$$tmp/debloat.reg"; \
	    wine ./$(TARGET) --tweaks reg-undo visual > "$$tmp/debloat-undo.reg"; \
	    for f in harden harden-undo debloat debloat-undo; do \
	        test -s "$$tmp/$$f.reg" || { echo "  ERROR $$f.reg came out empty"; rm -rf "$$tmp"; exit 1; }; \
	    done; \
	    mv "$$tmp"/*.reg .; \
	    rm -rf "$$tmp"; \
	else \
	    echo "  SKIP  regs — wine not installed (the .reg files are checked in)"; \
	fi

msi: $(TARGET) $(HELPER)
	@if command -v wixl >/dev/null 2>&1; then \
	    echo "  MSI   dist/mshell-$(VERSION).msi"; \
	    mkdir -p dist; \
	    wixl -D Version=$(VERSION) -o "dist/mshell-$(VERSION).msi" \
	         packaging/mshell.wxs; \
	else \
	    echo "  SKIP  msi — wixl not installed (apt install wixl)"; \
	fi

dist: $(TARGET) $(HELPER)
	@echo "  DIST  $(DISTNAME)"
	rm -rf "$(DISTDIR)" "dist/$(DISTNAME).zip"
	mkdir -p "$(DISTDIR)/config" "$(DISTDIR)/meta"
	cp $(TARGET)          "$(DISTDIR)/"
	cp $(HELPER)          "$(DISTDIR)/"
	cp config/init.lua      "$(DISTDIR)/config/"
	cp config/init.full.lua "$(DISTDIR)/config/"
	cp meta/mshell.lua      "$(DISTDIR)/meta/"
	cp meta/types.lua       "$(DISTDIR)/meta/"
	cp config/.luarc.json   "$(DISTDIR)/config/"
	cp $(DIST_FILES)      "$(DISTDIR)/"
	cd dist && python3 -m zipfile -c "$(DISTNAME).zip" "$(DISTNAME)"
	@echo "  ->    dist/$(DISTNAME).zip"

$(TEST_DIR)/test_match: $(TEST_DIR)/test_match.c $(SRC_DIR)/match.c $(SRC_DIR)/match.h
	@echo "  HOSTCC $@"
	$(HOST_CC) -O1 -Wall -Wextra -o $@ $(TEST_DIR)/test_match.c $(SRC_DIR)/match.c

$(TEST_DIR)/test_layout_math: $(TEST_DIR)/test_layout_math.c $(SRC_DIR)/layout_math.c $(SRC_DIR)/layout_math.h
	@echo "  HOSTCC $@"
	$(HOST_CC) -O1 -Wall -Wextra -o $@ $(TEST_DIR)/test_layout_math.c $(SRC_DIR)/layout_math.c

$(TEST_DIR)/test_whichkey_math: $(TEST_DIR)/test_whichkey_math.c $(SRC_DIR)/whichkey_math.c $(SRC_DIR)/whichkey_math.h
	@echo "  HOSTCC $@"
	$(HOST_CC) -O1 -Wall -Wextra -o $@ $(TEST_DIR)/test_whichkey_math.c $(SRC_DIR)/whichkey_math.c

$(TEST_DIR)/test_update_parse: $(TEST_DIR)/test_update_parse.c $(SRC_DIR)/update_parse.c $(SRC_DIR)/update_parse.h
	@echo "  HOSTCC $@"
	$(HOST_CC) -O1 -Wall -Wextra -o $@ $(TEST_DIR)/test_update_parse.c $(SRC_DIR)/update_parse.c

$(TEST_DIR)/test_desktop_list: $(TEST_DIR)/test_desktop_list.c $(SRC_DIR)/desktop_list.c $(SRC_DIR)/desktop_list.h
	@echo "  HOSTCC $@"
	$(HOST_CC) -O1 -Wall -Wextra -o $@ $(TEST_DIR)/test_desktop_list.c $(SRC_DIR)/desktop_list.c

$(TEST_DIR)/test_api_spec: $(TEST_DIR)/test_api_spec.c $(SRC_DIR)/api_spec.c $(SRC_DIR)/api_spec.h
	@echo "  HOSTCC $@"
	$(HOST_CC) -O1 -Wall -Wextra -o $@ $(TEST_DIR)/test_api_spec.c $(SRC_DIR)/api_spec.c

GEN_META = tools/gen_lua_meta

$(GEN_META): tools/gen_lua_meta.c $(SRC_DIR)/api_spec.c $(SRC_DIR)/api_spec.h
	@echo "  HOSTCC $@"
	$(HOST_CC) -O1 -Wall -Wextra -o $@ tools/gen_lua_meta.c $(SRC_DIR)/api_spec.c

meta: $(GEN_META)
	@echo "  META   meta/mshell.lua"
	@./$(GEN_META) meta/mshell.lua

PROBE = tools/probe_shellcloak.exe tools/probe_dpiband.exe \
       tools/probe_frame.exe

probe: $(PROBE)

tools/probe_shellcloak.exe: tools/probe_shellcloak.c
	@echo "  CC     $@"
	$(CC) -O1 -municode -DUNICODE -D_UNICODE -Wall -Wextra \
	      -o $@ $< -lole32 -ldwmapi -luser32

tools/probe_dpiband.exe: tools/probe_dpiband.c
	@echo "  CC     $@"
	$(CC) -O1 -municode -DUNICODE -D_UNICODE -Wall -Wextra \
	      -o $@ $< -ldwmapi -luser32 -lgdi32

tools/probe_frame.exe: tools/probe_frame.c
	@echo "  CC     $@"
	$(CC) -O1 -municode -DUNICODE -D_UNICODE -Wall -Wextra \
	      -o $@ $< -ldwmapi -luser32 -lgdi32

HOST_LUA = $(TEST_DIR)/lua

$(HOST_LUA): $(LUA_SRCS) $(LUA_DIR)/lua.c
	@echo "  HOSTCC $@"
	@$(HOST_CC) -O1 -w -o $@ -I$(LUA_DIR) $(LUA_SRCS) $(LUA_DIR)/lua.c -lm

check-config: $(HOST_LUA)
	@echo "  CONFIG"
	@./$(HOST_LUA) $(TEST_DIR)/check_config.lua $(SRC_DIR)/api_spec.c \
	    config/init.lua config/init.full.lua README.md

test: $(TEST_BINS) check-config
	@echo "  TEST"
	@fail=0; for t in $(TEST_BINS); do ./$$t || fail=1; done; \
	 if [ $$fail -ne 0 ]; then echo "  TESTS FAILED"; exit 1; fi; \
	 echo "  all tests passed"

clean:
	rm -f $(TARGET) $(HELPER) $(ALL_OBJS) $(HELPER_OBJS) $(RES_OBJ) $(TEST_BINS) $(PROBE) $(GEN_META) $(HOST_LUA)
	rm -f .version-*
	rm -f $(LUA_DIR)/lua.o $(LUA_DIR)/luac.o $(LUA_DIR)/liblua.a \
	      $(LUA_DIR)/lua $(LUA_DIR)/luac
