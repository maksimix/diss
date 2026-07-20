#pragma once

#include "waveguide_calculator.h"

#include <QtCore/QObject>

#include <atomic>

class CalculationWorker : public QObject
{
    Q_OBJECT

public:
    explicit CalculationWorker(QObject *parent = nullptr);
    // Thread-safe: the GUI calls this directly to interrupt the active request.
    void setLatestRequestId(int request_id) noexcept;

public slots:
    void calculate(int request_id, const WaveguideParameters &parameters);

signals:
    void calculated(int request_id, const WaveguideCalculationResult &result);
    void progressed(int request_id, const QString &stage);

private:
    std::atomic<int> latest_request_id_{0};
    WaveguideCalculator calculator_;
};
