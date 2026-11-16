// Interner Header der VST3-Module: Fabrik eines Bundles und ihre Klassen.
#pragma once

#include "pluginterfaces/base/ipluginbase.h"

#include "vst3_host.h"

#include <string>
#include <vector>

namespace vst3 {

struct ClassInfo {
    std::string name, vendor, version, subCategories;
};

// Eine Klasse der Fabrik mit allen Angaben, die Lader und Liste brauchen.
struct FactoryClass { TUID cid; std::string category; ClassInfo info; };

IPluginFactory *factoryFor(const std::string &bundle);
std::vector<FactoryClass> factoryClasses(IPluginFactory *factory);
bool classInfo(IPluginFactory *factory, const TUID cid, ClassInfo &out);

} // namespace vst3
