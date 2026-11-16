// VST3-Adapter: lädt ein VST3-Plugin und zeigt es Resolve als VST2-Plugin (AEffect).
//
// Resolve hat unter Linux keinen VST3-Host (VST3Host besteht nur aus leeren Rümpfen), der
// VST2-Host dagegen läuft mit dem Lader. Darum übersetzt der Adapter: Opcodes des
// VST2-Dispatchers auf IComponent/IAudioProcessor/IEditController, processReplacing auf
// IAudioProcessor::process, Parameter über ihren Index, Zustand als ein Chunk aus
// Komponenten- und Controller-Zustand, den Editor über IPlugView im X11-Fenster von Resolve.
//
// Grenzen wie bei VST2: nur der Haupt-Audiobus (Sidechain und weitere Busse bekommen Stille),
// keine MIDI-Ereignisse, keine Programmlisten, nur 32 Bit.
//
// Diese Datei: Aufbau, Parameter, Audio, Abbau. Die Befehle von Resolve stehen in
// vst3_dispatch.cpp, Host-Objekte in vst3_host.cpp, die Fabrik in vst3_module.cpp.

#include "vst3_adapter.h"

#include "log.h"

#include <algorithm>
#include <cstdio>

namespace vst3 {

namespace {

struct VstTimeInfo {
    double samplePos, sampleRate, nanoSeconds, ppqPos, tempo, barStartPos, cycleStartPos, cycleEndPos;
    int32_t timeSigNumerator, timeSigDenominator, smpteOffset, smpteFrameRate, samplesToNextClock, flags;
};
enum {
    kVstTransportPlaying = 1 << 1, kVstTransportCycleActive = 1 << 2, kVstTransportRecording = 1 << 3,
    kVstNanosValid = 1 << 8, kVstPpqPosValid = 1 << 9, kVstTempoValid = 1 << 10, kVstBarsValid = 1 << 11,
    kVstCyclePosValid = 1 << 12, kVstTimeSigValid = 1 << 13,
};

Adapter *adapterOf(AEffect *e)
{
    return static_cast<Adapter *>(e->object);
}

// Lebende Adapter, für die regelmäßige Arbeit im GUI-Thread (Werte an den Controller).
// Rekursiv: idle() ruft Resolve, Resolve kann darin effClose schicken (destroy).
std::recursive_mutex gAdaptersMutex;
std::vector<Adapter *> gAdapters;

int idleAll(void *)
{
    std::lock_guard lock(gAdaptersMutex);
    const std::vector<Adapter *> all = gAdapters;
    for (Adapter *a : all)
        if (std::find(gAdapters.begin(), gAdapters.end(), a) != gAdapters.end())
            a->idle();
    return 1;
}

} // namespace

tresult ComponentHandler::beginEdit(ParamID id)
{
    auto it = a_->indexOf.find(id);
    if (it != a_->indexOf.end())
        a_->call(audioMasterBeginEdit, it->second, 0);
    return kResultOk;
}

tresult ComponentHandler::performEdit(ParamID id, ParamValue value)
{
    auto it = a_->indexOf.find(id);
    a_->toProcessor.push(id, value);
    if (it != a_->indexOf.end()) {
        a_->values[size_t(it->second)].store(float(value));
        a_->call(audioMasterAutomate, it->second, 0, nullptr, float(value));
    }
    return kResultOk;
}

tresult ComponentHandler::endEdit(ParamID id)
{
    auto it = a_->indexOf.find(id);
    if (it != a_->indexOf.end())
        a_->call(audioMasterEndEdit, it->second, 0);
    return kResultOk;
}

// Nur vormerken, ausgewertet in idle() im GUI-Thread. Plugins rufen das mitten in setActive
// oder process auf, über yabridge auch aus anderen Threads; ein Rückruf ins Plugin oder an
// Resolve von hier aus blockierte Resolve (Neoverb: Latenz geändert während effMainsChanged).
tresult ComponentHandler::restartComponent(int32 flags)
{
    logMessage("VST3 %s: restartComponent %#x (%s)", a_->info.name.c_str(), unsigned(flags),
               a_->onMainThread() ? "GUI-Thread" : "anderer Thread");
    a_->restartFlags.fetch_or(flags);
    return kResultOk;
}

tresult Frame::resizeView(IPlugView *view, ViewRect *size)
{
    if (!view || !size)
        return kInvalidArgument;
    const int w = size->getWidth(), h = size->getHeight();
    a_->rect = {0, 0, int16_t(h), int16_t(w)};
    a_->call(audioMasterSizeWindow, w, h);
    view->onSize(size);
    return kResultTrue;
}

bool Adapter::init(IPluginFactory *factory, const TUID cid)
{
    if (factory->createInstance(reinterpret_cast<FIDString>(cid), IComponent::iid,
                                reinterpret_cast<void **>(&component)) != kResultOk
        || !component) {
        logMessage("  VST3: createInstance(IComponent) schlug fehl");
        component = nullptr;
        return false;
    }
    if (component->initialize(hostApp()) != kResultOk) {
        logMessage("  VST3: IComponent::initialize schlug fehl");
        component->release();
        component = nullptr;
        return false;
    }
    if (component->queryInterface(IAudioProcessor::iid, reinterpret_cast<void **>(&processor)) != kResultOk
        || !processor) {
        logMessage("  VST3: kein IAudioProcessor");
        processor = nullptr;
        return false;
    }
    if (component->queryInterface(IEditController::iid, reinterpret_cast<void **>(&controller)) != kResultOk
        || !controller) {
        controller = nullptr;
        TUID ccid;
        if (component->getControllerClassId(ccid) == kResultOk
            && factory->createInstance(reinterpret_cast<FIDString>(ccid), IEditController::iid,
                                       reinterpret_cast<void **>(&controller)) == kResultOk
            && controller) {
            if (controller->initialize(hostApp()) != kResultOk) {
                logMessage("  VST3: IEditController::initialize schlug fehl");
                controller->release();
                controller = nullptr;
            } else {
                separateController = true;
            }
        } else {
            controller = nullptr;
        }
    }
    if (separateController) {
        component->queryInterface(IConnectionPoint::iid, reinterpret_cast<void **>(&componentPoint));
        controller->queryInterface(IConnectionPoint::iid, reinterpret_cast<void **>(&controllerPoint));
        if (componentPoint && controllerPoint) {
            componentPoint->connect(controllerPoint);
            controllerPoint->connect(componentPoint);
        }
    }
    if (controller) {
        controller->setComponentHandler(&handler);
        auto *state = new MemoryStream;
        if (component->getState(state) == kResultOk) {
            state->seek(0, IBStream::kIBSeekSet, nullptr);
            controller->setComponentState(state);
        }
        state->release();
    } else {
        logMessage("  VST3: kein Edit-Controller, keine Parameter und kein Editor");
    }

    setupBuses();
    readParameters();

    fillEffect(cid);
    return true;
}

// VST2-Beschreibung des Plugins ausfüllen: Aufrufe, Anzahlen, Flags, ID, Version.
void Adapter::fillEffect(const TUID cid)
{
    effect.magic = kEffectMagic;
    effect.object = this;
    effect.dispatcher = [](AEffect *e, int op, int index, long value, void *ptr, float opt) -> long {
        return adapterOf(e)->dispatch(op, index, value, ptr, opt);
    };
    effect.processReplacing = [](AEffect *e, float **in, float **out, int32_t n) { adapterOf(e)->process(in, out, n); };
    effect.process = effect.processReplacing;
    effect.setParameter = [](AEffect *e, int32_t i, float v) { adapterOf(e)->setParameter(i, v); };
    effect.getParameter = [](AEffect *e, int32_t i) -> float {
        Adapter *a = adapterOf(e);
        return i >= 0 && size_t(i) < a->ids.size() ? a->values[size_t(i)].load() : 0.f;
    };
    effect.numPrograms = 1;
    effect.numParams = int32_t(ids.size());
    effect.flags = effFlagsCanReplacing | effFlagsProgramChunks | (controller ? effFlagsHasEditor : 0)
                   | (info.subCategories.find("Instrument") != std::string::npos ? effFlagsIsSynth : 0);
    effect.initialDelay = int32_t(processor->getLatencySamples());
    effect.ioRatio = 1.f;
    // VST2 kennt nur 32 Bit als ID: aus der Klassen-ID gefaltet (FNV-1a).
    uint32_t hash = 2166136261u;
    for (int i = 0; i < 16; ++i)
        hash = (hash ^ static_cast<unsigned char>(cid[i])) * 16777619u;
    effect.uniqueID = int32_t(hash);
    int major = 0, minor = 0, patch = 0;
    sscanf(info.version.c_str(), "%d.%d.%d", &major, &minor, &patch);
    effect.version = major * 1000 + minor * 100 + patch;
}

// Hauptbusse (Index 0) aktiv und nach Möglichkeit Stereo, Nebenbusse aus. VST2 sieht nur
// die Kanäle der Hauptbusse.
void Adapter::setupBuses()
{
    const int32 nIn = component->getBusCount(kAudio, kInput), nOut = component->getBusCount(kAudio, kOutput);
    inArr.assign(size_t(nIn), SpeakerArr::kEmpty);
    outArr.assign(size_t(nOut), SpeakerArr::kEmpty);
    for (int32 i = 0; i < nIn; ++i)
        processor->getBusArrangement(kInput, i, inArr[size_t(i)]);
    for (int32 i = 0; i < nOut; ++i)
        processor->getBusArrangement(kOutput, i, outArr[size_t(i)]);
    auto wanted = [](std::vector<SpeakerArrangement> v) {
        if (!v.empty())
            v[0] = SpeakerArr::kStereo;
        return v;
    };
    std::vector<SpeakerArrangement> in = wanted(inArr), out = wanted(outArr);
    if (processor->setBusArrangements(in.data(), nIn, out.data(), nOut) == kResultTrue) {
        inArr = in;
        outArr = out;
    } else {
        for (int32 i = 0; i < nIn; ++i)
            processor->getBusArrangement(kInput, i, inArr[size_t(i)]);
        for (int32 i = 0; i < nOut; ++i)
            processor->getBusArrangement(kOutput, i, outArr[size_t(i)]);
    }
    for (int32 i = 0; i < nIn; ++i)
        component->activateBus(kAudio, kInput, i, i == 0);
    for (int32 i = 0; i < nOut; ++i)
        component->activateBus(kAudio, kOutput, i, i == 0);
    if (component->getBusCount(kEvent, kInput) > 0)
        component->activateBus(kEvent, kInput, 0, false);
    effect.numInputs = nIn ? SpeakerArr::getChannelCount(inArr[0]) : 0;
    effect.numOutputs = nOut ? SpeakerArr::getChannelCount(outArr[0]) : 0;
    setupBuffers();
}

void Adapter::setupBuffers()
{
    const size_t frames = size_t(setup.maxSamplesPerBlock);
    silence.assign(frames, 0.f);
    trash.assign(frames, 0.f);
    size_t auxChannels = 0;
    for (size_t i = 1; i < inArr.size(); ++i)
        auxChannels += size_t(SpeakerArr::getChannelCount(inArr[i]));
    for (size_t i = 1; i < outArr.size(); ++i)
        auxChannels += size_t(SpeakerArr::getChannelCount(outArr[i]));
    auxPtrs.assign(auxChannels, nullptr);
    inBufs.assign(inArr.size(), AudioBusBuffers{});
    outBufs.assign(outArr.size(), AudioBusBuffers{});
    size_t next = 0;
    auto aux = [&](std::vector<AudioBusBuffers> &bufs, const std::vector<SpeakerArrangement> &arr, float *buffer) {
        for (size_t i = 1; i < bufs.size(); ++i) {
            bufs[i].numChannels = SpeakerArr::getChannelCount(arr[i]);
            bufs[i].channelBuffers32 = auxPtrs.data() + next;
            for (int32 c = 0; c < bufs[i].numChannels; ++c)
                auxPtrs[next++] = buffer;
        }
    };
    aux(inBufs, inArr, silence.data());
    aux(outBufs, outArr, trash.data());
    if (!inBufs.empty())
        inBufs[0].numChannels = effect.numInputs;
    if (!outBufs.empty())
        outBufs[0].numChannels = effect.numOutputs;
}

void Adapter::readParameters()
{
    ids.clear();
    indexOf.clear();
    const int32 n = controller ? controller->getParameterCount() : 0;
    for (int32 i = 0; i < n; ++i) {
        ParameterInfo p{};
        if (controller->getParameterInfo(i, p) != kResultOk)
            continue;
        indexOf[p.id] = int32(ids.size());
        if (p.flags & ParameterInfo::kIsBypass)
            bypassIndex = int32(ids.size());
        ids.push_back(p.id);
    }
    values.reset(new std::atomic<float>[ids.size() ? ids.size() : 1]);
    refreshValues();
}

void Adapter::refreshValues()
{
    for (size_t i = 0; i < ids.size(); ++i)
        values[i].store(float(controller->getParamNormalized(ids[i])));
}

void Adapter::setActive(bool on)
{
    if (on == active)
        return;
    if (on) {
        processor->setupProcessing(setup);
        setupBuffers();
        component->setActive(true);
        processor->setProcessing(true);
        effect.initialDelay = int32_t(processor->getLatencySamples());
    } else {
        processor->setProcessing(false);
        component->setActive(false);
    }
    active = on;
}

void Adapter::setParameter(int32 index, float value)
{
    if (index < 0 || size_t(index) >= ids.size())
        return;
    values[size_t(index)].store(value);
    toProcessor.push(ids[size_t(index)], value);
    if (onMainThread())
        controller->setParamNormalized(ids[size_t(index)], value);
    else
        toController.push(ids[size_t(index)], value);
}

// Zeitangaben von Resolve (Tempo, Takt, Position) für das Plugin übersetzen.
void Adapter::fillContext()
{
    context = {};
    context.sampleRate = setup.sampleRate;
    const long want = kVstNanosValid | kVstPpqPosValid | kVstTempoValid | kVstBarsValid | kVstCyclePosValid
                      | kVstTimeSigValid;
    if (auto *t = reinterpret_cast<VstTimeInfo *>(call(audioMasterGetTime, 0, want))) {
        context.projectTimeSamples = TSamples(t->samplePos);
        if (t->sampleRate > 0)
            context.sampleRate = t->sampleRate;
        if (t->flags & kVstTransportPlaying)
            context.state |= ProcessContext::kPlaying;
        if (t->flags & kVstTransportCycleActive)
            context.state |= ProcessContext::kCycleActive;
        if (t->flags & kVstTransportRecording)
            context.state |= ProcessContext::kRecording;
        if (t->flags & kVstNanosValid) {
            context.state |= ProcessContext::kSystemTimeValid;
            context.systemTime = int64(t->nanoSeconds);
        }
        if (t->flags & kVstPpqPosValid) {
            context.state |= ProcessContext::kProjectTimeMusicValid;
            context.projectTimeMusic = t->ppqPos;
        }
        if (t->flags & kVstTempoValid) {
            context.state |= ProcessContext::kTempoValid;
            context.tempo = t->tempo;
        }
        if (t->flags & kVstBarsValid) {
            context.state |= ProcessContext::kBarPositionValid;
            context.barPositionMusic = t->barStartPos;
        }
        if (t->flags & kVstCyclePosValid) {
            context.state |= ProcessContext::kCycleValid;
            context.cycleStartMusic = t->cycleStartPos;
            context.cycleEndMusic = t->cycleEndPos;
        }
        if (t->flags & kVstTimeSigValid) {
            context.state |= ProcessContext::kTimeSigValid;
            context.timeSigNumerator = t->timeSigNumerator;
            context.timeSigDenominator = t->timeSigDenominator;
        }
    }
}

void Adapter::process(float **in, float **out, int32 frames)
{
    if (!active || frames <= 0)
        return;
    inChanges.clear();
    outChanges.clear();
    toProcessor.drain([this](ParamID id, ParamValue v) { inChanges.add(id, v); }, false);

    fillContext();

    // Nebenbusse haben Puffer für setup.maxSamplesPerBlock; größere Blöcke ohne sie.
    const bool auxFits = frames <= setup.maxSamplesPerBlock;
    for (size_t i = 1; i < inBufs.size(); ++i)
        inBufs[i].numChannels = auxFits ? SpeakerArr::getChannelCount(inArr[i]) : 0;
    for (size_t i = 1; i < outBufs.size(); ++i)
        outBufs[i].numChannels = auxFits ? SpeakerArr::getChannelCount(outArr[i]) : 0;
    if (!inBufs.empty())
        inBufs[0].channelBuffers32 = in;
    if (!outBufs.empty())
        outBufs[0].channelBuffers32 = out;

    ProcessData data;
    data.processMode = setup.processMode;
    data.symbolicSampleSize = kSample32;
    data.numSamples = frames;
    data.numInputs = int32(inBufs.size());
    data.numOutputs = int32(outBufs.size());
    data.inputs = inBufs.empty() ? nullptr : inBufs.data();
    data.outputs = outBufs.empty() ? nullptr : outBufs.data();
    data.inputParameterChanges = &inChanges;
    data.outputParameterChanges = &outChanges;
    data.inputEvents = &events;
    data.outputEvents = &events;
    data.processContext = &context;
    processor->process(data);

    // Werte, die das Plugin selbst setzt (Anzeigen, Pegel): an Controller und Resolve.
    for (int32 i = 0, n = outChanges.getParameterCount(); i < n; ++i) {
        IParamValueQueue *q = outChanges.getParameterData(i);
        int32 offset;
        ParamValue v;
        if (q->getPointCount() > 0 && q->getPoint(q->getPointCount() - 1, offset, v) == kResultOk) {
            toController.push(q->getParameterId(), v, false);
            auto it = indexOf.find(q->getParameterId());
            if (it != indexOf.end())
                values[size_t(it->second)].store(float(v));
        }
    }
}

// Im GUI-Thread: Werte aus anderen Threads an den Controller geben.
// Rückrufe an Resolve stehen am Ende: Resolve kann darin das Plugin schließen (destroy).
void Adapter::idle()
{
    if (controller)
        toController.drain([this](ParamID id, ParamValue v) { controller->setParamNormalized(id, v); }, true);
    const int32 flags = restartFlags.exchange(0);
    if (!flags)
        return;
    if (flags & kParamValuesChanged)
        refreshValues();
    if (flags & kLatencyChanged) {
        effect.initialDelay = int32_t(processor->getLatencySamples());
        logMessage("VST3 %s: Latenz %d", info.name.c_str(), effect.initialDelay);
    }
    AEffect *e = &effect;
    audioMasterCallback m = master;
    if (flags & kParamValuesChanged)
        m(e, audioMasterUpdateDisplay, 0, 0, nullptr, 0);
    if (flags & kLatencyChanged)
        m(e, audioMasterIOChanged, 0, 0, nullptr, 0);
}

void Adapter::destroy()
{
    {
        std::lock_guard lock(gAdaptersMutex);
        for (size_t i = 0; i < gAdapters.size(); ++i)
            if (gAdapters[i] == this) {
                gAdapters.erase(gAdapters.begin() + long(i));
                break;
            }
    }
    closeEditor();
    setActive(false);
    if (componentPoint && controllerPoint) {
        componentPoint->disconnect(controllerPoint);
        controllerPoint->disconnect(componentPoint);
    }
    if (componentPoint)
        componentPoint->release();
    if (controllerPoint)
        controllerPoint->release();
    if (controller) {
        controller->setComponentHandler(nullptr);
        if (separateController)
            controller->terminate();
        controller->release();
    }
    if (processor)
        processor->release();
    if (component) {
        component->terminate();
        component->release();
    }
    delete this;
}

} // namespace vst3

