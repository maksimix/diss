#include "cylindrical_bessel.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

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

// ----------------------------------------------------- вещественный порядок --

double besselJ(double order, double argument)
{
    if (order < 0.0 || !(argument > 0.0)) {
        return order == 0.0 && argument == 0.0 ? 1.0 : 0.0;
    }
    return std::cyl_bessel_j(order, argument);
}

double besselY(double order, double argument)
{
    if (order < 0.0 || !(argument > 0.0)) {
        return 0.0;
    }
    return std::cyl_neumann(order, argument);
}

double besselJDerivative(double order, double argument)
{
    if (!(argument > 0.0)) {
        return 0.0;
    }
    return order / argument * besselJ(order, argument) - besselJ(order + 1.0, argument);
}

double besselYDerivative(double order, double argument)
{
    if (!(argument > 0.0)) {
        return 0.0;
    }
    return order / argument * besselY(order, argument) - besselY(order + 1.0, argument);
}

double besselJRatio(double order, double argument)
{
    if (!(argument > 0.0)) {
        return 0.0;
    }
    // J_{nu+1}/J_nu = 1 / (b_1 - 1 / (b_2 - ...)), b_k = 2 (nu + k) / x.
    // Дробь считается с конца: в эту сторону она устойчива, потому что каждый
    // следующий знаменатель растёт как 2k/x и влияние выбранного «хвоста»
    // затухает быстрее любой степени.
    const int depth = static_cast<int>(order + argument) + 80;
    double fraction = 2.0 * (order + depth) / argument;
    for (int index = depth - 1; index >= 1; --index) {
        const double next = 2.0 * (order + index) / argument - 1.0 / fraction;
        fraction = next;
    }
    return 1.0 / fraction;
}

double besselYRatio(double order, double argument)
{
    if (!(argument > 0.0)) {
        return 0.0;
    }
    // В осциллирующей области x > nu функции второго рода имеют нули, и
    // рекурсия по отношениям на них спотыкается; там обе величины конечны, и
    // прямое частное точнее.
    const double direct_low = besselY(order, argument);
    const double direct_high = besselY(order + 1.0, argument);
    if (std::isfinite(direct_low) && std::isfinite(direct_high) &&
        std::abs(direct_low) > 1.0e-280) {
        return direct_high / direct_low;
    }

    // Иначе — вверх по порядку от дробной части: для Y_nu это направление
    // устойчиво, а отношения не переполняются даже там, где сами функции уже
    // вне диапазона double.
    const double integer_part = std::floor(order);
    const double fractional_order = order - integer_part;
    double ratio = besselY(fractional_order + 1.0, argument) /
                   besselY(fractional_order, argument);
    const int steps = static_cast<int>(integer_part);
    for (int index = 1; index <= steps; ++index) {
        ratio = 2.0 * (fractional_order + index) / argument - 1.0 / ratio;
    }
    return ratio;
}

double besselJLogMagnitude(double order, double argument, double *sign)
{
    if (sign != nullptr) {
        *sign = 1.0;
    }
    if (!(argument > 0.0) || order < 0.0) {
        return -std::numeric_limits<double>::infinity();
    }
    const double direct = besselJ(order, argument);
    if (std::isfinite(direct) && std::abs(direct) > 1.0e-280) {
        if (sign != nullptr) {
            *sign = direct < 0.0 ? -1.0 : 1.0;
        }
        return std::log(std::abs(direct));
    }
    // Прямое значение исчезло в машинный ноль: остаётся восходящий ряд
    // J_nu(x) = (x/2)^nu / Gamma(nu+1) * S. В этой области S отличается от
    // единицы на величину порядка x^2/(4 nu), так что ряд сходится с первых
    // членов и знак определяется его суммой.
    double term = 1.0;
    double series = 1.0;
    const double squared = 0.25 * argument * argument;
    for (int index = 0; index < 200; ++index) {
        term *= -squared / ((index + 1.0) * (order + index + 1.0));
        series += term;
        if (std::abs(term) < 1.0e-18 * std::abs(series)) {
            break;
        }
    }
    if (sign != nullptr) {
        *sign = series < 0.0 ? -1.0 : 1.0;
    }
    return order * std::log(0.5 * argument) - std::lgamma(order + 1.0) +
           std::log(std::abs(series));
}

double besselYLogMagnitude(double order, double argument, double *sign)
{
    if (sign != nullptr) {
        *sign = -1.0;
    }
    if (!(argument > 0.0) || order < 0.0) {
        return std::numeric_limits<double>::infinity();
    }
    const double direct = besselY(order, argument);
    if (std::isfinite(direct) && std::abs(direct) > 1.0e-280) {
        if (sign != nullptr) {
            *sign = direct < 0.0 ? -1.0 : 1.0;
        }
        return std::log(std::abs(direct));
    }
    // Значение переполнило double: логарифм набирается лесенкой отношений
    // соседних порядков, каждое из которых конечно.
    const double integer_part = std::floor(order);
    const double fractional_order = order - integer_part;
    double value = besselY(fractional_order, argument);
    double logarithm = std::log(std::abs(value));
    double current_sign = value < 0.0 ? -1.0 : 1.0;
    double ratio = besselY(fractional_order + 1.0, argument) / value;
    const int steps = static_cast<int>(integer_part);
    for (int index = 1; index <= steps; ++index) {
        logarithm += std::log(std::abs(ratio));
        current_sign *= ratio < 0.0 ? -1.0 : 1.0;
        ratio = 2.0 * (fractional_order + index) / argument - 1.0 / ratio;
    }
    if (sign != nullptr) {
        *sign = current_sign;
    }
    return logarithm;
}
}
