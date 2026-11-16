// VST2-Schnittstelle, soweit Lader und VST3-Adapter sie brauchen (Aufbau wie aeffect.h,
// eigene Deklarationen). Gemeinsam für resolve_linux_vst.cpp und den VST3-Adapter (vst3*.cpp).
#pragma once

#include <cstddef>
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

// VST2-Konstanten (Opcodes, Flags), wie in aeffectx.h.
enum {
    effOpen = 0, effClose = 1, effSetProgram = 2, effGetProgram = 3, effGetProgramName = 5,
    effGetParamLabel = 6, effGetParamDisplay = 7, effGetParamName = 8, effSetSampleRate = 10,
    effSetBlockSize = 11, effMainsChanged = 12, effEditGetRect = 13, effEditOpen = 14,
    effEditClose = 15, effEditIdle = 19, effGetChunk = 23, effSetChunk = 24, effCanBeAutomated = 26,
    effString2Parameter = 27, effGetProgramNameIndexed = 29, effGetInputProperties = 33,
    effGetOutputProperties = 34, effGetPlugCategory = 35, effSetSpeakerArrangement = 42,
    effSetBypass = 44, effGetEffectName = 45, effGetVendorString = 47, effGetProductString = 48,
    effGetVendorVersion = 49, effVendorSpecific = 50, effCanDo = 51, effGetTailSize = 52,
    effGetParameterProperties = 56, effGetVstVersion = 58, effSetProcessPrecision = 77,
};
enum {
    audioMasterAutomate = 0, audioMasterVersion = 1, audioMasterGetTime = 7, audioMasterIOChanged = 13,
    audioMasterSizeWindow = 15, audioMasterUpdateDisplay = 42, audioMasterBeginEdit = 43,
    audioMasterEndEdit = 44,
};
enum {
    effFlagsHasEditor = 1, effFlagsCanReplacing = 1 << 4, effFlagsProgramChunks = 1 << 5,
    effFlagsIsSynth = 1 << 8,
};
// VST-Version, die Host (audioMasterVersion) und Plugin (effGetVstVersion) melden: 2.4.
constexpr int kVstVersion = 2400;

// Zeitangaben des Hosts (audioMasterGetTime): Tempo, Takt, Position.
struct VstTimeInfo {
    double samplePos, sampleRate, nanoSeconds, ppqPos, tempo, barStartPos, cycleStartPos, cycleEndPos;
    int32_t timeSigNumerator, timeSigDenominator, smpteOffset, smpteFrameRate, samplesToNextClock, flags;
};
enum {
    kVstTransportPlaying = 1 << 1, kVstTransportCycleActive = 1 << 2, kVstTransportRecording = 1 << 3,
    kVstNanosValid = 1 << 8, kVstPpqPosValid = 1 << 9, kVstTempoValid = 1 << 10, kVstBarsValid = 1 << 11,
    kVstCyclePosValid = 1 << 12, kVstTimeSigValid = 1 << 13,
};
static_assert(sizeof(VstTimeInfo) == 88, "VstTimeInfo");
static_assert(offsetof(VstTimeInfo, timeSigNumerator) == 64, "VstTimeInfo");
static_assert(offsetof(VstTimeInfo, flags) == 84, "VstTimeInfo");

// Ein- und Ausgänge (effGetInputProperties, effGetOutputProperties).
struct VstPinProperties {
    char label[64];
    int32_t flags, arrangementType;
    char shortLabel[8];
    char future[48];
};
enum { kVstPinIsActive = 1 << 0, kVstPinIsStereo = 1 << 1 };
enum { kSpeakerArrUserDefined = -2, kSpeakerArrEmpty = -1, kSpeakerArrMono = 0, kSpeakerArrStereo = 1 };
static_assert(sizeof(VstPinProperties) == 128, "VstPinProperties");
static_assert(offsetof(VstPinProperties, arrangementType) == 68, "VstPinProperties");

// Angaben zu einem Parameter (effGetParameterProperties).
struct VstParameterProperties {
    float stepFloat, smallStepFloat, largeStepFloat;
    char label[64];
    int32_t flags, minInteger, maxInteger, stepInteger, largeStepInteger;
    char shortLabel[8];
    int16_t displayIndex, category, numParametersInCategory, reserved;
    char categoryLabel[24];
    char future[16];
};
enum {
    kVstParameterIsSwitch = 1 << 0, kVstParameterUsesIntegerMinMax = 1 << 1,
    kVstParameterUsesFloatStep = 1 << 2, kVstParameterUsesIntStep = 1 << 3,
    kVstParameterSupportsDisplayIndex = 1 << 4,
};
static_assert(sizeof(VstParameterProperties) == 152, "VstParameterProperties");
static_assert(offsetof(VstParameterProperties, flags) == 76, "VstParameterProperties");
static_assert(offsetof(VstParameterProperties, displayIndex) == 104, "VstParameterProperties");

// Kategorien eines Plugins (effGetPlugCategory).
enum {
    kPlugCategEffect = 1, kPlugCategSynth = 2, kPlugCategAnalysis = 3, kPlugCategMastering = 4,
    kPlugCategSpacializer = 5, kPlugCategRoomFx = 6, kPlugCategRestoration = 8, kPlugCategGenerator = 11,
};

constexpr int kPreS = 0x50726553; // 'PreS'
constexpr long kAeCs = 0x41654373; // 'AeCs'

// VST3-Adapter (vst3*.cpp): Ein VST3-Plugin erscheint als VST2-Plugin. `dir` ist ein
// Ordner Name.vst mit den Dateien `bundle` (Symlink auf das .vst3-Bundle) und `class`
// (Klassen-ID, 32 Hexziffern), angelegt von resolve-linux-vst.
bool isVst3Entry(const std::string &dir);
std::string vst3Bundle(const std::string &dir);
AEffect *vst3Load(const std::string &dir, audioMasterCallback master);
// Gibt die Effektklassen eines Bundles aus, je Zeile: ID, Name, Unterkategorien (Tab).
int vst3ListClasses(const std::string &bundle);
