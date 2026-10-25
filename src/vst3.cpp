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
// IRunLoop (Linux-Editoren brauchen ihn für Timer und Dateideskriptoren) läuft über glib:
// Resolves Qt nutzt die glib-Ereignisschleife, Rückrufe kommen so im GUI-Thread an.

#define INIT_CLASS_IID
#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/gui/iplugviewcontentscalesupport.h"
#include "pluginterfaces/vst/ivstattributes.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/vstspeaker.h"

#include "vst2.h"

#include <dirent.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

// --- Hilfen -------------------------------------------------------------------------

template<class T>
bool is(const TUID iid)
{
    return FUnknownPrivate::iidEqual(iid, T::iid);
}

// UTF-16 (String128) nach UTF-8, höchstens `max` Bytes ohne NUL, nicht mitten im Zeichen.
std::string utf8(const char16 *s, size_t max = 255)
{
    std::string out;
    for (; s && *s; ++s) {
        uint32_t c = *s;
        if (c >= 0xD800 && c < 0xDC00 && s[1] >= 0xDC00 && s[1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (s[1] - 0xDC00);
            ++s;
        }
        char b[4];
        size_t n;
        if (c < 0x80) {
            b[0] = char(c);
            n = 1;
        } else if (c < 0x800) {
            b[0] = char(0xC0 | (c >> 6));
            b[1] = char(0x80 | (c & 0x3F));
            n = 2;
        } else if (c < 0x10000) {
            b[0] = char(0xE0 | (c >> 12));
            b[1] = char(0x80 | ((c >> 6) & 0x3F));
            b[2] = char(0x80 | (c & 0x3F));
            n = 3;
        } else {
            b[0] = char(0xF0 | (c >> 18));
            b[1] = char(0x80 | ((c >> 12) & 0x3F));
            b[2] = char(0x80 | ((c >> 6) & 0x3F));
            b[3] = char(0x80 | (c & 0x3F));
            n = 4;
        }
        if (out.size() + n > max)
            break;
        out.append(b, n);
    }
    return out;
}

// UTF-8 nach UTF-16 (nur BMP, reicht für Eingaben wie Parameterwerte).
void utf16(const char *s, char16 *out, size_t size)
{
    size_t i = 0;
    auto *p = reinterpret_cast<const unsigned char *>(s);
    while (*p && i + 1 < size) {
        uint32_t c = *p++;
        if (c >= 0xE0 && p[0] && p[1]) {
            c = ((c & 0x0F) << 12) | ((p[0] & 0x3F) << 6) | (p[1] & 0x3F);
            p += 2;
        } else if (c >= 0xC0 && p[0]) {
            c = ((c & 0x1F) << 6) | (p[0] & 0x3F);
            p += 1;
        }
        out[i++] = char16(c);
    }
    out[i] = 0;
}

// Kopiert höchstens `size - 1` Bytes und schließt mit NUL ab.
void copyString(void *dest, const std::string &s, size_t size)
{
    size_t n = s.size() < size ? s.size() : size - 1;
    memcpy(dest, s.data(), n);
    static_cast<char *>(dest)[n] = 0;
}

std::string readFile(const std::string &path)
{
    std::string out;
    if (FILE *f = fopen(path.c_str(), "r")) {
        char buf[256];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0)
            out.append(buf, n);
        fclose(f);
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == ' '))
        out.pop_back();
    return out;
}

std::string hexId(const TUID id)
{
    std::string out;
    char b[3];
    for (int i = 0; i < 16; ++i) {
        snprintf(b, sizeof b, "%02X", static_cast<unsigned char>(id[i]));
        out += b;
    }
    return out;
}

bool parseId(const std::string &hex, TUID id)
{
    if (hex.size() != 32)
        return false;
    for (int i = 0; i < 16; ++i) {
        char *end;
        const std::string byte = hex.substr(i * 2, 2);
        id[i] = char(strtoul(byte.c_str(), &end, 16));
        if (*end)
            return false;
    }
    return true;
}

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

