CXX      ?= g++
CXXFLAGS ?= -O2 -g -Wall -Wextra
CXXFLAGS += -std=c++17 -fPIC -fvisibility=hidden -fno-exceptions -fno-rtti -fvisibility-inlines-hidden
LDFLAGS  += -shared -Wl,-z,defs -static-libstdc++ -static-libgcc -Wl,--exclude-libs,ALL
LDLIBS   += -ldl

BUILD ?= build

$(BUILD)/libresolve-vst.so: src/resolve_vst.cpp
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
