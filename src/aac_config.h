// AAC-Angaben: AudioSpecificConfig bilden (aus Abtastrate und Kanalzahl) und aus den Zusatzdaten
// einer Spur lesen (esds-Atom), rein gerechnet ohne Resolve und Log (Hintergrund in aac.cpp).
// Eigene Datei, damit tests/aac-test.cpp sie ohne den Rest der Bibliothek baut.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// AudioSpecificConfig für AAC-LC (ISO 14496-3, 1.6.2.1): 5 Bit Objekttyp 2, 4 Bit Index der
// Abtastrate, 4 Bit Kanalkonfiguration, 3 Bit 0.
inline bool aacLcConfig(int sampleRate, int channels, uint8_t out[2])
{
    static const int rates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
    int rateIndex = -1;
    for (int i = 0; i < int(sizeof rates / sizeof *rates); ++i)
        if (rates[i] == sampleRate)
            rateIndex = i;
    const int config = channels >= 1 && channels <= 6 ? channels : channels == 8 ? 7 : -1;
    if (rateIndex < 0 || config < 0)
        return false;
    out[0] = uint8_t(2 << 3 | rateIndex >> 1);
    out[1] = uint8_t((rateIndex & 1) << 7 | config << 3);
    return true;
}

// Länge eines MPEG-4-Deskriptors (bis zu vier Bytes, je sieben Bit).
inline bool descriptorLength(const uint8_t *&p, const uint8_t *end, size_t *len)
{
    *len = 0;
    for (int i = 0; i < 4; ++i) {
        if (p >= end)
            return false;
        uint8_t b = *p++;
        *len = (*len << 7) | (b & 0x7f);
        if (!(b & 0x80))
            return *len <= size_t(end - p);
    }
    return false;
}

// Sucht in ES_Descriptor (0x03) → DecoderConfigDescriptor (0x04) die DecoderSpecificInfo
// (0x05), die AudioSpecificConfig.
inline bool findSpecificInfo(const uint8_t *p, const uint8_t *end, const uint8_t **info, size_t *infoLen, int *objectType)
{
    while (p < end) {
        uint8_t tag = *p++;
        size_t len;
        if (!descriptorLength(p, end, &len))
            return false;
        const uint8_t *body = p, *bodyEnd = p + len;
        if (tag == 0x03 && len >= 3) {
            uint8_t flags = body[2];
            body += 3;
            if (flags & 0x80)
                body += 2;
            if ((flags & 0x40) && body < bodyEnd)
                body += 1 + *body;
            if (flags & 0x20)
                body += 2;
            if (body <= bodyEnd && findSpecificInfo(body, bodyEnd, info, infoLen, objectType))
                return true;
        } else if (tag == 0x04 && len >= 13) {
            *objectType = body[0];
            if (findSpecificInfo(body + 13, bodyEnd, info, infoLen, objectType))
                return true;
        } else if (tag == 0x05) {
            *info = body;
            *infoLen = len;
            return true;
        }
        p = bodyEnd;
    }
    return false;
}

// AudioSpecificConfig aus den Zusatzdaten der Spur: ein esds-Atom (mit oder ohne Kopf) oder
// schon die AudioSpecificConfig selbst.
inline bool audioSpecificConfig(const uint8_t *p, size_t n, const uint8_t **asc, size_t *ascLen, int *objectType)
{
    if (n >= 12 && memcmp(p + 4, "esds", 4) == 0)
        return findSpecificInfo(p + 12, p + n, asc, ascLen, objectType);
    if (n >= 5 && p[0] == 0 && p[4] == 0x03)
        return findSpecificInfo(p + 4, p + n, asc, ascLen, objectType);
    if (n >= 1 && p[0] == 0x03)
        return findSpecificInfo(p, p + n, asc, ascLen, objectType);
    if (n >= 2 && n <= 64) {
        *asc = p;
        *ascLen = n;
        return true;
    }
    return false;
}
