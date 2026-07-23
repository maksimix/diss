#pragma once

#include "field_visualization_generator.h"
#include "waveguide_types.h"

class QtFieldGlyphAdapter
{
public:
    QVector<FieldGlyph> build(
        const em::FieldSolution &solution,
        const postprocessing::FieldVisualizationSettings &settings = {},
        const postprocessing::GenerationControl &control = {}) const;

    // offset_fraction — смещение плоскости среза вдоль нормали в долях
    // поперечного размера [-0.5, 0.5]; resolution_scale < 1 огрубляет сетку
    // (нужно стопке срезов объёмной заливки).
    FieldSlice buildSlice(
        const em::FieldSolution &solution,
        FieldSlicePlane plane,
        double offset_fraction = 0.0,
        const postprocessing::GenerationControl &control = {},
        double resolution_scale = 1.0) const;

    // Стопка равноотстоящих полупрозрачных срезов поперёк волновода — грубое
    // объёмное представление |E| целиком, а не одной плоскостью.
    QVector<FieldSlice> buildVolumeSlices(
        const em::FieldSolution &solution,
        FieldSlicePlane plane,
        int slice_count,
        const postprocessing::GenerationControl &control = {}) const;
};
