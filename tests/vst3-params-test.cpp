// Test der Abbildung VST3-Parameter -> VST2-Eigenschaften (src/vst3_params.cpp).
// Läuft ohne Resolve und ohne Plugin (make test).
#include "../src/vst2.h"
#include "../src/vst3_params.h"

#include <cstdio>
#include <string>

namespace {

int failures = 0;

void expect(bool ok, const char *what)
{
    printf("%s  %s\n", ok ? "ok   " : "FEHLER", what);
    failures += !ok;
}

// Schreibt `s` (nur ASCII) als UTF-16 in ein Feld fester Größe.
template<size_t N>
void fill(char16 (&dest)[N], const std::string &s)
{
    size_t i = 0;
    for (; i < s.size() && i + 1 < N; ++i) {
        dest[i] = char16(s[i]);
    }
    dest[i] = 0;
}

ParameterInfo info(const std::string &title, const std::string &shortTitle, int32 steps)
{
    ParameterInfo p = {};
    fill(p.title, title);
    fill(p.shortTitle, shortTitle);
    p.stepCount = steps;
    return p;
}

VstParameterProperties map(const ParameterInfo &p)
{
    VstParameterProperties props;
    vst3::paramProperties(p, &props);
    return props;
}

} // namespace

int main()
{
    const VstParameterProperties cont = map(info("Gain", "G", 0));
    expect(cont.flags == 0, "stepCount 0: keine Flags");

    const VstParameterProperties sw = map(info("Bypass", "B", 1));
    expect(sw.flags == kVstParameterIsSwitch, "stepCount 1: nur Switch");

    const VstParameterProperties stepped = map(info("Mode", "M", 3));
    expect(stepped.flags == (kVstParameterUsesIntegerMinMax | kVstParameterUsesIntStep),
           "stepCount 3: IntegerMinMax | IntStep");
    expect(stepped.minInteger == 0 && stepped.maxInteger == 3, "stepCount 3: min 0, max 3");
    expect(stepped.stepInteger == 1 && stepped.largeStepInteger == 1, "stepCount 3: Schritte 1");

    const VstParameterProperties longer = map(info(std::string(100, 'x'), "ABCDEFGHIJ", 0));
    expect(std::string(longer.label) == std::string(63, 'x'), "Label auf 63 Zeichen gekürzt");
    expect(std::string(longer.shortLabel) == "ABCDEFG", "Kurzlabel auf 7 Zeichen gekürzt");

    return failures ? 1 : 0;
}
