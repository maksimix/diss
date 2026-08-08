#include "ridged_circular_solver.h"

#include "cylindrical_bessel.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace em
{
namespace
{
constexpr double numerical_tolerance = 1.0e-12;
constexpr double logarithm_clamp = 700.0;

// ------------------------------------------------------ угловые функции ------

// Порядок nu_{w,m} угловой собственной функции частичной области. В первой
// области угол ограничен плоскостями симметрии, поэтому порядок задают оба
// условия; во второй области дальняя граница — всегда металл гребня, и от g1
// она не зависит.
double angularOrder(const RidgedCircularProblem &problem, int region, int index)
{
    const double width = region == 1 ? problem.sector_angle_rad : problem.ridge_angle_rad;
    const double half_step = region == 1 ? std::abs(problem.g2 - problem.g1)
                                         : static_cast<double>(problem.g2);
    return index * pi / width + half_step * pi / (2.0 * width);
}

// Сама угловая функция и её производная по углу.
double angularPattern(const RidgedCircularProblem &problem, double order, double azimuth_rad)
{
    const double shift = 0.5 * pi * problem.g2;
    return problem.family == ModeFamily::TransverseElectric
               ? std::cos(order * azimuth_rad - shift)
               : std::sin(order * azimuth_rad + shift);
}

double angularPatternDerivative(const RidgedCircularProblem &problem,
                                double order,
                                double azimuth_rad)
{
    const double shift = 0.5 * pi * problem.g2;
    return problem.family == ModeFamily::TransverseElectric
               ? -order * std::sin(order * azimuth_rad - shift)
               : order * std::cos(order * azimuth_rad + shift);
}

// Квадрат нормы угловой функции. Ноль означает, что функция тождественно
// нулевая: при nu = 0 синус вырождается, и такого члена ряда просто нет.
double angularNorm(const RidgedCircularProblem &problem, int region, int index)
{
    const double width = region == 1 ? problem.sector_angle_rad : problem.ridge_angle_rad;
    const double order = angularOrder(problem, region, index);
    if (order <= numerical_tolerance) {
        const double value = angularPattern(problem, 0.0, 0.0);
        return std::abs(value) < 1.0e-9 ? 0.0 : width;
    }
    const double shift = problem.family == ModeFamily::TransverseElectric
                             ? -0.5 * pi * problem.g2
                             : 0.5 * pi * problem.g2;
    const double oscillation =
        (std::sin(2.0 * (order * width + shift)) - std::sin(2.0 * shift)) / (4.0 * order);
    return problem.family == ModeFamily::TransverseElectric ? 0.5 * width + oscillation
                                                            : 0.5 * width - oscillation;
}

// ------------------------------------------------------- базис на окне -------

// Проекция краевой функции на угловую собственную функцию области. Весовой
// множитель Гегенбауэра задаёт правильное поведение поля у кромки, и интеграл
// от него с косинусом (синусом) берётся в замкнутом виде через функцию Бесселя
// порядка n + lambda. Общие множители, зависящие только от номера полинома,
// опущены: в определителе они выносятся из строки и столбца и корней не сдвигают.
double edgeProjection(const RidgedCircularProblem &problem,
                      double edge_exponent,
                      int polynomial_index,
                      double angular_order)
{
    const bool longitudinal = problem.family == ModeFamily::TransverseMagnetic;
    const double lambda = longitudinal ? edge_exponent + 0.5 : edge_exponent - 0.5;
    // Чётность полинома диктует условие на плоскости phi = 0: E_phi и E_z ведут
    // себя там противоположно, поэтому у продольной функции степень сдвинута.
    const int degree = longitudinal ? 2 * polynomial_index + 1 - problem.g2
                                    : 2 * polynomial_index + problem.g2;
    const double order = degree + lambda;
    const double argument = angular_order * problem.aperture_angle_rad;
    if (argument <= numerical_tolerance) {
        // Предел при nu -> 0: J_{n+lambda}(a) / a^lambda -> a^n / (2^{n+lambda}
        // Gamma(n+lambda+1)), то есть нулю при n > 0 и конечной величине при n = 0.
        return degree > 0 ? 0.0
                          : std::pow(2.0, -lambda) / std::tgamma(lambda + 1.0);
    }
    return besselJ(order, argument) / std::pow(argument, lambda);
}

// ------------------------------------------------------ радиальные функции ---

double clampExponent(double value)
{
    return std::max(-logarithm_clamp, std::min(logarithm_clamp, value));
}

RidgedRadialProfile makeRadialProfile(double order,
                                double beta,
                                double radius_ratio,
                                bool inner_region,
                                bool neumann_outer,
                                bool divide_by_derivative)
{
    RidgedRadialProfile profile;
    profile.order = order;
    profile.inner_argument = beta * radius_ratio;
    profile.outer_argument = beta;
    profile.inner_region = inner_region;
    profile.neumann_outer = neumann_outer;
    profile.divide_by_derivative = divide_by_derivative;
    if (!(profile.inner_argument > 0.0)) {
        return profile;
    }

    const double x1 = profile.inner_argument;
    profile.inner_log_derivative_j = order / x1 - besselJRatio(order, x1);
    profile.log_inner_j = besselJLogMagnitude(order, x1, &profile.sign_inner_j);
    if (inner_region) {
        profile.normalization = divide_by_derivative ? profile.inner_log_derivative_j : 1.0;
        profile.valid = std::isfinite(profile.inner_log_derivative_j) &&
                        std::abs(profile.normalization) > 0.0;
        return profile;
    }

    const double x2 = profile.outer_argument;
    profile.inner_log_derivative_y = order / x1 - besselYRatio(order, x1);
    profile.log_inner_y = besselYLogMagnitude(order, x1, &profile.sign_inner_y);

    double sign_outer_j = 1.0;
    double sign_outer_y = 1.0;
    double log_outer_j = besselJLogMagnitude(order, x2, &sign_outer_j);
    double log_outer_y = besselYLogMagnitude(order, x2, &sign_outer_y);
    if (neumann_outer) {
        const double outer_log_derivative_j = order / x2 - besselJRatio(order, x2);
        const double outer_log_derivative_y = order / x2 - besselYRatio(order, x2);
        log_outer_j += std::log(std::abs(outer_log_derivative_j));
        log_outer_y += std::log(std::abs(outer_log_derivative_y));
        sign_outer_j *= outer_log_derivative_j < 0.0 ? -1.0 : 1.0;
        sign_outer_y *= outer_log_derivative_y < 0.0 ? -1.0 : 1.0;
    }
    profile.log_outer_factor = log_outer_j - log_outer_y;
    profile.sign_outer_factor = sign_outer_j * sign_outer_y;

    // G = [J(x1)/Y(x1)] / L — единственная комбинация, в которой наружное
    // условие вообще влияет на поле внутри области.
    const double log_g = profile.log_inner_j - profile.log_inner_y - profile.log_outer_factor;
    const double sign_g = profile.sign_inner_j * profile.sign_inner_y *
                          profile.sign_outer_factor;
    const double g = sign_g * std::exp(clampExponent(log_g));
    profile.normalization = divide_by_derivative
                                ? g * profile.inner_log_derivative_j -
                                      profile.inner_log_derivative_y
                                : g - 1.0;
    profile.valid = std::isfinite(profile.normalization) &&
                    std::abs(profile.normalization) > 0.0 &&
                    std::isfinite(profile.inner_log_derivative_y);
    return profile;
}

// R(x)/N и dR/dx / N в произвольной точке области.
void evaluateRadial(const RidgedRadialProfile &profile,
                    double argument,
                    double *value,
                    double *derivative)
{
    *value = 0.0;
    *derivative = 0.0;
    if (!profile.valid || !(argument > 0.0)) {
        return;
    }
    const double order = profile.order;
    const double log_derivative_j = order / argument - besselJRatio(order, argument);
    double sign_j = 1.0;
    const double log_j = besselJLogMagnitude(order, argument, &sign_j);

    if (profile.inner_region) {
        const double relative =
            sign_j * profile.sign_inner_j *
            std::exp(clampExponent(log_j - profile.log_inner_j));
        *value = relative / profile.normalization;
        *derivative = log_derivative_j * relative / profile.normalization;
        return;
    }

    const double log_derivative_y = order / argument - besselYRatio(order, argument);
    double sign_y = 1.0;
    const double log_y = besselYLogMagnitude(order, argument, &sign_y);
    // P = J(x) / (L * Y(x1)), Q = Y(x) / Y(x1): обе величины конечны на всём
    // отрезке r1..r2, тогда как J, Y и L по отдельности — нет.
    const double p = sign_j * profile.sign_outer_factor * profile.sign_inner_y *
                     std::exp(clampExponent(log_j - profile.log_outer_factor -
                                            profile.log_inner_y));
    const double q = sign_y * profile.sign_inner_y *
                     std::exp(clampExponent(log_y - profile.log_inner_y));
    *value = (p - q) / profile.normalization;
    *derivative = (p * log_derivative_j - q * log_derivative_y) / profile.normalization;
}

// Отношение, которым частичная область входит в систему сшивания: R/R' для
// H-волн и R'/R для E-волн, взятое на границе раздела.
double matchingRatio(const RidgedRadialProfile &profile)
{
    if (!profile.valid) {
        return 0.0;
    }
    double value = 0.0;
    double derivative = 0.0;
    evaluateRadial(profile, profile.inner_argument, &value, &derivative);
    return profile.divide_by_derivative ? value : derivative;
}

// ------------------------------------------------------------ система -------

struct SeriesTerm
{
    int region = 1;
    double angular_order = 0.0;
    double angular_norm = 0.0;
    double radial_ratio = 0.0;
    double weight = 0.0;              // множитель перед psi_i psi_j
    RidgedRadialProfile profile;
};

// Нормированный радиус, дальше которого член ряда с таким порядком уже ничего
// не вносит: радиальная функция ведёт себя как (r/r1)^{±nu}, поэтому при
// nu * |ln(r/r1)| больше сорока её вклад лежит под уровнем округления двойной
// точности. Проверка снимает основную часть работы при обходе сечения, где
// иначе каждая точка стоила бы всех членов ряда.
bool radialTermIsNegligible(double angular_order, double normalized_radius, double radius_ratio)
{
    if (!(angular_order > 0.0) || !(normalized_radius > 0.0) || !(radius_ratio > 0.0)) {
        return false;
    }
    return angular_order * std::abs(std::log(normalized_radius / radius_ratio)) > 40.0;
}

std::vector<SeriesTerm> buildSeries(const RidgedCircularProblem &problem, double normalized_cutoff)
{
    std::vector<SeriesTerm> series;
    const bool magnetic = problem.family == ModeFamily::TransverseElectric;
    const int count = std::max(1, problem.series_terms);
    series.reserve(static_cast<size_t>(2 * count));
    for (int region = 1; region <= 2; ++region) {
        const double permittivity =
            region == 1 ? problem.core_permittivity : problem.shell_permittivity;
        const double beta = normalized_cutoff * std::sqrt(permittivity);
        if (!(beta > 0.0)) {
            continue;
        }
        for (int index = 0; index < count; ++index) {
            SeriesTerm term;
            term.region = region;
            term.angular_order = angularOrder(problem, region, index);
            term.angular_norm = angularNorm(problem, region, index);
            if (!(term.angular_norm > numerical_tolerance)) {
                continue;
            }
            term.profile = makeRadialProfile(term.angular_order,
                                             beta,
                                             problem.radius_ratio,
                                             region == 1,
                                             magnetic,
                                             magnetic);
            if (!term.profile.valid) {
                continue;
            }
            term.radial_ratio = matchingRatio(term.profile);
            if (!std::isfinite(term.radial_ratio)) {
                continue;
            }
            // Условие сшивания — равенство величин по обе стороны границы,
            // поэтому вторая область входит с обратным знаком. Общий для всей
            // системы множитель 1 / (j kc) опущен.
            const double sign = region == 1 ? 1.0 : -1.0;
            term.weight = sign * beta / term.angular_norm * term.radial_ratio;
            series.push_back(term);
        }
    }
    return series;
}

std::vector<double> assembleSystemMatrix(const RidgedCircularProblem &problem,
                                         double normalized_cutoff)
{
    const int order = std::max(1, problem.edge_terms);
    const double edge_exponent = ridgeEdgeSingularityExponent(problem);
    std::vector<double> matrix(static_cast<size_t>(order) * order, 0.0);
    std::vector<double> projection(static_cast<size_t>(order), 0.0);
    for (const SeriesTerm &term : buildSeries(problem, normalized_cutoff)) {
        for (int index = 0; index < order; ++index) {
            projection[index] =
                edgeProjection(problem, edge_exponent, index, term.angular_order);
        }
        for (int row = 0; row < order; ++row) {
            for (int column = 0; column < order; ++column) {
                matrix[static_cast<size_t>(row) * order + column] +=
                    term.weight * projection[row] * projection[column];
            }
        }
    }
    return matrix;
}

// ln|det| и знак определителя через разложение с выбором главного элемента.
double logAbsDeterminant(std::vector<double> matrix, int order, int *sign)
{
    *sign = 1;
    double logarithm = 0.0;
    for (int step = 0; step < order; ++step) {
        int pivot = step;
        double best = std::abs(matrix[static_cast<size_t>(step) * order + step]);
        for (int row = step + 1; row < order; ++row) {
            const double candidate = std::abs(matrix[static_cast<size_t>(row) * order + step]);
            if (candidate > best) {
                best = candidate;
                pivot = row;
            }
        }
        if (!(best > 0.0)) {
            *sign = 0;
            return -std::numeric_limits<double>::infinity();
        }
        if (pivot != step) {
            for (int column = 0; column < order; ++column) {
                std::swap(matrix[static_cast<size_t>(step) * order + column],
                          matrix[static_cast<size_t>(pivot) * order + column]);
            }
            *sign = -*sign;
        }
        const double diagonal = matrix[static_cast<size_t>(step) * order + step];
        logarithm += std::log(std::abs(diagonal));
        *sign *= diagonal < 0.0 ? -1 : 1;
        for (int row = step + 1; row < order; ++row) {
            const double factor = matrix[static_cast<size_t>(row) * order + step] / diagonal;
            if (factor == 0.0) {
                continue;
            }
            for (int column = step; column < order; ++column) {
                matrix[static_cast<size_t>(row) * order + column] -=
                    factor * matrix[static_cast<size_t>(step) * order + column];
            }
        }
    }
    return logarithm;
}

double systemLogDeterminant(const RidgedCircularProblem &problem,
                            double normalized_cutoff,
                            int *sign)
{
    const int order = std::max(1, problem.edge_terms);
    return logAbsDeterminant(assembleSystemMatrix(problem, normalized_cutoff), order, sign);
}

// Симметричная задача на собственные значения методом Якоби: порядок системы
// здесь единицы, поэтому вращений хватает с запасом, а собственный вектор
// наименьшего по модулю значения и есть искомое нетривиальное решение.
void symmetricEigen(std::vector<double> matrix,
                    int order,
                    std::vector<double> *values,
                    std::vector<double> *vectors)
{
    values->assign(static_cast<size_t>(order), 0.0);
    vectors->assign(static_cast<size_t>(order) * order, 0.0);
    for (int index = 0; index < order; ++index) {
        (*vectors)[static_cast<size_t>(index) * order + index] = 1.0;
    }
    for (int sweep = 0; sweep < 100; ++sweep) {
        double off_diagonal = 0.0;
        for (int row = 0; row < order; ++row) {
            for (int column = row + 1; column < order; ++column) {
                off_diagonal += matrix[static_cast<size_t>(row) * order + column] *
                                matrix[static_cast<size_t>(row) * order + column];
            }
        }
        if (off_diagonal < 1.0e-30) {
            break;
        }
        for (int p = 0; p < order; ++p) {
            for (int q = p + 1; q < order; ++q) {
                const double apq = matrix[static_cast<size_t>(p) * order + q];
                if (std::abs(apq) < 1.0e-300) {
                    continue;
                }
                const double app = matrix[static_cast<size_t>(p) * order + p];
                const double aqq = matrix[static_cast<size_t>(q) * order + q];
                const double theta = 0.5 * (aqq - app) / apq;
                const double t = (theta >= 0.0 ? 1.0 : -1.0) /
                                 (std::abs(theta) + std::sqrt(theta * theta + 1.0));
                const double c = 1.0 / std::sqrt(t * t + 1.0);
                const double s = t * c;
                for (int k = 0; k < order; ++k) {
                    const double akp = matrix[static_cast<size_t>(k) * order + p];
                    const double akq = matrix[static_cast<size_t>(k) * order + q];
                    matrix[static_cast<size_t>(k) * order + p] = c * akp - s * akq;
                    matrix[static_cast<size_t>(k) * order + q] = s * akp + c * akq;
                }
                for (int k = 0; k < order; ++k) {
                    const double apk = matrix[static_cast<size_t>(p) * order + k];
                    const double aqk = matrix[static_cast<size_t>(q) * order + k];
                    matrix[static_cast<size_t>(p) * order + k] = c * apk - s * aqk;
                    matrix[static_cast<size_t>(q) * order + k] = s * apk + c * aqk;
                }
                for (int k = 0; k < order; ++k) {
                    const double vkp = (*vectors)[static_cast<size_t>(k) * order + p];
                    const double vkq = (*vectors)[static_cast<size_t>(k) * order + q];
                    (*vectors)[static_cast<size_t>(k) * order + p] = c * vkp - s * vkq;
                    (*vectors)[static_cast<size_t>(k) * order + q] = s * vkp + c * vkq;
                }
            }
        }
    }
    for (int index = 0; index < order; ++index) {
        (*values)[index] = matrix[static_cast<size_t>(index) * order + index];
    }
}
}

double ridgeEdgeSingularityExponent(const RidgedCircularProblem &problem)
{
    const double permittivity_ratio =
        problem.shell_permittivity / std::max(numerical_tolerance, problem.core_permittivity);
    if (problem.aperture_angle_rad < problem.ridge_angle_rad - 1.0e-9) {
        // Кромка кольцевого сегмента: металлическое полуплоское ребро.
        return 0.5;
    }
    if (problem.ridge_angle_rad < problem.sector_angle_rad - 1.0e-9) {
        // Кромка гребня конечной толщины на границе двух диэлектриков.
        return 2.0 / pi * std::atan(std::sqrt(1.0 + 2.0 * permittivity_ratio));
    }
    // Бесконечно тонкий гребень: клин из двух диэлектриков.
    return 1.0 / pi *
           std::acos((1.0 - permittivity_ratio) / (1.0 + permittivity_ratio));
}

std::vector<double> findRidgedCircularCutoffs(const RidgedCircularProblem &problem,
                                              int count,
                                              const SolveControl &control)
{
    std::vector<double> roots;
    if (count <= 0) {
        return roots;
    }
    // Диапазон просмотра: до kc*r2 = 12 укладываются все моды, которые имеет
    // смысл возбуждать в такой структуре. Шаг выбран мельче типичного
    // расстояния между соседними корнями и полюсами определителя.
    constexpr double scan_start = 0.02;
    constexpr double scan_end = 12.0;
    constexpr double scan_step = 0.005;

    int previous_sign = 0;
    double previous_logarithm = 0.0;
    double previous_point = scan_start;
    for (double point = scan_start; point <= scan_end; point += scan_step) {
        if (control.isCancellationRequested()) {
            return roots;
        }
        int sign = 0;
        const double logarithm = systemLogDeterminant(problem, point, &sign);
        if (sign != 0 && previous_sign != 0 && sign != previous_sign) {
            double low = previous_point;
            double high = point;
            for (int iteration = 0; iteration < 60; ++iteration) {
                const double middle = 0.5 * (low + high);
                int middle_sign = 0;
                systemLogDeterminant(problem, middle, &middle_sign);
                if (middle_sign == previous_sign) {
                    low = middle;
                } else {
                    high = middle;
                }
            }
            const double root = 0.5 * (low + high);
            int root_sign = 0;
            const double root_logarithm = systemLogDeterminant(problem, root, &root_sign);
            // Знак определителя меняется и в нуле, и в полюсе: в нуле модуль
            // проваливается, в полюсе — вырастает. Без этой проверки полюсы
            // радиальных отношений выдавались бы за моды.
            if (root_logarithm < std::min(previous_logarithm, logarithm) - 5.0) {
                roots.push_back(root);
                if (static_cast<int>(roots.size()) >= count) {
                    return roots;
                }
            }
        }
        previous_sign = sign;
        previous_logarithm = logarithm;
        previous_point = point;
    }
    return roots;
}

std::vector<double> ridgedCircularEdgeCoefficients(const RidgedCircularProblem &problem,
                                                   double normalized_cutoff)
{
    const int order = std::max(1, problem.edge_terms);
    std::vector<double> matrix = assembleSystemMatrix(problem, normalized_cutoff);
    // Строки и столбцы различаются на много порядков, если базисные функции
    // окна дают резко разные проекции; масштабирование делает выбор наименьшего
    // собственного значения осмысленным.
    double scale = 0.0;
    for (double value : matrix) {
        scale = std::max(scale, std::abs(value));
    }
    if (!(scale > 0.0)) {
        std::vector<double> fallback(static_cast<size_t>(order), 0.0);
        fallback[0] = 1.0;
        return fallback;
    }
    for (double &value : matrix) {
        value /= scale;
    }
    std::vector<double> values;
    std::vector<double> vectors;
    symmetricEigen(matrix, order, &values, &vectors);
    int selected = 0;
    for (int index = 1; index < order; ++index) {
        if (std::abs(values[index]) < std::abs(values[selected])) {
            selected = index;
        }
    }
    std::vector<double> coefficients(static_cast<size_t>(order), 0.0);
    double norm = 0.0;
    for (int index = 0; index < order; ++index) {
        coefficients[index] = vectors[static_cast<size_t>(index) * order + selected];
        norm += coefficients[index] * coefficients[index];
    }
    norm = std::sqrt(std::max(numerical_tolerance, norm));
    for (double &value : coefficients) {
        value /= norm;
    }
    return coefficients;
}

// ------------------------------------------------------------- поле ---------

RidgedCircularModeField::RidgedCircularModeField(
    const SimulationRequest &request,
    const RidgedCircularProblem &problem,
    const ModeDescriptor &mode,
    const std::vector<double> &edge_coefficients,
    Complex forward_longitudinal_amplitude)
    : geometry_(request.model.waveguide),
      problem_(problem),
      mode_(mode),
      angular_frequency_rad_per_s_(2.0 * pi * request.frequency_hz),
      forward_longitudinal_amplitude_(forward_longitudinal_amplitude)
{
    const Material &shell = request.model.filling_material;
    const Material &core = request.model.waveguide.ridge.core_material;
    permeability_h_per_m_ = vacuum_permeability_h_per_m * shell.relative_permeability;
    core_permittivity_f_per_m_ = vacuum_permittivity_f_per_m * core.relative_permittivity;
    shell_permittivity_f_per_m_ = vacuum_permittivity_f_per_m * shell.relative_permittivity;

    const double radius_m = std::max(numerical_tolerance, geometry_.inner_radius_m);
    const double normalized_cutoff = mode.cutoff_wavenumber_per_m * radius_m;
    transverse_wavenumber_per_m_[1] =
        mode.cutoff_wavenumber_per_m * std::sqrt(problem.core_permittivity);
    transverse_wavenumber_per_m_[2] =
        mode.cutoff_wavenumber_per_m * std::sqrt(problem.shell_permittivity);

    // Коэффициенты рядов: краевая функция окна задаёт нормальную производную
    // (H-волны) или само значение (E-волны) продольной компоненты на границе
    // раздела, а ортогональность угловых функций превращает это в коэффициент
    // каждого члена ряда.
    const bool magnetic = problem.family == ModeFamily::TransverseElectric;
    const double edge_exponent = ridgeEdgeSingularityExponent(problem);
    const int series_count = std::max(1, problem.series_terms);
    const int edge_count = static_cast<int>(edge_coefficients.size());
    for (int region = 1; region <= 2; ++region) {
        const double permittivity =
            region == 1 ? problem.core_permittivity : problem.shell_permittivity;
        const double beta = normalized_cutoff * std::sqrt(permittivity);
        for (int index = 0; index < series_count; ++index) {
            const double order = angularOrder(problem, region, index);
            const double norm = angularNorm(problem, region, index);
            if (!(norm > numerical_tolerance)) {
                continue;
            }
            const RidgedRadialProfile profile = makeRadialProfile(order,
                                                                  beta,
                                                                  problem.radius_ratio,
                                                                  region == 1,
                                                                  magnetic,
                                                                  magnetic);
            if (!profile.valid) {
                continue;
            }
            double projection = 0.0;
            for (int polynomial = 0; polynomial < edge_count; ++polynomial) {
                projection += edge_coefficients[polynomial] *
                              edgeProjection(problem, edge_exponent, polynomial, order);
            }
            Term term;
            term.region = region;
            term.angular_order = order;
            term.radial = profile;
            // Нормировка радиальной функции уже содержит деление на R'(r1) для
            // H-волн и на R(r1) для E-волн, поэтому здесь остаётся только
            // проекция, норма и множитель beta у H-волн.
            term.coefficient = (magnetic ? beta : 1.0) * projection / norm;
            if (std::abs(term.coefficient) > 0.0) {
                terms_.push_back(term);
            }
        }
    }
}

bool RidgedCircularModeField::contains(const Vec3 &position_m) const
{
    return insideCrossSection(geometry_, position_m.x, position_m.y, numerical_tolerance) &&
           !insideCircularRidgeMetal(geometry_, position_m.x, position_m.y) &&
           position_m.z >= -0.5 * geometry_.length_m - numerical_tolerance &&
           position_m.z <= 0.5 * geometry_.length_m + numerical_tolerance;
}

FieldPhasor RidgedCircularModeField::evaluate(const Vec3 &position_m) const
{
    FieldPhasor field;
    if (!contains(position_m) || terms_.empty()) {
        return field;
    }

    const double outer_radius_m = geometry_.inner_radius_m;
    const double minimum_radius_m = std::max(1.0e-12, outer_radius_m * 1.0e-9);
    const double radius_m = std::max(minimum_radius_m,
                                     std::hypot(position_m.x, position_m.y));
    double azimuth_rad = std::atan2(position_m.y, position_m.x);
    if (azimuth_rad < 0.0) {
        azimuth_rad += 2.0 * pi;
    }

    // Свёртка на расчётный сектор. Каждое отражение меняет знак продольной
    // компоненты, если соответствующая плоскость симметрии для неё нулевая, и
    // разворачивает радиальную составляющую поперечного поля.
    const double sector_rad = problem_.sector_angle_rad;
    const double steps = azimuth_rad / sector_rad;
    const long long sector_index = static_cast<long long>(std::floor(steps));
    const double fraction = steps - std::floor(steps);
    const bool mirrored = sector_index % 2 != 0;
    const double folded_rad = (mirrored ? 1.0 - fraction : fraction) * sector_rad;

    const bool magnetic = problem_.family == ModeFamily::TransverseElectric;
    const double reflection_g1 = magnetic ? (problem_.g1 == 0 ? 1.0 : -1.0)
                                          : (problem_.g1 == 0 ? -1.0 : 1.0);
    const double reflection_g2 = magnetic ? (problem_.g2 == 0 ? 1.0 : -1.0)
                                          : (problem_.g2 == 0 ? -1.0 : 1.0);
    // Азимут приведён к [0, 2 pi), поэтому номер сектора неотрицателен: сектор k
    // получается k отражениями, из которых через плоскость g1 проходит каждое
    // нечётное, а через плоскость g2 — каждое чётное.
    const long long g1_count = (sector_index + 1) / 2;
    const long long g2_count = sector_index / 2;
    double parity = 1.0;
    for (long long index = 0; index < g1_count; ++index) {
        parity *= reflection_g1;
    }
    for (long long index = 0; index < g2_count; ++index) {
        parity *= reflection_g2;
    }

    const int region = radius_m <= problem_.radius_ratio * outer_radius_m ? 1 : 2;
    const double beta_per_m = transverse_wavenumber_per_m_[region];
    if (!(beta_per_m > 0.0)) {
        return field;
    }
    const double normalized_radius = radius_m / outer_radius_m;
    const double beta = mode_.cutoff_wavenumber_per_m * outer_radius_m *
                        std::sqrt(region == 1 ? problem_.core_permittivity
                                              : problem_.shell_permittivity);

    double longitudinal = 0.0;
    double radial_gradient = 0.0;
    double azimuthal_gradient = 0.0;
    for (const Term &term : terms_) {
        if (term.region != region) {
            continue;
        }
        if (!term.radial.valid ||
            radialTermIsNegligible(term.angular_order, normalized_radius,
                                   problem_.radius_ratio)) {
            continue;
        }
        double value = 0.0;
        double derivative = 0.0;
        evaluateRadial(term.radial, beta * normalized_radius, &value, &derivative);
        const double pattern = angularPattern(problem_, term.angular_order, folded_rad);
        const double pattern_derivative =
            angularPatternDerivative(problem_, term.angular_order, folded_rad);
        longitudinal += term.coefficient * value * pattern;
        radial_gradient += term.coefficient * beta_per_m * derivative * pattern;
        azimuthal_gradient += term.coefficient * value * pattern_derivative / radius_m;
    }
    if (!std::isfinite(longitudinal) || !std::isfinite(radial_gradient) ||
        !std::isfinite(azimuthal_gradient)) {
        return field;
    }

    longitudinal *= parity;
    azimuthal_gradient *= parity;
    radial_gradient *= parity;
    if (mirrored) {
        // Отражение меняет знак производной по углу: вместе с чётностью это и
        // даёт правильный разворот радиальной составляющей поперечного поля.
        azimuthal_gradient = -azimuthal_gradient;
    }

    const double distance_from_input_m = position_m.z + 0.5 * geometry_.length_m;
    const Complex axial_factor =
        forward_longitudinal_amplitude_ *
        std::exp(-mode_.propagation_constant_per_m * distance_from_input_m);
    const Complex axial_derivative = -mode_.propagation_constant_per_m * axial_factor;
    const Complex imaginary_unit(0.0, 1.0);
    const double beta_squared = beta_per_m * beta_per_m;
    const double cosine = std::cos(azimuth_rad);
    const double sine = std::sin(azimuth_rad);

    const auto to_cartesian = [cosine, sine](Complex radial, Complex azimuthal) {
        return ComplexVec3{radial * cosine - azimuthal * sine,
                           radial * sine + azimuthal * cosine,
                           0.0};
    };

    if (magnetic) {
        field.magnetic_a_per_m.z = axial_factor * longitudinal;
        const ComplexVec3 transverse_magnetic =
            to_cartesian(axial_derivative * radial_gradient / beta_squared,
                         axial_derivative * azimuthal_gradient / beta_squared);
        field.magnetic_a_per_m.x = transverse_magnetic.x;
        field.magnetic_a_per_m.y = transverse_magnetic.y;
        const Complex scale = imaginary_unit * angular_frequency_rad_per_s_ *
                              permeability_h_per_m_ / beta_squared;
        field.electric_v_per_m =
            to_cartesian(-scale * axial_factor * azimuthal_gradient,
                         scale * axial_factor * radial_gradient);
    } else {
        field.electric_v_per_m.z = axial_factor * longitudinal;
        const ComplexVec3 transverse_electric =
            to_cartesian(axial_derivative * radial_gradient / beta_squared,
                         axial_derivative * azimuthal_gradient / beta_squared);
        field.electric_v_per_m.x = transverse_electric.x;
        field.electric_v_per_m.y = transverse_electric.y;
        const Complex permittivity =
            region == 1 ? core_permittivity_f_per_m_ : shell_permittivity_f_per_m_;
        const Complex scale =
            imaginary_unit * angular_frequency_rad_per_s_ * permittivity / beta_squared;
        field.magnetic_a_per_m =
            to_cartesian(scale * axial_factor * azimuthal_gradient,
                         -scale * axial_factor * radial_gradient);
    }
    return field;
}

// ---------------------------------------------------------- решатель --------

bool RidgedCircularWaveguideSolver::canSolve(const SimulationRequest &request,
                                             std::string *reason)
{
    const WaveguideGeometry &geometry = request.model.waveguide;
    if (!hasCircularRidges(geometry)) {
        if (reason != nullptr) {
            *reason = "гребни круглого сечения не заданы или вырождены";
        }
        return false;
    }
    const bool has_inserts =
        !request.model.pec_plates.empty() || !request.model.dielectric_blocks.empty() ||
        !request.model.slot_geometries.empty() || hasEnabledShapes(request.model.shapes);
    if (has_inserts) {
        if (reason != nullptr) {
            *reason = "метод частичных областей описывает регулярное сечение, "
                      "а не вставки в тракте";
        }
        return false;
    }
    return true;
}

FieldSolution RidgedCircularWaveguideSolver::solve(const SimulationRequest &request,
                                                    const SolveControl &control) const
{
    FieldSolution solution;
    solution.request = request;
    solution.diagnostics.backend_name =
        "Метод частичных областей: круглый волновод с гребнями";

    std::string reason;
    if (!canSolve(request, &reason)) {
        solution.error_message = "Структура не описывается этим методом: " + reason + ".";
        return solution;
    }
    const WaveguideGeometry &geometry = request.model.waveguide;
    if (!std::isfinite(request.frequency_hz) || request.frequency_hz <= 0.0) {
        solution.error_message = "Frequency must be positive.";
        return solution;
    }
    if (!(geometry.length_m > 0.0)) {
        solution.error_message = "Waveguide inner dimensions and length must be positive.";
        return solution;
    }
    const Material &shell = request.model.filling_material;
    const Material &core = geometry.ridge.core_material;
    const double shell_permittivity = std::real(shell.relative_permittivity);
    const double core_permittivity = std::real(core.relative_permittivity);
    if (!(shell_permittivity > 0.0) || !(core_permittivity > 0.0) ||
        std::real(shell.relative_permeability) <= 0.0) {
        solution.error_message = "Material parameters are not passive positive media.";
        return solution;
    }

    RidgedCircularProblem problem;
    problem.radius_ratio = geometry.ridge.partition_radius_m / geometry.inner_radius_m;
    problem.sector_angle_rad = geometry.ridge.sector_angle_rad;
    problem.ridge_angle_rad = geometry.ridge.ridge_angle_rad;
    problem.aperture_angle_rad = geometry.ridge.aperture_angle_rad;
    problem.core_permittivity = core_permittivity;
    problem.shell_permittivity = shell_permittivity;
    problem.series_terms = std::max(4, geometry.ridge.series_terms);
    problem.edge_terms = std::max(1, geometry.ridge.edge_terms);

    const double relative_permeability = std::real(shell.relative_permeability);
    const int per_class = std::max(1, std::max(request.settings.maximum_n,
                                               request.excitation.automatic
                                                   ? 1
                                                   : request.excitation.q));
    const bool layered = std::abs(core_permittivity - shell_permittivity) >
                         1.0e-9 * std::max(1.0, shell_permittivity);

    for (int g1 = 0; g1 <= 1; ++g1) {
        for (int g2 = 0; g2 <= 1; ++g2) {
            for (const ModeFamily family : {ModeFamily::TransverseElectric,
                                            ModeFamily::TransverseMagnetic}) {
                if (control.isCancellationRequested()) {
                    solution.cancelled = true;
                    solution.error_message = "Calculation cancelled.";
                    return solution;
                }
                problem.g1 = g1;
                problem.g2 = g2;
                problem.family = family;
                const std::vector<double> cutoffs =
                    findRidgedCircularCutoffs(problem, per_class, control);
                for (size_t index = 0; index < cutoffs.size(); ++index) {
                    ModeDescriptor mode;
                    mode.family = family;
                    mode.symmetry_g1 = g1;
                    mode.symmetry_g2 = g2;
                    mode.order_q = static_cast<int>(index) + 1;
                    mode.cutoff_wavenumber_per_m = cutoffs[index] / geometry.inner_radius_m;
                    mode.cutoff_frequency_hz = speed_of_light_m_per_s *
                                               mode.cutoff_wavenumber_per_m / (2.0 * pi);
                    mode.propagating = request.frequency_hz > mode.cutoff_frequency_hz;
                    if (!layered) {
                        // Однородное заполнение: мода остаётся чистой H или E,
                        // поперечное собственное число от частоты не зависит, и
                        // постоянная распространения берётся точно.
                        const double free_space_wavenumber =
                            2.0 * pi * request.frequency_hz / speed_of_light_m_per_s;
                        const double scale =
                            std::sqrt(shell_permittivity * relative_permeability);
                        const Complex squared =
                            scale * scale *
                            Complex(mode.cutoff_wavenumber_per_m *
                                            mode.cutoff_wavenumber_per_m -
                                        free_space_wavenumber * free_space_wavenumber,
                                    0.0);
                        Complex root = std::sqrt(squared);
                        if (std::real(root) < 0.0 ||
                            (std::abs(std::real(root)) < numerical_tolerance &&
                             std::imag(root) < 0.0)) {
                            root = -root;
                        }
                        mode.propagation_constant_per_m = root;
                    }
                    solution.available_modes.push_back(mode);
                }
            }
        }
    }

    std::sort(solution.available_modes.begin(),
              solution.available_modes.end(),
              [](const ModeDescriptor &left, const ModeDescriptor &right) {
                  return left.cutoff_frequency_hz < right.cutoff_frequency_hz;
              });

    solution.success = true;
    if (solution.available_modes.empty()) {
        solution.diagnostics.warnings.push_back(
            "Спектр структуры пуст: проверьте радиус раздела и углы гребня.");
        return solution;
    }

    const auto selected = std::find_if(
        solution.available_modes.begin(),
        solution.available_modes.end(),
        [&request](const ModeDescriptor &mode) {
            if (request.excitation.automatic) {
                return mode.propagating;
            }
            return mode.family == request.excitation.family &&
                   mode.symmetry_g1 == request.excitation.g1 &&
                   mode.symmetry_g2 == request.excitation.g2 &&
                   mode.order_q == request.excitation.q;
        });
    if (selected == solution.available_modes.end()) {
        solution.diagnostics.warnings.push_back(
            request.excitation.automatic
                ? "No propagating mode exists at the requested frequency."
                : "The requested mode is outside the enumerated mode set.");
        return solution;
    }

    solution.selected_mode = *selected;
    solution.has_selected_mode = true;
    problem.g1 = solution.selected_mode.symmetry_g1;
    problem.g2 = solution.selected_mode.symmetry_g2;
    problem.family = solution.selected_mode.family;
    const std::vector<double> coefficients = ridgedCircularEdgeCoefficients(
        problem, solution.selected_mode.cutoff_wavenumber_per_m * geometry.inner_radius_m);
    solution.forward_longitudinal_amplitude = 1.0;
    solution.field = std::make_shared<RidgedCircularModeField>(
        request, problem, solution.selected_mode, coefficients, 1.0);

    if (layered) {
        solution.diagnostics.warnings.push_back(
            "Слоистое заполнение: моды гибридные (HE/EH). Критические волновые "
            "числа и поперечная структура поля посчитаны точно, а постоянная "
            "распространения вне отсечки требует полного гибридного "
            "определителя и здесь не вычисляется.");
    }
    if (!solution.selected_mode.propagating) {
        solution.diagnostics.warnings.push_back(
            "Выбранная мода на этой частоте заперта.");
    }
    return solution;
}
}
