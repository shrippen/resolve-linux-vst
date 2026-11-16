// Abbildung eines VST3-Parameters auf die VST2-Eigenschaften (effGetParameterProperties).
// Eigene Datei, damit tests/ sie ohne Resolve prüfen kann. Interner Header, nur für src/vst3*.cpp.
#pragma once

#include "pluginterfaces/vst/ivsteditcontroller.h"

#include "vst3_host.h"

namespace vst3 {

// Füllt das VstParameterProperties unter `ptr` aus `p`; gibt 1 zurück (Befehl beantwortet).
long paramProperties(const ParameterInfo &p, void *ptr);

} // namespace vst3
