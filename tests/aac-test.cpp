// Test der AAC-Angaben (src/aac_config.h): AudioSpecificConfig aus Abtastrate und Kanälen und
// das Lesen aus esds-Atom, ES-Descriptor oder nackter Config. Erwartungen sind die Werte, die
// aac.cpp vor dem Auslagern lieferte. Läuft ohne Resolve (make test).
#include "../src/aac_config.h"

#include <cstdio>
#include <cstring>

namespace {

int failures = 0;

void expect(bool ok, const char *what)
{
    printf("%s  %s\n", ok ? "ok   " : "FEHLER", what);
    failures += !ok;
}

bool config(int rate, int channels, uint8_t b0, uint8_t b1)
{
    uint8_t out[2] = {0, 0};
    return aacLcConfig(rate, channels, out) && out[0] == b0 && out[1] == b1;
}

bool refused(int rate, int channels)
{
    uint8_t out[2];
    return !aacLcConfig(rate, channels, out);
}

constexpr uint8_t kAsc[] = {0x11, 0x90}; // 48 kHz, Stereo

// ES_Descriptor (0x03) → DecoderConfigDescriptor (0x04, 13 Byte, Objekttyp 0x40) →
// DecoderSpecificInfo (0x05) mit kAsc.
constexpr uint8_t kEs[] = {0x03, 0x16, 0x00, 0x00, 0x00, 0x04, 0x11, 0x40, 0x15, 0x00, 0x00, 0x00, 0x00, 0x00,
                           0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x02, 0x11, 0x90};

bool reads(const uint8_t *p, size_t n, int wantType)
{
    const uint8_t *asc = nullptr;
    size_t len = 0;
    int type = 0;
    return audioSpecificConfig(p, n, &asc, &len, &type) && len == sizeof kAsc && memcmp(asc, kAsc, len) == 0
           && type == wantType;
}

} // namespace

int main()
{
    expect(config(48000, 2, 0x11, 0x90), "48 kHz Stereo = 11 90");
    expect(config(44100, 2, 0x12, 0x10), "44,1 kHz Stereo = 12 10");
    expect(config(96000, 1, 0x10, 0x08), "96 kHz Mono = 10 08");
    expect(config(7350, 6, 0x16, 0x30), "7,35 kHz 5.1 = 16 30");
    expect(config(48000, 8, 0x11, 0xb8), "48 kHz 8 Kanäle = Konfiguration 7");
    expect(refused(48000, 7), "7 Kanäle abgelehnt");
    expect(refused(48000, 0), "0 Kanäle abgelehnt");
    expect(refused(47000, 2), "unbekannte Rate abgelehnt");

    expect(reads(kEs, sizeof kEs, 0x40), "ES_Descriptor ohne Kopf");

    uint8_t headed[4 + sizeof kEs];
    memset(headed, 0, 4);
    memcpy(headed + 4, kEs, sizeof kEs);
    expect(reads(headed, sizeof headed, 0x40), "ES_Descriptor mit vier Byte davor");

    uint8_t esds[12 + sizeof kEs] = {0, 0, 0, 0, 'e', 's', 'd', 's'};
    memcpy(esds + 12, kEs, sizeof kEs);
    expect(reads(esds, sizeof esds, 0x40), "esds-Atom");

    expect(reads(kAsc, sizeof kAsc, 0), "nackte Config (2 Byte)");
    expect(!reads(kAsc, 1, 0), "ein Byte reicht nicht");

    const uint8_t truncated[] = {0x03, 0x7f, 0x00};
    const uint8_t *asc;
    size_t len;
    int type = 0;
    expect(!audioSpecificConfig(truncated, sizeof truncated, &asc, &len, &type), "Descriptor länger als Daten");

    printf("%s\n", failures ? "FEHLGESCHLAGEN" : "alles ok");
    return failures ? 1 : 0;
}
