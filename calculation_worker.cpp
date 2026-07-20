#include "calculation_worker.h"

CalculationWorker::CalculationWorker(QObject *parent)
    : QObject(parent)
{
}

void CalculationWorker::setLatestRequestId(int request_id) noexcept
{
    latest_request_id_.store(request_id, std::memory_order_release);
}

void CalculationWorker::calculate(int request_id, const WaveguideParameters &parameters)
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
        });
    if (result.cancelled || cancellation_requested()) {
        return;
    }
    emit calculated(request_id, result);
}
