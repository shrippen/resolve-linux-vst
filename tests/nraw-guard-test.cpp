// Test des N-RAW-Decoders ohne Prüfdaten: fremde, abgeschnittene und beschädigte Codestreams
// ergeben Unsupported oder Corrupt und stürzen nicht ab (künstliche Puffer; make test).
#include "../src/nraw_decode.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

int failures = 0;

void expect(bool ok, const char *what)
{
    printf("%s  %s\n", ok ? "ok   " : "FEHLER", what);
    failures += !ok;
}

constexpr size_t WIDTH = 4140;
constexpr size_t HEIGHT = 2330;
constexpr size_t LCOD_OFFSET = 0x2a; // Lcod im PIH
constexpr size_t NC_OFFSET = 0x3a;   // Nc im PIH
constexpr size_t SLICE_SIZE = 6;

// Kopf eines N-RAW-Codestreams (SOC bis WGT), ohne Bilddaten.
const uint8_t HEADER[] = {
    0xff, 0x10, 0xff, 0x50, 0x00, 0x22, 0x43, 0x4f, 0x4e, 0x54, 0x41, 0x43,
    0x54, 0x5f, 0x49, 0x4e, 0x54, 0x4f, 0x50, 0x49, 0x58, 0x5f, 0xef, 0xc0,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xff, 0x12, 0x00, 0x27, 0x00, 0x1b, 0x9a, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x10, 0x2c, 0x09, 0x1a, 0x00, 0x00, 0x00, 0x10, 0x04, 0x04,
    0x08, 0x12, 0x44, 0x10, 0x51, 0x14, 0x50, 0x88, 0x70, 0x83, 0xf0, 0x15,
    0x23, 0xd1, 0x49, 0xcd, 0x3f, 0x7f, 0x07, 0xff, 0x13, 0x00, 0x0e, 0x0e,
    0x11, 0x51, 0x0e, 0x11, 0x51, 0x0e, 0x11, 0x00, 0x0e, 0x11, 0x51, 0xff,
    0x14, 0x00, 0x34, 0x03, 0x02, 0x03, 0x13, 0x02, 0x09, 0x02, 0x16, 0x01,
    0x0a, 0x01, 0x15, 0x02, 0x05, 0x01, 0x03, 0x01, 0x11, 0x00, 0x01, 0x00,
    0x07, 0x00, 0x0d, 0x00, 0x0b, 0x02, 0x06, 0x01, 0x04, 0x01, 0x10, 0x00,
    0x00, 0x00, 0x08, 0x00, 0x0e, 0x01, 0x14, 0x00, 0x12, 0x00, 0x0f, 0x00,
    0x17, 0x00, 0x0c, 0x00, 0x18,
};

const int32_t CURVE_VALUES[] = {0, 4, 8, 12};

NrawResult run(const std::vector<uint8_t> &cs, size_t count = WIDTH * HEIGHT)
{
    std::vector<uint16_t> out(count, 0);
    NrawCurve curve{CURVE_VALUES, sizeof CURVE_VALUES / sizeof *CURVE_VALUES};
    return decodeNraw(cs.data(), cs.size(), curve, out.data(), out.size());
}

// Kopf, ein SLH und Füllbytes; Lcod passt zur Länge.
std::vector<uint8_t> stream(size_t body, uint8_t fill)
{
    std::vector<uint8_t> cs(HEADER, HEADER + sizeof HEADER);
    const uint8_t slh[SLICE_SIZE] = {0xff, 0x20, 0x00, 0x04, 0x00, 0x00};
    cs.insert(cs.end(), slh, slh + SLICE_SIZE);
    cs.insert(cs.end(), body, fill);
    size_t total = cs.size();
    for (int i = 0; i < 4; i++) {
        cs[LCOD_OFFSET + i] = uint8_t(total >> (8 * (3 - i)));
    }
    return cs;
}

} // namespace

int main()
{
    expect(run({}) == NrawResult::Unsupported, "leer");
    expect(run(std::vector<uint8_t>(64, 0xab)) == NrawResult::Unsupported, "fremde Daten");

    std::vector<uint8_t> cut(HEADER, HEADER + 40);
    expect(run(cut) == NrawResult::Corrupt, "Kopf abgeschnitten");

    expect(run(stream(0, 0)) == NrawResult::Corrupt, "kein Precinct nach dem SLH");
    expect(run(stream(100, 0x00)) == NrawResult::Corrupt, "Precinct der Länge 0");
    expect(run(stream(5000, 0xff)) == NrawResult::Corrupt, "Precinct länger als der Strom");

    std::vector<uint8_t> other = stream(100, 0x55);
    other[NC_OFFSET] = 3;
    expect(run(other) == NrawResult::Unsupported, "andere Komponentenzahl");

    expect(run(stream(100, 0x55), WIDTH * HEIGHT - 1) == NrawResult::Unsupported, "Zielpuffer passt nicht");

    // Überlange Paketlängen im ersten Precinct dürfen nie über den Puffer lesen.
    std::vector<uint8_t> big = stream(40, 0x00);
    size_t prec = sizeof HEADER + SLICE_SIZE;
    big[prec + 2] = 28; // Lprc: 12 + 28 = 40 Byte
    memset(&big[prec + 12], 0xff, 7);
    expect(run(big) == NrawResult::Corrupt, "Paketlängen über das Precinct hinaus");

    return failures == 0 ? 0 : 1;
}
