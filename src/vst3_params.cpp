// VST3-Adapter: VST3-Parameter als VST2-Parameter-Eigenschaften.

#include "vst3_params.h"

#include "vst2.h"

namespace vst3 {

// effGetParameterProperties: Name, Kurzname und Stufen eines Parameters im VST2-Format.
long paramProperties(const ParameterInfo &p, void *ptr)
{
    auto *props = static_cast<VstParameterProperties *>(ptr);
    memset(props, 0, sizeof *props);
    copyString(props->label, utf8(p.title, 63), 64);
    copyString(props->shortLabel, utf8(p.shortTitle, 7), 8);
    if (p.stepCount == 1) {
        props->flags = kVstParameterIsSwitch;
    } else if (p.stepCount > 1) {
        // Ein VST2-Host liest die Ganzzahl-Felder nur mit diesen Flags; Werte 0..stepCount.
        props->flags = kVstParameterUsesIntegerMinMax | kVstParameterUsesIntStep;
        props->minInteger = 0;
        props->maxInteger = p.stepCount;
        props->stepInteger = props->largeStepInteger = 1;
    }
    return 1;
}

} // namespace vst3
