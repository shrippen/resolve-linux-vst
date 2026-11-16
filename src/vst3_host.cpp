// VST3-Host-Objekte: was der Adapter dem Plugin als Host gibt (IHostApplication, IMessage,
// IAttributeList) und IRunLoop über glib. Hilfen für Zeichenketten und Klassen-IDs.
//
// IRunLoop (Linux-Editoren brauchen ihn für Timer und Dateideskriptoren) läuft über glib:
// Resolves Qt nutzt die glib-Ereignisschleife, Rückrufe kommen so im GUI-Thread an.

#include "vst3_host.h"

#include "log.h"

#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>

namespace vst3 {

// UTF-16 (String128) nach UTF-8, höchstens `max` Bytes ohne NUL, nicht mitten im Zeichen.
std::string utf8(const char16 *s, size_t max)
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

// --- Host-Objekte für das Plugin ----------------------------------------------------

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

namespace {

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

// --- IRunLoop über glib ---------------------------------------------------------------

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

} // namespace

IHostApplication *hostApp()
{
    return &gHost;
}

Linux::IRunLoop *hostRunLoop()
{
    return glib() ? &gRunLoop : nullptr;
}

} // namespace vst3
