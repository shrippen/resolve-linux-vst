CXX      ?= g++
CXXFLAGS ?= -O2 -g -Wall -Wextra
override CXXFLAGS += -std=c++17 -fPIC -fvisibility=hidden -fno-exceptions -fno-rtti -fvisibility-inlines-hidden
# VST3-Schnittstellen von Steinberg (MIT), Submodul: git submodule update --init
VST3     := third_party/vst3_pluginterfaces
VST3INC   = -I$(BUILD)/include
LDFLAGS  += -pthread -shared -Wl,-z,defs -static-libstdc++ -static-libgcc -Wl,--exclude-libs,ALL -Wl,--version-script=src/exports.map
LDLIBS   += -ldl

BUILD ?= build

VST3SRC  := src/vst3.cpp src/vst3_dispatch.cpp src/vst3_host.cpp src/vst3_iids.cpp src/vst3_module.cpp src/vst3_params.cpp
NRAWSRC  := src/nraw_decode.cpp src/nraw_entropy.cpp src/nraw_color.cpp src/nraw_pool.cpp src/nraw_gpu.cpp
NRAWHDR  := src/nraw_decode.h src/nraw_frame.h src/nraw_pool.h src/nraw_gpu.h src/nraw_curves.h src/log.h
VST3HDR  := src/vst3_adapter.h src/vst3_host.h src/vst3_module.h src/vst3_params.h
SOURCES := src/resolve_linux_vst.cpp src/log.cpp src/plugin_conf.cpp src/scale.cpp src/editor_size.cpp src/scanner.cpp src/wine_dpi.cpp src/process.cpp $(VST3SRC) src/aac.cpp src/nev.cpp src/nraw.cpp src/x11.cpp src/sites.cpp src/patch.cpp $(NRAWSRC) $(VST3)/base/funknown.cpp

# N-RAW auf der GPU: eigene Bibliothek, nur mit nvcc zur Bauzeit (Kernel als PTX, compute_75 als
# Untergrenze: der Treiber übersetzt für die jeweilige Karte). Ohne nvcc entfällt sie, die
# Hauptbibliothek hängt nicht von CUDA ab.
NVCC      ?= /opt/cuda/bin/nvcc
CUDA_INC  ?= $(abspath $(dir $(NVCC))/../include)
CUDA_LIB  := $(BUILD)/libresolve-linux-vst-nraw-cuda.so
ifneq ($(wildcard $(NVCC)),)
CUDA_TARGET := $(CUDA_LIB)
else
CUDA_TARGET :=
$(info nvcc nicht gefunden ($(NVCC)): ohne N-RAW-GPU-Bibliothek)
endif

all: $(BUILD)/libresolve-linux-vst.so $(BUILD)/vst3-classes $(BUILD)/resolve-check $(BUILD)/wine-dpi-restore $(CUDA_TARGET)

$(BUILD)/nraw_cuda.ptx: src/nraw_cuda.cu src/nraw_cuda_entropy.cuh src/nraw_gpu.h
	@mkdir -p $(BUILD)
	$(NVCC) -ptx -arch=compute_75 -std=c++17 -o $@ $<

$(CUDA_LIB): src/nraw_cuda.cpp src/nraw_gpu.h $(BUILD)/nraw_cuda.ptx
	$(CXX) $(CXXFLAGS) -I$(CUDA_INC) -DNRAW_PTX_PATH='"$(abspath $(BUILD)/nraw_cuda.ptx)"' -pthread -shared -Wl,-z,defs -static-libstdc++ -static-libgcc -Wl,--exclude-libs,ALL -o $@ src/nraw_cuda.cpp -ldl

# Die Header binden sich als "pluginterfaces/..." ein: Verweis unter build/include.
$(BUILD)/include/pluginterfaces:
	@test -f $(VST3)/base/funknown.h || { echo "VST3-Schnittstellen fehlen: git submodule update --init" >&2; exit 1; }
	@mkdir -p $(BUILD)/include
	ln -sfn $(abspath $(VST3)) $@

$(BUILD)/libresolve-linux-vst.so: $(SOURCES) src/vst2.h src/log.h src/sites.h src/patch.h src/nraw.h $(NRAWHDR) src/editor_size.h src/scale.h src/plugin_conf.h src/process.h src/wine_dpi.h src/wine_base.h $(VST3HDR) src/exports.map | $(BUILD)/include/pluginterfaces
	$(CXX) $(CXXFLAGS) $(VST3INC) $(LDFLAGS) -o $@ $(SOURCES) $(LDLIBS)

