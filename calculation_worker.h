#pragma once

#include "waveguide_calculator.h"

#include <QtCore/QObject>

#include <atomic>
#include <memory>

class CalculationWorker : public QObject
{
    Q_OBJECT

public:
    explicit CalculationWorker(QObject *parent = nullptr);
    // Thread-safe: the GUI calls this directly to interrupt the active request.
    void setLatestRequestId(int request_id) noexcept;
    // Аналогично для перестройки стрелок: устаревший запрос обрывается, как
    // только интерфейс выдал более новый (или обнулил счётчик при новом
    // расчёте либо закрытии окна).
    void setLatestGlyphRequestId(int request_id) noexcept;
    // И для заливки |E| (перенос среза и стопка объёма): у среза и объёма одно
    // пространство номеров — новый запрос любого вида отменяет предыдущий.
    void setLatestFillRequestId(int request_id) noexcept;

public slots:
    void calculate(int request_id,
                   const WaveguideParameters &parameters,
                   double arrow_density);
    // Перестраивает линии и стрелки поля по уже готовому решению — без запуска
    // решателя. Вызывается при смене концентрации стрелок пользователем.
    void regenerateGlyphs(int glyph_request_id,
                          std::shared_ptr<const em::FieldSolution> field_solution,
                          double arrow_density);
    // Пересобирает срез |E| на смещённой плоскости (slice_plane —
    // FieldSlicePlane как int, offset_fraction — доля поперечника [-0.5, 0.5]).
    void rebuildSlice(int fill_request_id,
                      std::shared_ptr<const em::FieldSolution> field_solution,
                      int slice_plane,
                      double offset_fraction);
    // Строит стопку срезов для объёмной заливки |E| всей полости.
    void buildVolumeFill(int fill_request_id,
                         std::shared_ptr<const em::FieldSolution> field_solution,
                         int slice_plane,
                         int slice_count);

signals:
    void calculated(int request_id, const WaveguideCalculationResult &result);
    void progressed(int request_id, const QString &stage);
    void glyphsRegenerated(int glyph_request_id, const QVector<FieldGlyph> &glyphs);
    void sliceRebuilt(int fill_request_id, int slice_plane, const FieldSlice &slice);
    void volumeFillBuilt(int fill_request_id,
                         int slice_plane,
                         const QVector<FieldSlice> &slices);

private:
    std::atomic<int> latest_request_id_{0};
    std::atomic<int> latest_glyph_request_id_{0};
    std::atomic<int> latest_fill_request_id_{0};
    WaveguideCalculator calculator_;
};
