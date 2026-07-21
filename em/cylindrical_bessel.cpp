#include "cylindrical_bessel.h"

#include <cmath>
#include <functional>

namespace em
{
namespace
{
// Нули ищутся сканированием с постоянным шагом и последующей дихотомией.
// Порядки и номера мод здесь малы (m, n <= ~8), поэтому такой перебор дешевле и
// заметно надёжнее асимптотики Мак-Магона с ньютоновским уточнением, которая на
// первых нулях низких порядков может сойтись к соседнему корню.
constexpr double scan_step = 0.05;
constexpr double scan_limit = 400.0;
constexpr int bisection_iterations = 80;

double bisect(const std::function<double(double)> &function, double low, double high)
{
    double low_value = function(low);
    for (int iteration = 0; iteration < bisection_iterations; ++iteration) {
        const double middle = 0.5 * (low + high);
        const double middle_value = function(middle);
        if (middle_value == 0.0) {
            return middle;
        }
        if ((low_value < 0.0) != (middle_value < 0.0)) {
            high = middle;
        } else {
            low = middle;
            low_value = middle_value;
        }
    }
    return 0.5 * (low + high);
}

// Находит index-й положительный корень function, начиная сканирование с start.
double nthRoot(const std::function<double(double)> &function, double start, int index)
{
    if (index < 1) {
        return 0.0;
    }

    int found = 0;
    double previous = start;
    double previous_value = function(previous);
    for (double x = start + scan_step; x < scan_limit; x += scan_step) {
        const double value = function(x);
        if (value == 0.0) {
            if (++found == index) {
                return x;
            }
            previous = x;
            previous_value = value;
            continue;
        }
        if ((previous_value < 0.0) != (value < 0.0)) {
            if (++found == index) {
                return bisect(function, previous, x);
            }
        }
        previous = x;
        previous_value = value;
    }
    return 0.0;
}
}

double besselJ(int order, double argument)
{
    if (order < 0) {
        return 0.0;
    }
    return std::cyl_bessel_j(static_cast<double>(order), argument);
}

double besselJDerivative(int order, double argument)
{
    if (order < 0) {
        return 0.0;
    }
    if (order == 0) {
        return -besselJ(1, argument);
    }
    return 0.5 * (besselJ(order - 1, argument) - besselJ(order + 1, argument));
}

double besselJZero(int order, int index)
{
    if (order < 0 || index < 1) {
        return 0.0;
    }
    // J_m(0) = 0 при m >= 1 — это не тот нуль, что нумеруют p_mn, поэтому
    // сканирование начинается правее нуля.
    const double start = order == 0 ? 1.0e-3 : 0.5;
    return nthRoot([order](double x) { return besselJ(order, x); }, start, index);
}

double besselJDerivativeZero(int order, int index)
{
    if (order < 0 || index < 1) {
        return 0.0;
    }
    // При m >= 2 производная обращается в ноль в самой точке x = 0; этот
    // тривиальный корень пропускается стартом сканирования правее него. Первый
    // настоящий нуль даже при m = 1 равен 1.8412, так что старт 0.5 безопасен.
    const double start = order == 0 ? 1.0e-3 : 0.5;
    return nthRoot([order](double x) { return besselJDerivative(order, x); }, start, index);
}
}
