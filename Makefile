CXX      ?= g++
CXXFLAGS ?= -O2 -g -Wall -Wextra
CXXFLAGS += -std=c++17 -fPIC -fvisibility=hidden -fno-exceptions -fno-rtti -fvisibility-inlines-hidden
# VST3-Schnittstellen von Steinberg (MIT), Submodul: git submodule update --init
VST3     := third_party/vst3_pluginterfaces
VST3INC   = -I$(BUILD)/include
LDFLAGS  += -shared -Wl,-z,defs -static-libstdc++ -static-libgcc -Wl,--exclude-libs,ALL -Wl,--version-script=src/exports.map
LDLIBS   += -ldl

BUILD ?= build

SOURCES := src/resolve_vst.cpp src/vst3.cpp $(VST3)/base/funknown.cpp

all: $(BUILD)/libresolve-vst.so $(BUILD)/vst3-classes

# Die Header binden sich als "pluginterfaces/..." ein: Verweis unter build/include.
$(BUILD)/include/pluginterfaces:
	@test -f $(VST3)/base/funknown.h || { echo "VST3-Schnittstellen fehlen: git submodule update --init" >&2; exit 1; }
	@mkdir -p $(BUILD)/include
	ln -sfn $(abspath $(VST3)) $@

$(BUILD)/libresolve-vst.so: $(SOURCES) src/vst2.h src/exports.map | $(BUILD)/include/pluginterfaces
	$(CXX) $(CXXFLAGS) $(VST3INC) $(LDFLAGS) -o $@ $(SOURCES) $(LDLIBS)

$(BUILD)/vst3-classes: tools/vst3-classes.cpp src/vst3.cpp src/vst2.h | $(BUILD)/include/pluginterfaces
	$(CXX) $(filter-out -fPIC,$(CXXFLAGS)) $(VST3INC) -o $@ tools/vst3-classes.cpp src/vst3.cpp $(VST3)/base/funknown.cpp $(LDLIBS)

clean:
	rm -rf $(BUILD)

.PHONY: all clean

PREFIX ?= $(HOME)/.local

install: all
	install -Dm755 $(BUILD)/libresolve-vst.so $(PREFIX)/lib/resolve-vst/libresolve-vst.so
	install -Dm755 $(BUILD)/vst3-classes $(PREFIX)/lib/resolve-vst/vst3-classes
	install -Dm755 resolve-vst $(PREFIX)/bin/resolve-vst
	install -Dm644 resolve-vst.desktop $(PREFIX)/share/applications/resolve-vst.desktop

uninstall:
	rm -rf $(PREFIX)/lib/resolve-vst
	rm -f $(PREFIX)/bin/resolve-vst $(PREFIX)/share/applications/resolve-vst.desktop

.PHONY: install uninstall

# Testprogramm: Editor eines Plugins wie in Resolve öffnen und aufnehmen (braucht X11, ffmpeg).
editor-test: $(BUILD)/editor-test

$(BUILD)/editor-test: tools/editor-test.cpp src/vst2.h $(BUILD)/libresolve-vst.so
	$(CXX) -O2 -Wall -Wextra -o $@ $< -lX11 $$(pkg-config --cflags --libs glib-2.0) $(abspath $(BUILD)/libresolve-vst.so) -Wl,-rpath,$(abspath $(BUILD))

.PHONY: editor-test
