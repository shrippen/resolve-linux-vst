CXX      ?= g++
CXXFLAGS ?= -O2 -g -Wall -Wextra
CXXFLAGS += -std=c++17 -fPIC -fvisibility=hidden -fno-exceptions -fno-rtti -fvisibility-inlines-hidden
LDFLAGS  += -shared -Wl,-z,defs -static-libstdc++ -static-libgcc -Wl,--exclude-libs,ALL -Wl,--version-script=src/exports.map
LDLIBS   += -ldl

BUILD ?= build

$(BUILD)/libresolve-vst.so: src/resolve_vst.cpp src/exports.map
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -rf $(BUILD)

.PHONY: clean

PREFIX ?= $(HOME)/.local

install: $(BUILD)/libresolve-vst.so
	install -Dm755 $(BUILD)/libresolve-vst.so $(PREFIX)/lib/resolve-vst/libresolve-vst.so
	install -Dm755 resolve-vst $(PREFIX)/bin/resolve-vst
	install -Dm644 resolve-vst.desktop $(PREFIX)/share/applications/resolve-vst.desktop

uninstall:
	rm -rf $(PREFIX)/lib/resolve-vst
	rm -f $(PREFIX)/bin/resolve-vst $(PREFIX)/share/applications/resolve-vst.desktop

.PHONY: install uninstall

# Testprogramm: Editor eines Plugins wie in Resolve öffnen und aufnehmen (braucht X11, ffmpeg).
editor-test: $(BUILD)/editor-test

$(BUILD)/editor-test: tools/editor-test.cpp $(BUILD)/libresolve-vst.so
	$(CXX) -O2 -Wall -Wextra -o $@ $< -lX11 $(abspath $(BUILD)/libresolve-vst.so) -Wl,-rpath,$(abspath $(BUILD))

.PHONY: editor-test
