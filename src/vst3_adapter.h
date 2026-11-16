// Der VST3-Adapter als Klasse: ein geladenes Plugin, das Resolve als VST2-Plugin sieht.
// Die Methoden stehen in vst3.cpp (Aufbau, Audio, Abbau) und vst3_dispatch.cpp (Befehle von
// Resolve). Interner Header, nur für src/vst3*.cpp.
#pragma once

#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"

#include "vst2.h"
#include "vst3_host.h"
#include "vst3_module.h"

#include <pthread.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace vst3 {

// Werteübergabe zwischen Threads: kurz gesperrt, der Audio-Thread wartet nie (try_lock).
class Pending {
public:
    // Im Audio-Thread mit wait = false: Ist gesperrt, geht der Wert verloren (es folgt bald
    // ein neuer).
    void push(ParamID id, ParamValue value, bool wait = true)
    {
        std::unique_lock lock(mutex_, std::defer_lock);
        if (wait)
            lock.lock();
        else if (!lock.try_lock())
            return;
        for (auto &e : entries_)
            if (e.first == id) {
                e.second = value;
                return;
            }
        if (entries_.size() < 4096)
            entries_.emplace_back(id, value);
    }
    template<class F>
    void drain(F &&f, bool wait)
    {
        std::unique_lock lock(mutex_, std::defer_lock);
        if (wait)
            lock.lock();
        else if (!lock.try_lock())
            return;
        for (auto &e : entries_)
            f(e.first, e.second);
        entries_.clear();
    }

private:
    std::mutex mutex_;
    std::vector<std::pair<ParamID, ParamValue>> entries_;
};

class Adapter;

class ComponentHandler : public IComponentHandler {
public:
    FIXED_REFCOUNT
    explicit ComponentHandler(Adapter *a) : a_(a) {}
    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) override
    {
        if (is<FUnknown>(iid) || is<IComponentHandler>(iid)) {
            *obj = static_cast<IComponentHandler *>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    tresult PLUGIN_API beginEdit(ParamID id) override;
    tresult PLUGIN_API performEdit(ParamID id, ParamValue value) override;
    tresult PLUGIN_API endEdit(ParamID id) override;
    tresult PLUGIN_API restartComponent(int32 flags) override;

private:
    Adapter *a_;
};

class Frame : public IPlugFrame {
public:
    FIXED_REFCOUNT
    explicit Frame(Adapter *a) : a_(a) {}
    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) override
    {
        if (is<FUnknown>(iid) || is<IPlugFrame>(iid)) {
            *obj = static_cast<IPlugFrame *>(this);
            return kResultOk;
        }
        if (is<Linux::IRunLoop>(iid) && hostRunLoop()) {
            *obj = hostRunLoop();
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    tresult PLUGIN_API resizeView(IPlugView *view, ViewRect *size) override;

private:
    Adapter *a_;
};

class Adapter {
public:
    AEffect effect{};
    audioMasterCallback master = nullptr;
    ClassInfo info;
    IComponent *component = nullptr;
    IAudioProcessor *processor = nullptr;
    IEditController *controller = nullptr;
    IConnectionPoint *componentPoint = nullptr, *controllerPoint = nullptr;
    bool separateController = false;
    pthread_t mainThread = pthread_self();

    std::vector<ParamID> ids;
    std::map<ParamID, int32> indexOf;
    std::unique_ptr<std::atomic<float>[]> values;
    int32 bypassIndex = -1;
    Pending toProcessor, toController;
    std::atomic<int32> restartFlags{0}; // aus restartComponent, für idle()

    std::vector<SpeakerArrangement> inArr, outArr;
    std::vector<AudioBusBuffers> inBufs, outBufs;
    std::vector<float> silence, trash;
    std::vector<float *> auxPtrs; // Kanalzeiger für Nebenbusse
    ProcessSetup setup{kRealtime, kSample32, 1024, 48000.0};
    bool active = false;
    ParamChanges inChanges, outChanges;
    EmptyEvents events;
    ProcessContext context{};

    IPlugView *view = nullptr;
    ERect rect{};
    float contentScale = 0;
    std::vector<char> chunk;

    ComponentHandler handler{this};
    Frame frame{this};

    long call(int opcode, int index, long value, void *ptr = nullptr, float opt = 0)
    {
        return master ? master(&effect, opcode, index, value, ptr, opt) : 0;
    }
    bool onMainThread() const { return pthread_equal(pthread_self(), mainThread); }

    bool init(IPluginFactory *factory, const TUID cid);
    void fillEffect(const TUID cid);
    void setupBuses();
    void setupBuffers();
    void readParameters();
    void refreshValues();
    void setActive(bool on);
    void fillContext();
    void process(float **in, float **out, int32 frames);
    void setParameter(int32 index, float value);
    void idle();
    long dispatch(int opcode, int index, long value, void *ptr, float opt);
    long setSpeakers(long value, void *ptr);
    long editorRect(ERect **out);
    long openEditor(void *window);
    void closeEditor();
    long getChunk(void **out);
    long setChunk(const char *data, long size);
    void destroy();

private:
    void findController(IPluginFactory *factory);
};

} // namespace vst3
