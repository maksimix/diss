#include "calculation_worker.h"

#include "postprocessing/qt_field_glyph_adapter.h"

CalculationWorker::CalculationWorker(QObject *parent)
    : QObject(parent)
{
}

void CalculationWorker::setLatestRequestId(int request_id) noexcept
{
    latest_request_id_.store(request_id, std::memory_order_release);
}

void CalculationWorker::setLatestGlyphRequestId(int request_id) noexcept
{
    latest_glyph_request_id_.store(request_id, std::memory_order_release);
}

void CalculationWorker::setLatestFillRequestId(int request_id) noexcept
{
    latest_fill_request_id_.store(request_id, std::memory_order_release);
}

void CalculationWorker::calculate(int request_id,
                                  const WaveguideParameters &parameters,
                                  double arrow_density)
{
    const auto cancellation_requested = [this, request_id]() {
        return latest_request_id_.load(std::memory_order_acquire) != request_id;
    };
    if (cancellation_requested()) {
        return;
    }

    const WaveguideCalculationResult result = calculator_.calculate(
        parameters,
        cancellation_requested,
        [this, request_id, &cancellation_requested](const QString &stage) {
            if (!cancellation_requested()) {
                emit progressed(request_id, stage);
            }
        },
        arrow_density);
    if (result.cancelled || cancellation_requested()) {
        return;
    }
    emit calculated(request_id, result);
}

void CalculationWorker::regenerateGlyphs(
    int glyph_request_id,
    std::shared_ptr<const em::FieldSolution> field_solution,
    double arrow_density)
{
    const auto cancellation_requested = [this, glyph_request_id]() {
        return latest_glyph_request_id_.load(std::memory_order_acquire) !=
               glyph_request_id;
    };
    if (!field_solution || cancellation_requested()) {
        return;
    }

    postprocessing::GenerationControl control;
    control.cancellation_requested = cancellation_requested;
    postprocessing::FieldVisualizationSettings settings;
    settings.arrow_density = arrow_density;
    const QVector<FieldGlyph> glyphs =
        QtFieldGlyphAdapter().build(*field_solution, settings, control);
    if (cancellation_requested()) {
        return;
    }
    emit glyphsRegenerated(glyph_request_id, glyphs);
}

void CalculationWorker::rebuildSlice(
    int fill_request_id,
    std::shared_ptr<const em::FieldSolution> field_solution,
    int slice_plane,
    double offset_fraction)
{
    const auto cancellation_requested = [this, fill_request_id]() {
        return latest_fill_request_id_.load(std::memory_order_acquire) !=
               fill_request_id;
    };
    if (!field_solution || cancellation_requested()) {
        return;
    }

    postprocessing::GenerationControl control;
    control.cancellation_requested = cancellation_requested;
    const FieldSlicePlane plane = static_cast<FieldSlicePlane>(slice_plane);
    const FieldSlice slice =
        QtFieldGlyphAdapter().buildSlice(*field_solution, plane, offset_fraction, control);
    if (cancellation_requested()) {
        return;
    }
    emit sliceRebuilt(fill_request_id, slice_plane, slice);
}

void CalculationWorker::buildVolumeFill(
    int fill_request_id,
    std::shared_ptr<const em::FieldSolution> field_solution,
    int slice_plane,
    int slice_count)
{
    const auto cancellation_requested = [this, fill_request_id]() {
        return latest_fill_request_id_.load(std::memory_order_acquire) !=
               fill_request_id;
    };
    if (!field_solution || cancellation_requested()) {
        return;
    }

    postprocessing::GenerationControl control;
    control.cancellation_requested = cancellation_requested;
    const FieldSlicePlane plane = static_cast<FieldSlicePlane>(slice_plane);
    const QVector<FieldSlice> slices =
        QtFieldGlyphAdapter().buildVolumeSlices(*field_solution,
                                                plane,
                                                slice_count,
                                                control);
    if (cancellation_requested()) {
        return;
    }
    emit volumeFillBuilt(fill_request_id, slice_plane, slices);
}
