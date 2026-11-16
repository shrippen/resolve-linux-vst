// VST3-Module: öffnet die .so eines Bundles, holt die Fabrik und liest ihre Klassen.
// Dazu die Einstiegspunkte, die ohne Adapter auskommen (Eintrag erkennen, Bundle, Klassenliste).

#include "pluginterfaces/vst/ivstaudioprocessor.h"

#include "vst3_module.h"

#include "log.h"
#include "vst2.h"

#include <dirent.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>

namespace vst3 {

// --- Module ---------------------------------------------------------------------------

namespace {

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

} // namespace

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
        f3->setHostContext(hostApp());
        f3->release();
    }
    factories[bundle] = factory;
    return factory;
}

// PClassInfo: Hersteller dann aus der Fabrik, Version und Unterkategorien leer.
std::vector<FactoryClass> factoryClasses(IPluginFactory *factory)
{
    std::vector<FactoryClass> out;
    PFactoryInfo fi{};
    factory->getFactoryInfo(&fi);
    IPluginFactory2 *f2 = nullptr;
    factory->queryInterface(IPluginFactory2::iid, reinterpret_cast<void **>(&f2));
    for (int32 i = 0, n = factory->countClasses(); i < n; ++i) {
        FactoryClass fc{};
        PClassInfo2 c2{};
        PClassInfo c{};
        if (f2 && f2->getClassInfo2(i, &c2) == kResultOk) {
            memcpy(fc.cid, c2.cid, sizeof fc.cid);
            fc.category = c2.category;
            fc.info = {c2.name, *c2.vendor ? c2.vendor : fi.vendor, c2.version, c2.subCategories};
        } else if (factory->getClassInfo(i, &c) == kResultOk) {
            memcpy(fc.cid, c.cid, sizeof fc.cid);
            fc.category = c.category;
            fc.info = {c.name, fi.vendor, "", ""};
        } else {
            continue;
        }
        out.push_back(fc);
    }
    if (f2) {
        f2->release();
    }
    return out;
}

// Angaben zu einer Klasse; false, wenn die Fabrik sie nicht kennt.
bool classInfo(IPluginFactory *factory, const TUID cid, ClassInfo &out)
{
    for (const FactoryClass &fc : factoryClasses(factory)) {
        if (FUnknownPrivate::iidEqual(fc.cid, cid)) {
            out = fc.info;
            return true;
        }
    }
    return false;
}

} // namespace vst3

using namespace vst3;

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

int vst3ListClasses(const std::string &bundle)
{
    IPluginFactory *factory = factoryFor(bundle);
    if (!factory)
        return 1;
    for (const FactoryClass &fc : factoryClasses(factory)) {
        if (fc.category != kVstAudioEffectClass) {
            continue;
        }
        printf("%s\t%s\t%s\n", hexId(fc.cid).c_str(), fc.info.name.c_str(), fc.info.subCategories.c_str());
    }
    fflush(stdout);
    return 0;
}
