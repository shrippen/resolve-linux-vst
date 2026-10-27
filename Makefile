CXX      ?= g++
CXXFLAGS ?= -O2 -g -Wall -Wextra
override CXXFLAGS += -std=c++17 -fPIC -fvisibility=hidden -fno-exceptions -fno-rtti -fvisibility-inlines-hidden
# VST3-Schnittstellen von Steinberg (MIT), Submodul: git submodule update --init
VST3     := third_party/vst3_pluginterfaces
VST3INC   = -I$(BUILD)/include
LDFLAGS  += -shared -Wl,-z,defs -static-libstdc++ -static-libgcc -Wl,--exclude-libs,ALL -Wl,--version-script=src/exports.map
LDLIBS   += -ldl

BUILD ?= build

SOURCES := src/resolve_linux_vst.cpp src/vst3.cpp src/aac.cpp src/nev.cpp src/x11.cpp src/sites.cpp $(VST3)/base/funknown.cpp

all: $(BUILD)/libresolve-linux-vst.so $(BUILD)/vst3-classes $(BUILD)/resolve-check

# Die Header binden sich als "pluginterfaces/..." ein: Verweis unter build/include.
$(BUILD)/include/pluginterfaces:
	@test -f $(VST3)/base/funknown.h || { echo "VST3-Schnittstellen fehlen: git submodule update --init" >&2; exit 1; }
	@mkdir -p $(BUILD)/include
	ln -sfn $(abspath $(VST3)) $@

$(BUILD)/libresolve-linux-vst.so: $(SOURCES) src/vst2.h src/sites.h src/exports.map | $(BUILD)/include/pluginterfaces
	$(CXX) $(CXXFLAGS) $(VST3INC) $(LDFLAGS) -o $@ $(SOURCES) $(LDLIBS)

$(BUILD)/vst3-classes: tools/vst3-classes.cpp src/vst3.cpp src/vst2.h | $(BUILD)/include/pluginterfaces
	$(CXX) $(filter-out -fPIC,$(CXXFLAGS)) $(VST3INC) -o $@ tools/vst3-classes.cpp src/vst3.cpp $(VST3)/base/funknown.cpp $(LDLIBS)

# Prüft ohne Resolve, ob die Stellen für den Lader noch passen (resolve-linux-vst check).
$(BUILD)/resolve-check: tools/resolve-check.cpp src/sites.cpp src/sites.h
	@mkdir -p $(BUILD)
	$(CXX) $(filter-out -fPIC,$(CXXFLAGS)) -o $@ tools/resolve-check.cpp src/sites.cpp

# Test der Mustersuche an selbst gebautem Code (ohne Resolve, läuft auch in der CI).
$(BUILD)/sites-test: tests/sites-test.cpp src/sites.cpp src/sites.h
	@mkdir -p $(BUILD)
	$(CXX) $(filter-out -fPIC,$(CXXFLAGS)) -o $@ tests/sites-test.cpp src/sites.cpp

test: $(BUILD)/sites-test
	$(BUILD)/sites-test

# clean lässt den AAC-Decoder (FFmpeg-Build dauert) stehen, distclean nimmt alles.
clean:
	rm -f $(BUILD)/libresolve-linux-vst.so $(BUILD)/vst3-classes $(BUILD)/resolve-check $(BUILD)/sites-test $(BUILD)/editor-test
	rm -rf $(BUILD)/dist

distclean:
	rm -rf $(BUILD)

.PHONY: all test clean distclean

PREFIX ?= $(HOME)/.local

install: all
	install -Dm755 $(BUILD)/libresolve-linux-vst.so $(PREFIX)/lib/resolve-linux-vst/libresolve-linux-vst.so
	install -Dm755 $(BUILD)/vst3-classes $(PREFIX)/lib/resolve-linux-vst/vst3-classes
	install -Dm755 $(BUILD)/resolve-check $(PREFIX)/lib/resolve-linux-vst/resolve-check
	if [ -f $(AAC_LIB) ]; then install -Dm755 $(AAC_LIB) $(PREFIX)/lib/resolve-linux-vst/ffmpeg-aac/libavcodec.so.60; fi
	install -Dm755 resolve-linux-vst $(PREFIX)/bin/resolve-linux-vst
	install -Dm644 resolve-linux-vst.desktop $(PREFIX)/share/applications/resolve-linux-vst.desktop
	sed -i 's|^Exec=.*|Exec=$(PREFIX)/bin/resolve-linux-vst %u|' $(PREFIX)/share/applications/resolve-linux-vst.desktop
	install -Dm644 -t $(PREFIX)/share/doc/resolve-linux-vst README.md LICENSE THIRD-PARTY.md

uninstall:
	rm -rf $(PREFIX)/lib/resolve-linux-vst $(PREFIX)/share/doc/resolve-linux-vst
	rm -f $(PREFIX)/bin/resolve-linux-vst $(PREFIX)/share/applications/resolve-linux-vst.desktop

.PHONY: install uninstall

# Testprogramm: Editor eines Plugins wie in Resolve öffnen und aufnehmen (braucht X11, ffmpeg).
editor-test: $(BUILD)/editor-test

