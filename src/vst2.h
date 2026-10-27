// VST2-Schnittstelle, soweit Lader und VST3-Adapter sie brauchen (Aufbau wie aeffect.h,
// eigene Deklarationen). Gemeinsam für resolve_linux_vst.cpp und vst3.cpp.
#pragma once

#include <cstdint>
#include <string>

struct AEffect;
using audioMasterCallback = long (*)(AEffect *, int, int, long, void *, float);
using Dispatcher = long (*)(AEffect *, int, int, long, void *, float);
using VstEntry = AEffect *(*)(audioMasterCallback);

struct AEffect {
    int32_t magic;
    Dispatcher dispatcher;
    void (*process)(AEffect *, float **, float **, int32_t);
    void (*setParameter)(AEffect *, int32_t, float);
    float (*getParameter)(AEffect *, int32_t);
    int32_t numPrograms, numParams, numInputs, numOutputs;
    int32_t flags;
    intptr_t resvd1, resvd2;
    int32_t initialDelay;
    int32_t realQualities, offQualities;
    float ioRatio;
    void *object, *user;
    int32_t uniqueID, version;
    void (*processReplacing)(AEffect *, float **, float **, int32_t);
    void (*processDoubleReplacing)(AEffect *, double **, double **, int32_t);
    char future[56];
};

struct ERect {
    int16_t top, left, bottom, right;
};

constexpr int32_t kEffectMagic = 0x56737450; // 'VstP'

// Log von resolve_linux_vst.cpp (bzw. stderr im Werkzeug vst3-classes).
void logMessage(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// VST3-Adapter (vst3.cpp): Ein VST3-Plugin erscheint als VST2-Plugin. `dir` ist ein
// Ordner Name.vst mit den Dateien `bundle` (Symlink auf das .vst3-Bundle) und `class`
// (Klassen-ID, 32 Hexziffern), angelegt von resolve-linux-vst.
bool isVst3Entry(const std::string &dir);
std::string vst3Bundle(const std::string &dir);
AEffect *vst3Load(const std::string &dir, audioMasterCallback master);
// Gibt die Effektklassen eines Bundles aus, je Zeile: ID, Name, Unterkategorien (Tab).
int vst3ListClasses(const std::string &bundle);
