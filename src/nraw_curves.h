// N-RAW-Kennlinien je fastticolib-Kontext, rein, damit tests/ es ausführen kann.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

using NrawCurveValues = std::vector<int32_t>;

// Ablage der gelesenen Kennlinien. Ein abgelegter Puffer bleibt bis zum Prozessende gültig:
// Ein anderer Thread kann noch mit ihm dekodieren, wenn ein neuer Kontext kommt.
// Gleiche Tabellen (der Normalfall, sie hängt nicht von der ISO ab) teilen einen Puffer.
class NrawCurves {
public:
    const NrawCurveValues *keep(NrawCurveValues &&values)
    {
        for (const auto &curve : kept_) {
            if (*curve == values) {
                return curve.get();
            }
        }
        kept_.push_back(std::make_unique<NrawCurveValues>(std::move(values)));
        return kept_.back().get();
    }

private:
    std::vector<std::unique_ptr<NrawCurveValues>> kept_;
};