$(BUILD)/vst3-classes: tools/vst3-classes.cpp $(VST3SRC) src/log.cpp src/vst2.h src/log.h $(VST3HDR) | $(BUILD)/include/pluginterfaces
	$(CXX) $(filter-out -fPIC,$(CXXFLAGS)) $(VST3INC) -o $@ tools/vst3-classes.cpp $(VST3SRC) src/log.cpp $(VST3)/base/funknown.cpp $(LDLIBS)

# Setzt die Wine-DPI nach Resolve zurück (vom Starter aufgerufen). Dieselbe Wine-Logik wie die
# Bibliothek (src/wine_dpi.cpp), ohne sie selbst zu laden.
WINE_RESTORE_SRC := tools/wine-dpi-restore.cpp src/wine_dpi.cpp src/plugin_conf.cpp src/scale.cpp src/log.cpp src/process.cpp
$(BUILD)/wine-dpi-restore: $(WINE_RESTORE_SRC) src/wine_dpi.h src/wine_restore.h src/wine_base.h src/plugin_conf.h src/scale.h src/log.h src/process.h
	@mkdir -p $(BUILD)
	$(CXX) $(filter-out -fPIC,$(CXXFLAGS)) -o $@ $(WINE_RESTORE_SRC) $(LDLIBS)

# Prüft ohne Resolve, ob die Stellen für den Lader noch passen (resolve-linux-vst check).
$(BUILD)/resolve-check: tools/resolve-check.cpp src/sites.cpp src/sites.h
	@mkdir -p $(BUILD)
	$(CXX) $(filter-out -fPIC,$(CXXFLAGS)) -o $@ tools/resolve-check.cpp src/sites.cpp

# Test der Mustersuche an selbst gebautem Code (ohne Resolve, läuft auch in der CI).
$(BUILD)/sites-test: tests/sites-test.cpp src/sites.cpp src/sites.h
	@mkdir -p $(BUILD)
	$(CXX) $(filter-out -fPIC,$(CXXFLAGS)) -o $@ tests/sites-test.cpp src/sites.cpp

# Test des Sprungs in Resolves Code (src/patch.cpp), an eigenem Speicher.
$(BUILD)/patch-test: tests/patch-test.cpp src/patch.cpp src/patch.h src/log.cpp src/log.h src/sites.cpp src/sites.h
	@mkdir -p $(BUILD)
	$(CXX) $(filter-out -fPIC,$(CXXFLAGS)) -o $@ tests/patch-test.cpp src/patch.cpp src/log.cpp src/sites.cpp

# Test der Basis-DPI-Entscheidung (rein gerechnet, ohne Wine).
$(BUILD)/wine-base-test: tests/wine-base-test.cpp src/wine_base.h
	@mkdir -p $(BUILD)
	$(CXX) $(filter-out -fPIC,$(CXXFLAGS)) -o $@ tests/wine-base-test.cpp

# Ohne weggelassene Kopien, damit ein Zeiger in eine Kopie auffällt.
$(BUILD)/process-test: tests/process-test.cpp src/process.cpp src/process.h
	@mkdir -p $(BUILD)
	$(CXX) $(filter-out -fPIC,$(CXXFLAGS)) -fno-elide-constructors -o $@ tests/process-test.cpp src/process.cpp

# Test von plugins.conf und den eingebauten Vorgaben (eigene Datei in einem Temp-Ordner).
$(BUILD)/plugin-conf-test: tests/plugin-conf-test.cpp src/plugin_conf.cpp src/plugin_conf.h
	@mkdir -p $(BUILD)
	$(CXX) $(filter-out -fPIC,$(CXXFLAGS)) -o $@ tests/plugin-conf-test.cpp src/plugin_conf.cpp

# Test der Abbildung VST3-Parameter -> VST2-Eigenschaften (baut die VST3-Quellen wie vst3-classes).
$(BUILD)/vst3-params-test: tests/vst3-params-test.cpp $(VST3SRC) src/log.cpp src/vst2.h src/log.h $(VST3HDR) | $(BUILD)/include/pluginterfaces
	@mkdir -p $(BUILD)
	$(CXX) $(filter-out -fPIC,$(CXXFLAGS)) $(VST3INC) -o $@ tests/vst3-params-test.cpp $(VST3SRC) src/log.cpp $(VST3)/base/funknown.cpp $(LDLIBS)

