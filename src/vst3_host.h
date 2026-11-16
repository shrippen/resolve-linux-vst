// Gemeinsames der VST3-Dateien: Hilfen, Referenzzählung und die Host-Objekte, die der
// Adapter dem Plugin gibt. Interner Header, nur für src/vst3*.cpp.
#pragma once

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace vst3 {

// --- Hilfen -------------------------------------------------------------------------

template<class T>
bool is(const TUID iid)
{
    return FUnknownPrivate::iidEqual(iid, T::iid);
}

std::string utf8(const char16 *s, size_t max = 255);
void utf16(const char *s, char16 *out, size_t size);
void copyString(void *dest, const std::string &s, size_t size);
std::string readFile(const std::string &path);
std::string hexId(const TUID id);
bool parseId(const std::string &hex, TUID id);

// Referenzzählung für Objekte, die der Host an das Plugin gibt und das Plugin freigibt.
template<class... I>
class Counted : public I... {
public:
    virtual ~Counted() = default;
    uint32 PLUGIN_API addRef() override { return ++refs_; }
    uint32 PLUGIN_API release() override
    {
        uint32 r = --refs_;
        if (r == 0)
            delete this;
        return r;
    }

private:
    std::atomic<uint32> refs_{1};
};

// Objekte, die einem Adapter gehören und mit ihm enden: Referenzen zählen nicht.
#define FIXED_REFCOUNT                                    \
    uint32 PLUGIN_API addRef() override { return 1000; } \
    uint32 PLUGIN_API release() override { return 1000; }

// --- Host-Objekte für das Plugin ----------------------------------------------------

class MemoryStream : public Counted<IBStream> {
public:
    MemoryStream() = default;
    MemoryStream(const char *data, size_t size) : data_(data, data + size) {}
    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) override
    {
        if (is<FUnknown>(iid) || is<IBStream>(iid)) {
            addRef();
            *obj = static_cast<IBStream *>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    tresult PLUGIN_API read(void *buffer, int32 numBytes, int32 *numBytesRead) override
    {
        int64 n = std::max<int64>(0, std::min<int64>(numBytes, int64(data_.size()) - pos_));
        if (n > 0)
            memcpy(buffer, data_.data() + pos_, size_t(n));
        pos_ += n;
        if (numBytesRead)
            *numBytesRead = int32(n);
        return kResultOk;
    }
    tresult PLUGIN_API write(void *buffer, int32 numBytes, int32 *numBytesWritten) override
    {
        if (numBytes < 0)
            return kInvalidArgument;
        if (pos_ + numBytes > int64(data_.size()))
            data_.resize(size_t(pos_ + numBytes));
        memcpy(data_.data() + pos_, buffer, size_t(numBytes));
        pos_ += numBytes;
        if (numBytesWritten)
            *numBytesWritten = numBytes;
        return kResultOk;
    }
    tresult PLUGIN_API seek(int64 pos, int32 mode, int64 *result) override
    {
        int64 base = mode == kIBSeekCur ? pos_ : mode == kIBSeekEnd ? int64(data_.size()) : 0;
        if (base + pos < 0)
            return kInvalidArgument;
        pos_ = base + pos;
        if (result)
            *result = pos_;
        return kResultOk;
    }
    tresult PLUGIN_API tell(int64 *pos) override
    {
        if (pos)
            *pos = pos_;
        return kResultOk;
    }
    const std::vector<char> &data() const { return data_; }

private:
    std::vector<char> data_;
    int64 pos_ = 0;
};

// Parameteränderungen für einen Block, fester Speicher (process läuft im Audio-Thread).
class ParamQueue : public IParamValueQueue {
public:
    FIXED_REFCOUNT
    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) override
    {
        if (is<FUnknown>(iid) || is<IParamValueQueue>(iid)) {
            *obj = static_cast<IParamValueQueue *>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    ParamID PLUGIN_API getParameterId() override { return id; }
    int32 PLUGIN_API getPointCount() override { return count; }
    tresult PLUGIN_API getPoint(int32 index, int32 &sampleOffset, ParamValue &value) override
    {
        if (index < 0 || index >= count)
            return kResultFalse;
        sampleOffset = points[index].offset;
        value = points[index].value;
        return kResultOk;
    }
    tresult PLUGIN_API addPoint(int32 sampleOffset, ParamValue value, int32 &index) override
    {
        // Gleicher Zeitpunkt: Wert ersetzen; sonst anhängen, solange Platz ist.
        for (int32 i = 0; i < count; ++i)
            if (points[i].offset == sampleOffset) {
                points[i].value = value;
                index = i;
                return kResultOk;
            }
        if (count == kMaxPoints) {
            points[count - 1] = {sampleOffset, value};
            index = count - 1;
            return kResultOk;
        }
        points[count] = {sampleOffset, value};
        index = count++;
        return kResultOk;
    }

    static constexpr int32 kMaxPoints = 16;
    struct Point {
        int32 offset;
        ParamValue value;
    };
    ParamID id = 0;
    int32 count = 0;
    Point points[kMaxPoints];
};

class ParamChanges : public IParameterChanges {
public:
    FIXED_REFCOUNT
    explicit ParamChanges(size_t capacity = 256) : queues_(capacity) {}
    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) override
    {
        if (is<FUnknown>(iid) || is<IParameterChanges>(iid)) {
            *obj = static_cast<IParameterChanges *>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    int32 PLUGIN_API getParameterCount() override { return used_; }
    IParamValueQueue *PLUGIN_API getParameterData(int32 index) override
    {
        return index >= 0 && index < used_ ? &queues_[size_t(index)] : nullptr;
    }
    IParamValueQueue *PLUGIN_API addParameterData(const ParamID &id, int32 &index) override
    {
        for (int32 i = 0; i < used_; ++i)
            if (queues_[size_t(i)].id == id) {
                index = i;
                return &queues_[size_t(i)];
            }
        if (size_t(used_) == queues_.size())
            return nullptr;
        ParamQueue &q = queues_[size_t(used_)];
        q.id = id;
        q.count = 0;
        index = used_++;
        return &q;
    }
    void clear() { used_ = 0; }
    void add(ParamID id, ParamValue value)
    {
        int32 index;
        if (IParamValueQueue *q = addParameterData(id, index))
            q->addPoint(0, value, index);
    }

private:
    std::vector<ParamQueue> queues_;
    int32 used_ = 0;
};

class EmptyEvents : public IEventList {
public:
    FIXED_REFCOUNT
    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) override
    {
        if (is<FUnknown>(iid) || is<IEventList>(iid)) {
            *obj = static_cast<IEventList *>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    int32 PLUGIN_API getEventCount() override { return 0; }
    tresult PLUGIN_API getEvent(int32, Event &) override { return kResultFalse; }
    tresult PLUGIN_API addEvent(Event &) override { return kResultOk; }
};

// --- IRunLoop über glib ---------------------------------------------------------------

struct Glib {
    using SourceFunc = int (*)(void *);
    using FdFunc = int (*)(int, unsigned, void *);
    using Destroy = void (*)(void *);
    unsigned (*timeoutAdd)(int, unsigned, SourceFunc, void *, Destroy);
    unsigned (*fdAdd)(int, int, unsigned, FdFunc, void *, Destroy);
    int (*sourceRemove)(unsigned);
};

Glib *glib();
IHostApplication *hostApp();
Linux::IRunLoop *hostRunLoop(); // nullptr, wenn es kein glib gibt

} // namespace vst3