class Attributes : public Counted<IAttributeList> {
public:
    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) override
    {
        if (is<FUnknown>(iid) || is<IAttributeList>(iid)) {
            addRef();
            *obj = static_cast<IAttributeList *>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    tresult PLUGIN_API setInt(AttrID id, int64 value) override
    {
        ints_[id] = value;
        return kResultOk;
    }
    tresult PLUGIN_API getInt(AttrID id, int64 &value) override
    {
        auto it = ints_.find(id);
        if (it == ints_.end())
            return kResultFalse;
        value = it->second;
        return kResultOk;
    }
    tresult PLUGIN_API setFloat(AttrID id, double value) override
    {
        floats_[id] = value;
        return kResultOk;
    }
    tresult PLUGIN_API getFloat(AttrID id, double &value) override
    {
        auto it = floats_.find(id);
        if (it == floats_.end())
            return kResultFalse;
        value = it->second;
        return kResultOk;
    }
    tresult PLUGIN_API setString(AttrID id, const TChar *string) override
    {
        std::u16string &s = strings_[id];
        s.clear();
        for (; string && *string; ++string)
            s += char16_t(*string);
        return kResultOk;
    }
    tresult PLUGIN_API getString(AttrID id, TChar *string, uint32 sizeInBytes) override
    {
        auto it = strings_.find(id);
        if (it == strings_.end() || sizeInBytes < sizeof(TChar))
            return kResultFalse;
        size_t n = std::min<size_t>(it->second.size(), sizeInBytes / sizeof(TChar) - 1);
        memcpy(string, it->second.data(), n * sizeof(TChar));
        string[n] = 0;
        return kResultOk;
    }
    tresult PLUGIN_API setBinary(AttrID id, const void *data, uint32 size) override
    {
        auto *p = static_cast<const char *>(data);
        binaries_[id].assign(p, p + size);
        return kResultOk;
    }
    tresult PLUGIN_API getBinary(AttrID id, const void *&data, uint32 &size) override
    {
        auto it = binaries_.find(id);
        if (it == binaries_.end())
            return kResultFalse;
        data = it->second.data();
        size = uint32(it->second.size());
        return kResultOk;
    }

private:
    std::map<std::string, int64> ints_;
    std::map<std::string, double> floats_;
    std::map<std::string, std::u16string> strings_;
    std::map<std::string, std::vector<char>> binaries_;
};

class Message : public Counted<IMessage> {
public:
    ~Message() override { attributes_->release(); }
    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) override
    {
        if (is<FUnknown>(iid) || is<IMessage>(iid)) {
            addRef();
            *obj = static_cast<IMessage *>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    FIDString PLUGIN_API getMessageID() override { return id_.c_str(); }
    void PLUGIN_API setMessageID(FIDString id) override { id_ = id ? id : ""; }
    IAttributeList *PLUGIN_API getAttributes() override { return attributes_; }

private:
    std::string id_;
    Attributes *attributes_ = new Attributes;
};

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

// Aus dem Prozess (Resolve lädt glib über Qt); nullptr, wenn es keins gibt.
Glib *glib()
{
    static Glib g;
    static const bool ok = [] {
        g.timeoutAdd = reinterpret_cast<decltype(g.timeoutAdd)>(dlsym(RTLD_DEFAULT, "g_timeout_add_full"));
        g.fdAdd = reinterpret_cast<decltype(g.fdAdd)>(dlsym(RTLD_DEFAULT, "g_unix_fd_add_full"));
        g.sourceRemove = reinterpret_cast<decltype(g.sourceRemove)>(dlsym(RTLD_DEFAULT, "g_source_remove"));
        if (!g.timeoutAdd || !g.fdAdd || !g.sourceRemove)
            logMessage("VST3: kein glib im Prozess, Editoren ohne IRunLoop");
        return g.timeoutAdd && g.fdAdd && g.sourceRemove;
    }();
    return ok ? &g : nullptr;
}

class RunLoop : public Linux::IRunLoop {
public:
    FIXED_REFCOUNT
    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) override
    {
        if (is<FUnknown>(iid) || is<Linux::IRunLoop>(iid)) {
            *obj = static_cast<Linux::IRunLoop *>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    tresult PLUGIN_API registerEventHandler(Linux::IEventHandler *handler, Linux::FileDescriptor fd) override
    {
        Glib *g = glib();
        if (!g || !handler)
            return kResultFalse;
        unsigned tag = g->fdAdd(0, fd, 1 /* G_IO_IN */, onFd, handler, nullptr);
        logMessage("VST3: IRunLoop Dateideskriptor %d", fd);
        std::lock_guard lock(mutex_);
        sources_.push_back({handler, tag});
        return kResultOk;
    }
    tresult PLUGIN_API unregisterEventHandler(Linux::IEventHandler *handler) override { return remove(handler); }
    tresult PLUGIN_API registerTimer(Linux::ITimerHandler *handler, Linux::TimerInterval ms) override
    {
        Glib *g = glib();
        if (!g || !handler)
            return kResultFalse;
        unsigned tag = g->timeoutAdd(0, unsigned(ms ? ms : 1), onTimer, handler, nullptr);
        logMessage("VST3: IRunLoop Timer %llu ms", static_cast<unsigned long long>(ms));
        std::lock_guard lock(mutex_);
        sources_.push_back({handler, tag});
        return kResultOk;
    }
    tresult PLUGIN_API unregisterTimer(Linux::ITimerHandler *handler) override { return remove(handler); }

private:
    static int onFd(int fd, unsigned, void *handler)
    {
        static_cast<Linux::IEventHandler *>(handler)->onFDIsSet(fd);
        return 1; // G_SOURCE_CONTINUE; entfernt wird über unregister
    }
    static int onTimer(void *handler)
    {
        static_cast<Linux::ITimerHandler *>(handler)->onTimer();
        return 1;
    }
    tresult remove(void *handler)
    {
        std::vector<unsigned> tags;
        {
            std::lock_guard lock(mutex_);
            for (size_t i = 0; i < sources_.size();)
                if (sources_[i].handler == handler) {
                    tags.push_back(sources_[i].tag);
                    sources_.erase(sources_.begin() + long(i));
                } else {
                    ++i;
                }
        }
        for (unsigned tag : tags)
            glib()->sourceRemove(tag);
        return kResultOk;
    }

    struct Source {
        void *handler;
        unsigned tag;
    };
    std::mutex mutex_;
    std::vector<Source> sources_;
};

RunLoop gRunLoop;

class HostApplication : public IHostApplication {
public:
    FIXED_REFCOUNT
    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) override
    {
        if (is<FUnknown>(iid) || is<IHostApplication>(iid)) {
            *obj = static_cast<IHostApplication *>(this);
            return kResultOk;
        }
        if (is<Linux::IRunLoop>(iid) && glib()) {
            *obj = static_cast<Linux::IRunLoop *>(&gRunLoop);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    tresult PLUGIN_API getName(String128 name) override
    {
        utf16("DaVinci Resolve", name, 128);
        return kResultOk;
    }
    tresult PLUGIN_API createInstance(TUID cid, TUID iid, void **obj) override
    {
        if (is<IMessage>(cid) && is<IMessage>(iid)) {
            *obj = static_cast<IMessage *>(new Message);
            return kResultOk;
        }
        if (is<IAttributeList>(cid) && is<IAttributeList>(iid)) {
            *obj = static_cast<IAttributeList *>(new Attributes);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
};

HostApplication gHost;

// --- Module ---------------------------------------------------------------------------

// Die .so eines Bundles: Contents/x86_64-linux/*.so, bei alten Plugins die Datei selbst.
std::string moduleFile(const std::string &bundle)
{
    struct stat st;
    if (stat(bundle.c_str(), &st) == 0 && S_ISREG(st.st_mode))
        return bundle;
    const std::string dir = bundle + "/Contents/x86_64-linux";
    std::string name = bundle.substr(bundle.rfind('/') + 1);
    if (name.size() > 5 && name.compare(name.size() - 5, 5, ".vst3") == 0)
        name.resize(name.size() - 5);
    if (access((dir + "/" + name + ".so").c_str(), R_OK) == 0)
        return dir + "/" + name + ".so";
    std::string found;
    if (DIR *d = opendir(dir.c_str())) {
        while (dirent *e = readdir(d)) {
            std::string n = e->d_name;
            if (n.size() > 3 && n.compare(n.size() - 3, 3, ".so") == 0) {
                found = dir + "/" + n;
                break;
            }
        }
        closedir(d);
    }
    return found;
}

// Geöffnete Module bleiben geladen (wie beim VST2-Lader: kein dlclose).
IPluginFactory *factoryFor(const std::string &bundle)
{
    static std::mutex mutex;
    static std::map<std::string, IPluginFactory *> factories;
    std::lock_guard lock(mutex);
    auto it = factories.find(bundle);
    if (it != factories.end())
        return it->second;

    const std::string file = moduleFile(bundle);
    if (file.empty()) {
        logMessage("  VST3: keine .so in %s", bundle.c_str());
        return nullptr;
    }
    void *handle = dlopen(file.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        logMessage("  VST3: dlopen: %s", dlerror());
        return nullptr;
    }
    using Entry = bool (*)(void *);
    using GetFactory = IPluginFactory *(*)();
    if (auto entry = reinterpret_cast<Entry>(dlsym(handle, "ModuleEntry")); entry && !entry(handle)) {
        logMessage("  VST3: ModuleEntry von %s schlug fehl", file.c_str());
        return nullptr;
    }
    auto get = reinterpret_cast<GetFactory>(dlsym(handle, "GetPluginFactory"));
    IPluginFactory *factory = get ? get() : nullptr;
    if (!factory) {
        logMessage("  VST3: kein GetPluginFactory in %s", file.c_str());
        return nullptr;
    }
    IPluginFactory3 *f3 = nullptr;
    if (factory->queryInterface(IPluginFactory3::iid, reinterpret_cast<void **>(&f3)) == kResultOk && f3) {
        f3->setHostContext(&gHost);
        f3->release();
    }
    factories[bundle] = factory;
    return factory;
}

struct ClassInfo {
    std::string name, vendor, version, subCategories;
};

// Angaben zu einer Klasse; false, wenn die Fabrik sie nicht kennt.
bool classInfo(IPluginFactory *factory, const TUID cid, ClassInfo &out)
{
    PFactoryInfo fi{};
    factory->getFactoryInfo(&fi);
    IPluginFactory2 *f2 = nullptr;
    factory->queryInterface(IPluginFactory2::iid, reinterpret_cast<void **>(&f2));
    bool found = false;
    for (int32 i = 0, n = factory->countClasses(); i < n && !found; ++i) {
        PClassInfo2 c2{};
        if (f2 && f2->getClassInfo2(i, &c2) == kResultOk) {
            if (!FUnknownPrivate::iidEqual(c2.cid, cid))
                continue;
            out = {c2.name, *c2.vendor ? c2.vendor : fi.vendor, c2.version, c2.subCategories};
            found = true;
        } else {
            PClassInfo c{};
            if (factory->getClassInfo(i, &c) != kResultOk || !FUnknownPrivate::iidEqual(c.cid, cid))
                continue;
            out = {c.name, fi.vendor, "", ""};
            found = true;
        }
    }
    if (f2)
        f2->release();
    return found;
}

// --- Adapter --------------------------------------------------------------------------

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
    audioMasterAutomate = 0, audioMasterGetTime = 7, audioMasterIOChanged = 13,
    audioMasterSizeWindow = 15, audioMasterUpdateDisplay = 42, audioMasterBeginEdit = 43,
    audioMasterEndEdit = 44,
};
enum {
    effFlagsHasEditor = 1, effFlagsCanReplacing = 1 << 4, effFlagsProgramChunks = 1 << 5,
    effFlagsIsSynth = 1 << 8,
};
constexpr int kPreS = 0x50726553; // 'PreS'
constexpr long kAeCs = 0x41654373; // 'AeCs'
constexpr int32_t kChunkMagic = 0x52335653; // 'R3VS'

struct VstTimeInfo {
    double samplePos, sampleRate, nanoSeconds, ppqPos, tempo, barStartPos, cycleStartPos, cycleEndPos;
    int32_t timeSigNumerator, timeSigDenominator, smpteOffset, smpteFrameRate, samplesToNextClock, flags;
};
enum {
    kVstTransportPlaying = 1 << 1, kVstTransportCycleActive = 1 << 2, kVstTransportRecording = 1 << 3,
    kVstNanosValid = 1 << 8, kVstPpqPosValid = 1 << 9, kVstTempoValid = 1 << 10, kVstBarsValid = 1 << 11,
    kVstCyclePosValid = 1 << 12, kVstTimeSigValid = 1 << 13,
};

struct VstPinProperties {
    char label[64];
    int32_t flags, arrangementType;
    char shortLabel[8];
    char future[48];
};
struct VstParameterProperties {
    float stepFloat, smallStepFloat, largeStepFloat;
    char label[64];
    int32_t flags, minInteger, maxInteger, stepInteger, largeStepInteger;
    char shortLabel[8];
    int16_t displayIndex, category, numParametersInCategory, reserved;
    char categoryLabel[24];
    char future[16];
};

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
        if (is<Linux::IRunLoop>(iid) && glib()) {
            *obj = static_cast<Linux::IRunLoop *>(&gRunLoop);
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
    void setupBuses();
    void setupBuffers();
    void readParameters();
    void refreshValues();
    void setActive(bool on);
    void process(float **in, float **out, int32 frames);
    void setParameter(int32 index, float value);
    void idle();
    long dispatch(int opcode, int index, long value, void *ptr, float opt);
    long editorRect(ERect **out);
    long openEditor(void *window);
    void closeEditor();
    long getChunk(void **out);
    long setChunk(const char *data, long size);
    void destroy();
};

Adapter *adapterOf(AEffect *e)
{
    return static_cast<Adapter *>(e->object);
}

// Lebende Adapter, für die regelmäßige Arbeit im GUI-Thread (Werte an den Controller).
std::mutex gAdaptersMutex;
std::vector<Adapter *> gAdapters;

int idleAll(void *)
{
    std::lock_guard lock(gAdaptersMutex);
    for (Adapter *a : gAdapters)
        a->idle();
    return 1;
}

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

tresult ComponentHandler::restartComponent(int32 flags)
{
    logMessage("VST3 %s: restartComponent %#x", a_->info.name.c_str(), unsigned(flags));
    if (flags & kParamValuesChanged) {
        a_->refreshValues();
        a_->call(audioMasterUpdateDisplay, 0, 0);
    }
    if (flags & kLatencyChanged) {
        a_->effect.initialDelay = int32_t(a_->processor->getLatencySamples());
        a_->call(audioMasterIOChanged, 0, 0);
    }
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
    if (component->initialize(&gHost) != kResultOk) {
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
            if (controller->initialize(&gHost) != kResultOk) {
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
    return true;
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

void Adapter::process(float **in, float **out, int32 frames)
{
    if (!active || frames <= 0)
        return;
    inChanges.clear();
    outChanges.clear();
    toProcessor.drain([this](ParamID id, ParamValue v) { inChanges.add(id, v); }, false);

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
void Adapter::idle()
{
    if (controller)
        toController.drain([this](ParamID id, ParamValue v) { controller->setParamNormalized(id, v); }, true);
}

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
    case effGetParameterProperties: {
        if (!ptr || !paramInfo(index, p))
            return 0;
        auto *props = static_cast<VstParameterProperties *>(ptr);
        memset(props, 0, sizeof *props);
        copyString(props->label, utf8(p.title, 63), 64);
        copyString(props->shortLabel, utf8(p.shortTitle, 7), 8);
        if (p.stepCount == 1) {
            props->flags = 1; // kVstParameterIsSwitch
        } else if (p.stepCount > 1) {
            props->flags = 4 | 16; // kVstParameterUsesIntegerMinMax | kVstParameterUsesIntStep
            props->maxInteger = p.stepCount;
            props->stepInteger = props->largeStepInteger = 1;
        }
        return 1;
    }
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
        setActive(value != 0);
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
        pin->flags = 1 | (channels == 2 ? 2 : 0); // kVstPinIsActive | kVstPinIsStereo
        pin->arrangementType = channels == 1 ? 0 : channels == 2 ? 1 : -2; // Mono, Stereo, unbekannt
        return 1;
    }
    case effGetPlugCategory: {
        const std::string &s = info.subCategories;
        auto has = [&s](const char *what) { return s.find(what) != std::string::npos; };
        if (has("Instrument"))
            return 2; // kPlugCategSynth
        if (has("Analyzer"))
            return 3;
        if (has("Mastering"))
            return 4;
        if (has("Spatial"))
            return 5;
        if (has("Reverb"))
            return 6;
        if (has("Restoration"))
            return 8;
        if (has("Generator"))
            return 11;
        return 1; // kPlugCategEffect
    }
    case effSetSpeakerArrangement: {
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
        return 2400;
    case effSetProcessPrecision:
        return value == 0 ? 1 : 0; // nur 32 Bit
    default:
        return 0;
    }
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

} // namespace

bool isVst3Entry(const std::string &dir)
{
    return access((dir + "/class").c_str(), R_OK) == 0;
}

std::string vst3Bundle(const std::string &dir)
{
    char *real = realpath((dir + "/bundle").c_str(), nullptr);
    if (!real)
        return {};
    std::string out = real;
    free(real);
    return out;
}

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

int vst3ListClasses(const std::string &bundle)
{
    IPluginFactory *factory = factoryFor(bundle);
    if (!factory)
        return 1;
    IPluginFactory2 *f2 = nullptr;
    factory->queryInterface(IPluginFactory2::iid, reinterpret_cast<void **>(&f2));
    for (int32 i = 0, n = factory->countClasses(); i < n; ++i) {
        PClassInfo2 c2{};
        PClassInfo c{};
        const char *category, *name, *sub = "";
        const char *cid;
        if (f2 && f2->getClassInfo2(i, &c2) == kResultOk) {
            category = c2.category;
            name = c2.name;
            sub = c2.subCategories;
            cid = c2.cid;
        } else if (factory->getClassInfo(i, &c) == kResultOk) {
            category = c.category;
            name = c.name;
            cid = c.cid;
        } else {
            continue;
        }
        if (strcmp(category, kVstAudioEffectClass) != 0)
            continue;
        TUID id;
        memcpy(id, cid, sizeof id);
        printf("%s\t%s\t%s\n", hexId(id).c_str(), name, sub);
    }
    if (f2)
        f2->release();
    fflush(stdout);
    return 0;
}
