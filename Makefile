# xmp-pokey — native XMPlay input plugin (official ASAP engine, not file 638)
#
#   /usr/bin/make          # host tests + 32-bit DLL
#   /usr/bin/make dll      # dist/xmp-pokey.dll
#   /usr/bin/make test     # host render/seek/detect tests
#   /usr/bin/make pack     # /workspace/xmp-pokey-1.0.4.zip
#
# If `make` is a wrapper, invoke GNU make explicitly.

ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
DIST := $(ROOT)/dist
SRC  := $(ROOT)/src
INC  := $(ROOT)/include/xmplay
ASAP := $(ROOT)/third_party/asap
OBJ  := $(DIST)/obj
OBJW := $(DIST)/obj-i686

I686_HOST := i686-w64-mingw32
I686_CC   := $(I686_HOST)-gcc
I686_CXX  := $(I686_HOST)-g++

CFLAGS_COM = -O2 -fno-strict-aliasing -Wall -Wno-unused-function \
	-Wno-unused-parameter -DNDEBUG \
	-I$(SRC) -I$(ASAP)

CFLAGS_L = $(CFLAGS_COM) -fPIC
CFLAGS_W = $(CFLAGS_COM) -DWIN32 -D_WIN32

.PHONY: all dll test pack clean

all: test dll

dll: $(DIST)/xmp-pokey.dll

test: $(DIST)/test_asap_render
	cd $(ROOT) && $(DIST)/test_asap_render

$(OBJ)/asap.o: $(ASAP)/asap.c $(ASAP)/asap.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_L) -c -o $@ $<

$(OBJ)/pokey_player.o: $(SRC)/pokey_player.c $(SRC)/pokey_player.h $(ASAP)/asap.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_L) -c -o $@ $<

$(OBJW)/asap.o: $(ASAP)/asap.c $(ASAP)/asap.h
	@mkdir -p $(dir $@)
	$(I686_CC) $(CFLAGS_W) -c -o $@ $<

$(OBJW)/pokey_player.o: $(SRC)/pokey_player.c $(SRC)/pokey_player.h $(ASAP)/asap.h
	@mkdir -p $(dir $@)
	$(I686_CC) $(CFLAGS_W) -c -o $@ $<

$(DIST)/test_asap_render: $(ROOT)/tests/test_asap_render.c $(OBJ)/asap.o $(OBJ)/pokey_player.o
	mkdir -p $(DIST)
	$(CC) $(CFLAGS_L) -o $@ $(ROOT)/tests/test_asap_render.c \
	  $(OBJ)/asap.o $(OBJ)/pokey_player.o -lm
	# tests resolve samples relative to cwd
	# run from ROOT

$(OBJW)/xmp-pokey.res: $(SRC)/xmp-pokey.rc
	@mkdir -p $(dir $@)
	$(I686_HOST)-windres -O coff -o $@ $<

$(DIST)/xmp-pokey.dll: $(SRC)/xmp-pokey.cpp $(SRC)/xmp-pokey.def $(OBJW)/asap.o $(OBJW)/pokey_player.o $(OBJW)/xmp-pokey.res
	mkdir -p $(DIST)
	$(I686_CXX) -shared -O2 -DNDEBUG -std=c++14 \
	  -static -static-libgcc -static-libstdc++ \
	  -I$(INC) -I$(SRC) -I$(ASAP) -DWIN32 -D_WIN32 \
	  -o $@ $(SRC)/xmp-pokey.cpp $(SRC)/xmp-pokey.def \
	  $(OBJW)/asap.o $(OBJW)/pokey_player.o $(OBJW)/xmp-pokey.res \
	  -Wl,--kill-at -Wl,--add-stdcall-alias \
	  -luser32 -lgdi32 -Wl,-s
	$(I686_HOST)-objdump -p $@ | grep -E 'dll name|XMPIN_GetInterface|file format' || true
	file $@

pack: dll
	rm -f /workspace/xmp-pokey-1.0.4.zip
	mkdir -p $(DIST)/pack
	cp -f $(DIST)/xmp-pokey.dll $(ROOT)/README.md $(DIST)/pack/
	cd $(DIST)/pack && zip -9 /workspace/xmp-pokey-1.0.4.zip xmp-pokey.dll README.md
	rm -rf $(DIST)/pack
	ls -l /workspace/xmp-pokey-1.0.4.zip

clean:
	rm -rf $(DIST)/xmp-pokey.dll $(DIST)/test_asap_render $(DIST)/obj $(DIST)/obj-i686 $(DIST)/pack
