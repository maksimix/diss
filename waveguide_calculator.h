#pragma once

#include "waveguide_types.h"

#include <functional>

// Моды сечения на частоте модели, отсортированные по возрастанию отсечки, с
// пометкой, распространяется ли каждая. Решаются только характеристические
// уравнения (для круглого сечения — нули функций Бесселя), поле не строится,
// поэтому вызов дешёвый и годится для диалога возбуждения. maximum_index —
// верхняя граница обоих индексов.
QVector<WaveguideMode> enumerateWaveguideModes(const WaveguideParameters &parameters,
                                               int maximum_index = 6);

// Приводит ручной выбор моды к тому сечению, которое сейчас у модели: в круглом
// волноводе радиальный индекс считается от единицы, у TM-моды прямоугольного оба
// индекса больше нуля. Возвращает true, если выбор пришлось поправить — смена
// сечения иначе оставляла бы модель с несуществующей модой.
bool normalizeModeSelection(WaveguideParameters &parameters);

class WaveguideCalculator
{
public:
    // line_density — концентрация силовых линий в визуализации поля (1.0 —
    // обычная). Настройка отображения, а не физики: на решатель не влияет.
    WaveguideCalculationResult calculate(
        const WaveguideParameters &parameters,
        const std::function<bool()> &cancellation_requested = {},
        const std::function<void(const QString &)> &progress_reporter = {},
        double line_density = 1.0) const;
};
