#pragma once

#include "field_visualization_generator.h"
#include "waveguide_types.h"

class QtFieldGlyphAdapter
{
public:
    QVector<FieldGlyph> build(
        const em::FieldSolution &solution,
        const postprocessing::GenerationControl &control = {}) const;

    FieldSlice buildSlice(
        const em::FieldSolution &solution,
        FieldSlicePlane plane,
        const postprocessing::GenerationControl &control = {}) const;
};