# Test der Auswertung der Vorwert-Datei der Wine-DPI (ohne Wine).
$(BUILD)/wine-restore-test: tests/wine-restore-test.cpp src/wine_restore.h
	@mkdir -p $(BUILD)
	$(CXX) $(filter-out -fPIC,$(CXXFLAGS)) -o $@ tests/wine-restore-test.cpp

# Test des N-RAW-Decoders mit künstlichen Puffern (ohne Prüfdaten).
$(BUILD)/nraw-curves-test: tests/nraw-curves-test.cpp src/nraw_curves.h
	@mkdir -p $(BUILD)
	$(CXX) $(filter-out -fPIC,$(CXXFLAGS)) -Isrc -o $@ tests/nraw-curves-test.cpp

$(BUILD)/nraw-guard-test: tests/nraw-guard-test.cpp $(NRAWSRC) $(NRAWHDR)
	@mkdir -p $(BUILD)
	$(CXX) $(filter-out -fPIC,$(CXXFLAGS)) -pthread -o $@ tests/nraw-guard-test.cpp $(NRAWSRC) src/log.cpp $(LDLIBS)

# Bitgenauer Test gegen fastticolib-Mitschnitte (private Prüfdaten, nicht in `make test`).
$(BUILD)/nraw-test: tests/nraw-test.cpp $(NRAWSRC) $(NRAWHDR) $(CUDA_TARGET)
	@mkdir -p $(BUILD)
	$(CXX) $(filter-out -fPIC,$(CXXFLAGS)) -pthread -o $@ tests/nraw-test.cpp $(NRAWSRC) src/log.cpp $(LDLIBS)

nraw-test: $(BUILD)/nraw-test
	$(BUILD)/nraw-test

test: $(BUILD)/process-test $(BUILD)/sites-test $(BUILD)/patch-test $(BUILD)/wine-base-test $(BUILD)/plugin-conf-test $(BUILD)/vst3-params-test $(BUILD)/wine-restore-test $(BUILD)/nraw-guard-test $(BUILD)/nraw-curves-test
	$(BUILD)/process-test
	@mkdir -p $(BUILD)/process-names
	for name in resolve VstScanner editor-test; do cp $(BUILD)/process-test $(BUILD)/process-names/$$name && $(BUILD)/process-names/$$name || exit 1; done
	$(BUILD)/sites-test
	$(BUILD)/patch-test
	$(BUILD)/wine-base-test
	$(BUILD)/plugin-conf-test
	$(BUILD)/vst3-params-test
	$(BUILD)/wine-restore-test
	$(BUILD)/nraw-guard-test
	$(BUILD)/nraw-curves-test
	tests/launcher-test.sh
	tests/scripts-test.sh

# Plugin-Test mit Waves über yabridge (nur lokal, sonst übersprungen).
plugin-test: all editor-test
	tests/wine-dpi.sh

# clean lässt den AAC-Decoder (FFmpeg-Build dauert) stehen, distclean nimmt alles.
clean:
	rm -f $(BUILD)/process-test $(BUILD)/libresolve-linux-vst.so $(BUILD)/vst3-classes $(BUILD)/resolve-check $(BUILD)/sites-test $(BUILD)/patch-test $(BUILD)/wine-base-test $(BUILD)/plugin-conf-test $(BUILD)/vst3-params-test $(BUILD)/wine-restore-test $(BUILD)/nraw-guard-test $(BUILD)/nraw-curves-test $(BUILD)/nraw-test $(BUILD)/wine-dpi-restore $(CUDA_LIB) $(BUILD)/nraw_cuda.ptx $(BUILD)/editor-test
	rm -rf $(BUILD)/dist $(BUILD)/process-names

distclean:
	rm -rf $(BUILD)

.PHONY: all test nraw-test plugin-test clean distclean

PREFIX ?= $(HOME)/.local

install: all
	install -Dm755 $(BUILD)/libresolve-linux-vst.so $(PREFIX)/lib/resolve-linux-vst/libresolve-linux-vst.so
	install -Dm755 $(BUILD)/vst3-classes $(PREFIX)/lib/resolve-linux-vst/vst3-classes
	install -Dm755 $(BUILD)/resolve-check $(PREFIX)/lib/resolve-linux-vst/resolve-check
	install -Dm755 $(BUILD)/wine-dpi-restore $(PREFIX)/lib/resolve-linux-vst/wine-dpi-restore
	if [ -f $(CUDA_LIB) ]; then install -Dm755 $(CUDA_LIB) $(PREFIX)/lib/resolve-linux-vst/libresolve-linux-vst-nraw-cuda.so; fi
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
