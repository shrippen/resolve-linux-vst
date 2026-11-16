// VST3-Adapter, Befehle von Resolve: Dispatcher (Opcodes), Editor und Chunk (Zustand).

#include "pluginterfaces/gui/iplugviewcontentscalesupport.h"

#include "vst3_adapter.h"
#include "vst3_params.h"

#include "log.h"

#include <climits>
#include <cstdio>

namespace vst3 {

namespace {

constexpr int32_t kChunkMagic = 0x52335653; // 'R3VS'

// effGetPlugCategory: VST2-Kategorie aus den Unterkategorien der VST3-Klasse.
long plugCategory(const std::string &s)
{
    auto has = [&s](const char *what) { return s.find(what) != std::string::npos; };
    if (has("Instrument"))
        return kPlugCategSynth;
    if (has("Analyzer"))
        return kPlugCategAnalysis;
    if (has("Mastering"))
        return kPlugCategMastering;
    if (has("Spatial"))
        return kPlugCategSpacializer;
    if (has("Reverb"))
        return kPlugCategRoomFx;
    if (has("Restoration"))
        return kPlugCategRestoration;
    if (has("Generator"))
        return kPlugCategGenerator;
    return kPlugCategEffect;
}

} // namespace

long Adapter::editorRect(ERect **out)
{
    if (!out || !controller)
        return 0;
    if (!view)
        view = controller->createView(ViewType::kEditor);
    if (!view)
        return 0;
    ViewRect r;
    if (view->getSize(&r) == kResultOk)
        rect = {0, 0, int16_t(r.getHeight()), int16_t(r.getWidth())};
    *out = &rect;
    return 1;
}

long Adapter::openEditor(void *window)
{
    if (!controller)
        return 0;
    if (!view)
        view = controller->createView(ViewType::kEditor);
    if (!view || view->isPlatformTypeSupported(kPlatformTypeX11EmbedWindowID) != kResultTrue) {
        logMessage("VST3 %s: kein X11-Editor", info.name.c_str());
        return 0;
    }
    view->setFrame(&frame);
    if (contentScale > 0) {
        IPlugViewContentScaleSupport *scale = nullptr;
        if (view->queryInterface(IPlugViewContentScaleSupport::iid, reinterpret_cast<void **>(&scale)) == kResultOk
            && scale) {
            tresult r = scale->setContentScaleFactor(contentScale);
            logMessage("VST3 %s: Inhaltsskalierung %.2f (Antwort %d)", info.name.c_str(), contentScale, int(r));
            scale->release();
        } else {
            logMessage("VST3 %s: keine Inhaltsskalierung (IPlugViewContentScaleSupport fehlt)", info.name.c_str());
        }
    }
    if (view->attached(window, kPlatformTypeX11EmbedWindowID) != kResultOk) {
        logMessage("VST3 %s: IPlugView::attached schlug fehl", info.name.c_str());
        view->setFrame(nullptr);
        view->release();
        view = nullptr;
        return 0;
    }
    ViewRect r;
    if (view->getSize(&r) == kResultOk)
        rect = {0, 0, int16_t(r.getHeight()), int16_t(r.getWidth())};
    return 1;
}

void Adapter::closeEditor()
{
    if (!view)
        return;
    view->removed();
    view->setFrame(nullptr);
    view->release();
    view = nullptr;
}

// Chunk: 'R3VS', Version 1, Länge und Daten des Komponentenzustands, dann des Controllers.
long Adapter::getChunk(void **out)
{
    auto *comp = new MemoryStream, *ctrl = new MemoryStream;
    component->getState(comp);
    if (controller)
        controller->getState(ctrl);
    auto put = [this](int32_t v) {
        chunk.insert(chunk.end(), reinterpret_cast<char *>(&v), reinterpret_cast<char *>(&v) + 4);
    };
    chunk.clear();
    put(kChunkMagic);
    put(1);
    put(int32_t(comp->data().size()));
    chunk.insert(chunk.end(), comp->data().begin(), comp->data().end());
    put(int32_t(ctrl->data().size()));
    chunk.insert(chunk.end(), ctrl->data().begin(), ctrl->data().end());
    comp->release();
    ctrl->release();
    *out = chunk.data();
    return long(chunk.size());
}

long Adapter::setChunk(const char *data, long size)
{
    auto get = [&](long pos, int32_t &v) {
        if (pos + 4 > size)
            return false;
        memcpy(&v, data + pos, 4);
        return true;
    };
    int32_t magic, version, compSize, ctrlSize;
    if (!get(0, magic) || magic != kChunkMagic || !get(4, version) || !get(8, compSize) || compSize < 0
        || 12 + long(compSize) + 4 > size || !get(12 + compSize, ctrlSize) || ctrlSize < 0
        || 16 + long(compSize) + ctrlSize > size) {
        logMessage("VST3 %s: unbekannter Chunk (%ld Bytes)", info.name.c_str(), size);
        return 0;
    }
    auto *comp = new MemoryStream(data + 12, size_t(compSize));
    component->setState(comp);
    if (controller) {
        comp->seek(0, IBStream::kIBSeekSet, nullptr);
        controller->setComponentState(comp);
        auto *ctrl = new MemoryStream(data + 16 + compSize, size_t(ctrlSize));
        controller->setState(ctrl);
        ctrl->release();
        refreshValues();
    }
    comp->release();
    return 1;
}

// effSetSpeakerArrangement.
long Adapter::setSpeakers(long value, void *ptr)
{
    // value: Eingang, ptr: Ausgang (VstSpeakerArrangement, numChannels an Byte 4).
    if (active || !value || !ptr)
        return 0;
    auto channels = [](const void *arr) { return reinterpret_cast<const int32_t *>(arr)[1]; };
    auto arrangement = [](int32_t c) -> SpeakerArrangement {
        switch (c) {
        case 1: return SpeakerArr::kMono;
        case 2: return SpeakerArr::kStereo;
        case 6: return SpeakerArr::k51;
        case 8: return SpeakerArr::k71Cine;
        default: return SpeakerArr::kEmpty;
        }
    };
    std::vector<SpeakerArrangement> in = inArr, out = outArr;
    if (!in.empty())
        in[0] = arrangement(channels(reinterpret_cast<void *>(value)));
    if (!out.empty())
        out[0] = arrangement(channels(ptr));
    if (processor->setBusArrangements(in.data(), int32(in.size()), out.data(), int32(out.size())) != kResultTrue)
        return 0;
    inArr = in;
    outArr = out;
    effect.numInputs = in.empty() ? 0 : SpeakerArr::getChannelCount(in[0]);
    effect.numOutputs = out.empty() ? 0 : SpeakerArr::getChannelCount(out[0]);
    setupBuffers();
    return 1;
}

long Adapter::dispatch(int opcode, int index, long value, void *ptr, float opt)
{
    auto paramInfo = [this](int index, ParameterInfo &p) {
        p = {};
        return controller && index >= 0 && size_t(index) < ids.size()
               && controller->getParameterInfo(index, p) == kResultOk;
    };
    ParameterInfo p;
    switch (opcode) {
    case effOpen:
        return 0;
    case effClose:
        destroy();
        return 1;
    case effGetProgram:
        return 0;
    case effGetProgramName:
        copyString(ptr, "Default", 24);
        return 1;
    case effGetProgramNameIndexed:
        if (index != 0)
            return 0;
        copyString(ptr, "Default", 24);
        return 1;
    case effGetParamLabel:
        if (!paramInfo(index, p))
            return 0;
        copyString(ptr, utf8(p.units, 7), 8);
        return 1;
    case effGetParamDisplay: {
        if (!controller || index < 0 || size_t(index) >= ids.size())
            return 0;
        String128 s{};
        if (controller->getParamStringByValue(ids[size_t(index)], values[size_t(index)].load(), s) != kResultOk)
            return 0;
        copyString(ptr, utf8(s, 23), 24);
        return 1;
    }
    case effGetParamName:
        if (!paramInfo(index, p))
            return 0;
        copyString(ptr, utf8(p.title, 31), 32);
        return 1;
    case effCanBeAutomated:
        return paramInfo(index, p) && (p.flags & ParameterInfo::kCanAutomate) ? 1 : 0;
    case effString2Parameter: {
        if (!ptr || !controller || index < 0 || size_t(index) >= ids.size())
            return 0;
        String128 s;
        utf16(static_cast<const char *>(ptr), s, 128);
        ParamValue v;
        if (controller->getParamValueByString(ids[size_t(index)], s, v) != kResultOk)
            return 0;
        setParameter(index, float(v));
        return 1;
    }
    case effGetParameterProperties:
        if (!ptr || !paramInfo(index, p))
            return 0;
        return paramProperties(p, ptr);
    case effSetSampleRate:
    case effSetBlockSize: {
        const bool was = active;
        setActive(false);
        if (opcode == effSetSampleRate)
            setup.sampleRate = opt;
        else if (value > 0)
            setup.maxSamplesPerBlock = int32(value);
        setActive(was);
        return 1;
    }
    case effMainsChanged:
        logMessage("VST3 %s: effMainsChanged %ld (%s)", info.name.c_str(), value,
                   onMainThread() ? "GUI-Thread" : "anderer Thread");
        setActive(value != 0);
        logMessage("VST3 %s: effMainsChanged %ld fertig", info.name.c_str(), value);
        return 1;
    case effEditGetRect:
        return editorRect(static_cast<ERect **>(ptr));
    case effEditOpen:
        return openEditor(ptr);
    case effEditClose:
        closeEditor();
        return 1;
    case effEditIdle:
        idle();
        return 0;
    case effGetChunk:
        return ptr ? getChunk(static_cast<void **>(ptr)) : 0;
    case effSetChunk:
        return ptr ? setChunk(static_cast<const char *>(ptr), value) : 0;
    case effGetInputProperties:
    case effGetOutputProperties: {
        const int channels = opcode == effGetInputProperties ? effect.numInputs : effect.numOutputs;
        if (!ptr || index < 0 || index >= channels)
            return 0;
        auto *pin = static_cast<VstPinProperties *>(ptr);
        memset(pin, 0, sizeof *pin);
        snprintf(pin->label, sizeof pin->label, "%s %d", opcode == effGetInputProperties ? "In" : "Out", index + 1);
        pin->flags = kVstPinIsActive | (channels == 2 ? kVstPinIsStereo : 0);
        pin->arrangementType = channels == 1 ? kSpeakerArrMono : channels == 2 ? kSpeakerArrStereo : kSpeakerArrUserDefined;
        return 1;
    }
    case effGetPlugCategory:
        return plugCategory(info.subCategories);
    case effSetSpeakerArrangement:
        return setSpeakers(value, ptr);
    case effSetBypass:
        if (bypassIndex < 0)
            return 0;
        setParameter(bypassIndex, value ? 1.f : 0.f);
        return 1;
    case effGetEffectName:
        copyString(ptr, info.name, 32);
        return 1;
    case effGetVendorString:
        copyString(ptr, info.vendor, 64);
        return 1;
    case effGetProductString:
        copyString(ptr, info.name, 64);
        return 1;
    case effGetVendorVersion:
        return effect.version;
    case effVendorSpecific:
        if (index == kPreS && value == kAeCs) {
            contentScale = opt;
            return 1;
        }
        return 0;
    case effCanDo: {
        const char *what = static_cast<const char *>(ptr);
        if (!what)
            return 0;
        if (!strcmp(what, "receiveVstTimeInfo") || !strcmp(what, "plugAsChannelInsert") || !strcmp(what, "plugAsSend"))
            return 1;
        if (!strcmp(what, "bypass"))
            return bypassIndex >= 0 ? 1 : 0;
        return 0;
    }
    case effGetTailSize: {
        uint32 tail = processor->getTailSamples();
        if (tail == kInfiniteTail)
            return INT_MAX;
        return tail == kNoTail ? 1 : long(tail);
    }
    case effGetVstVersion:
        return kVstVersion;
    case effSetProcessPrecision:
        return value == 0 ? 1 : 0; // nur 32 Bit
    default:
        return 0;
    }
}

} // namespace vst3