using namespace vst3;

AEffect *vst3Load(const std::string &dir, audioMasterCallback master)
{
    TUID cid;
    const std::string bundle = vst3Bundle(dir);
    if (bundle.empty() || !parseId(readFile(dir + "/class"), cid)) {
        logMessage("  VST3: %s/bundle oder class fehlt oder ist ungültig", dir.c_str());
        return nullptr;
    }
    logMessage("  VST3 %s, Klasse %s", bundle.c_str(), hexId(cid).c_str());
    IPluginFactory *factory = factoryFor(bundle);
    if (!factory)
        return nullptr;
    auto *a = new Adapter;
    if (!classInfo(factory, cid, a->info)) {
        logMessage("  VST3: Klasse nicht in der Fabrik");
        delete a;
        return nullptr;
    }
    a->master = master;
    if (!a->init(factory, cid)) {
        // Teilweise erzeugte Objekte bleiben liegen: Manche Plugins vertragen das Aufräumen
        // nach einem gescheiterten Start schlecht (wie beim VST2-Lader).
        return nullptr;
    }
    logMessage("  VST3 %s (%s): %d Parameter, %d→%d Kanäle, %s, Latenz %d", a->info.name.c_str(),
               a->info.subCategories.c_str(), a->effect.numParams, a->effect.numInputs, a->effect.numOutputs,
               a->separateController ? "eigener Controller" : "ein Objekt", a->effect.initialDelay);
    {
        std::lock_guard lock(gAdaptersMutex);
        gAdapters.push_back(a);
        static bool timer = false;
        if (!timer && glib()) {
            glib()->timeoutAdd(0, 30, idleAll, nullptr, nullptr);
            timer = true;
        }
    }
    return &a->effect;
}