$(BUILD)/editor-test: tools/editor-test.cpp src/vst2.h $(BUILD)/libresolve-linux-vst.so
	$(CXX) -O2 -Wall -Wextra -o $@ $< -lX11 $$(pkg-config --cflags --libs glib-2.0) $(abspath $(BUILD)/libresolve-linux-vst.so) -Wl,-rpath,$(abspath $(BUILD))

.PHONY: editor-test

# AAC-Decoder (src/aac.cpp): FFmpeg 6.0 wie in Resolve, nur libavcodec mit AAC. Lädt die
# Quellen von ffmpeg.org, sonst vom BLFS-Spiegel (dieselbe Datei; ffmpeg.org ist von GitHub
# Actions aus oft nicht erreichbar). Der Patch ist Upstream-Commit effadce6 (neue binutils).
FFMPEG_VERSION := 6.0
FFMPEG_SHA256  := 57be87c22d9b49c112b6d24bc67d42508660e6b718b3db89c44e47e289137082
FFMPEG_TAR     := $(BUILD)/ffmpeg-$(FFMPEG_VERSION).tar.xz
FFMPEG_SRC     := $(BUILD)/ffmpeg-$(FFMPEG_VERSION)
FFMPEG_URLS    := https://ffmpeg.org/releases/ffmpeg-$(FFMPEG_VERSION).tar.xz \
                  https://ftp.osuosl.org/pub/blfs/conglomeration/ffmpeg/ffmpeg-$(FFMPEG_VERSION).tar.xz
AAC_LIB        := $(BUILD)/ffmpeg-aac/libavcodec.so.60

aac: $(AAC_LIB)

$(FFMPEG_TAR):
	@mkdir -p $(BUILD)
	for url in $(FFMPEG_URLS); do \
		curl -fL --proto =https --connect-timeout 20 --retry 2 -o $@.part $$url \
			&& echo "$(FFMPEG_SHA256)  $@.part" | sha256sum -c --quiet && mv $@.part $@ && exit 0; \
		echo "$$url: failed, trying the next source" >&2; \
	done; rm -f $@.part; exit 1

$(AAC_LIB): $(FFMPEG_TAR) tools/ffmpeg-binutils.patch
	rm -rf $(FFMPEG_SRC) $(BUILD)/ffmpeg-aac-build
	tar -C $(BUILD) -xf $(FFMPEG_TAR)
	patch -d $(FFMPEG_SRC) -p1 < tools/ffmpeg-binutils.patch
	mkdir -p $(BUILD)/ffmpeg-aac-build
	cd $(BUILD)/ffmpeg-aac-build && $(abspath $(FFMPEG_SRC))/configure \
		--enable-shared --disable-static --enable-runtime-cpudetect \
		--disable-programs --disable-doc --disable-everything \
		--disable-avdevice --disable-avformat --disable-swscale --disable-postproc \
		--disable-avfilter --disable-swresample --disable-iconv --disable-lzma \
		--disable-xlib --disable-zlib --disable-bzlib --disable-vaapi --disable-vdpau \
		--disable-cuda --disable-cuvid --disable-nvdec --disable-nvenc --disable-ffnvcodec \
		--enable-decoder=aac,aac_fixed,aac_latm --enable-parser=aac,aac_latm,ac3 >/dev/null
	$(MAKE) -C $(BUILD)/ffmpeg-aac-build
	mkdir -p $(dir $@)
	strip --strip-unneeded -o $@ $(BUILD)/ffmpeg-aac-build/libavcodec/libavcodec.so.60

.PHONY: aac

# Release-Archiv (GitHub Actions, .github/workflows/ci.yml): derselbe Baum wie unter PREFIX,
# install.sh kopiert ihn nach ~/.local. Mit AAC-Decoder und den Lizenztexten, die den Binaries
# beiliegen müssen (LGPL für FFmpeg, MIT für Steinberg). Die FFmpeg-Quellen landen daneben in
# build/dist, die CI hängt sie ans Release (LGPL-2.1 §6: Quellen von derselben Stelle).
VERSION ?= $(shell git describe --tags --always 2>/dev/null | sed 's/^v//')
DIST    := resolve-linux-vst-$(VERSION)-x86_64

dist: all aac
	rm -rf $(BUILD)/dist
	$(MAKE) --no-print-directory install PREFIX=$(abspath $(BUILD)/dist/$(DIST))
	install -Dm644 tools/ffmpeg-binutils.patch $(BUILD)/dist/$(DIST)/share/doc/resolve-linux-vst/ffmpeg-binutils.patch
	tar -xOf $(FFMPEG_TAR) ffmpeg-$(FFMPEG_VERSION)/COPYING.LGPLv2.1 > $(BUILD)/dist/$(DIST)/share/doc/resolve-linux-vst/COPYING.LGPLv2.1
	install -Dm644 $(VST3)/LICENSE.txt $(BUILD)/dist/$(DIST)/share/doc/resolve-linux-vst/LICENSE.vst3_pluginterfaces
	tar -C $(BUILD)/dist -czf $(BUILD)/dist/$(DIST).tar.gz $(DIST)
	cp $(FFMPEG_TAR) $(BUILD)/dist/
	@echo $(BUILD)/dist/$(DIST).tar.gz

.PHONY: dist
