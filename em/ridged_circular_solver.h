#pragma once

#include "em_solution.h"
#include "em_solver.h"

#include <string>
#include <vector>

// Круглый волновод с бесконечно тонкими радиальными гребнями, кольцевыми
// сегментами и слоистым диэлектрическим заполнением — численно-аналитический
// метод частичных областей с учётом особенности поля на кромке гребня.
//
// Геометрия сечения (r2 — радиус внешней стенки, r1 — граница частичных
// областей):
//
//   * область I  — r < r1, любой угол сектора, проницаемость eps1;
//   * область II — r1 < r < r2, угол 0..phi2, проницаемость eps2;
//   * гребень    — металл при phi2 < phi < phi1 и r > r1 (при phi2 = phi1
//                  гребень вырождается в бесконечно тонкую перегородку);
//   * кольцевой сегмент — бесконечно тонкий металл на r = r1 при
//                  phi3 < phi < phi2; окно связи областей — при phi < phi3.
//
// Сечение симметрично относительно плоскостей phi = 0 и phi = phi1, поэтому
// считается один сектор с граничными условиями g1 (на phi = phi1, r < r1) и
// g2 (на phi = 0): g = 0 — электрическая стенка, g = 1 — магнитная. Четыре пары
// (g1, g2) дают четыре независимых спектра, из которых и складывается полный
// модовый состав структуры; мода обозначается H^q_{g1,g2} или E^q_{g1,g2}, где
// q — её номер в спектре своей пары.
//
// Источник постановки: метод частичных областей с гегенбауэровским базисом на
// окне связи, обеспечивающим правильное поведение поля у кромки (E_phi ~
// rho^{tau-1}, E_z ~ rho^{tau}).
namespace em
{
// Нормированная постановка: все радиусы отнесены к r2, поэтому собственным
// числом задачи является kc*r2 — та самая безразмерная величина, которую
// приводят таблицы критических волновых чисел.
struct RidgedCircularProblem
{
    double radius_ratio = 0.35;             // r1 / r2
    double sector_angle_rad = 0.5 * pi;     // phi1
    double ridge_angle_rad = 0.5 * pi;      // phi2 <= phi1
    double aperture_angle_rad = 0.5 * pi;   // phi3 <= phi2
    double core_permittivity = 1.0;         // eps1, r < r1
    double shell_permittivity = 1.0;        // eps2, r1 < r < r2
    int series_terms = 60;                  // M — членов ряда по собственным функциям
    int edge_terms = 3;                     // N — членов базиса на окне связи
    int g1 = 1;
    int g2 = 0;
    ModeFamily family = ModeFamily::TransverseElectric;
};

// Показатель особенности поля у кромки. Кромка кольцевого сегмента лежит в
// однородной среде (tau = 1/2), кромка гребня — на границе двух диэлектриков,
// и тогда показатель определяется их отношением.
double ridgeEdgeSingularityExponent(const RidgedCircularProblem &problem);

// Нормированные критические волновые числа kc*r2 первых count мод спектра
// заданной пары (g1, g2). Возвращает их по возрастанию; пустой вектор означает,
// что в просмотренном диапазоне корней нет.
std::vector<double> findRidgedCircularCutoffs(const RidgedCircularProblem &problem,
                                              int count,
                                              const SolveControl &control = {});

// Собственный вектор Q системы в найденном корне: коэффициенты разложения
// краевой функции на окне связи, из которых восстанавливается поле.
std::vector<double> ridgedCircularEdgeCoefficients(const RidgedCircularProblem &problem,
                                                   double normalized_cutoff);

// Радиальная функция частичной области, уже поделённая на своё значение
// (E-волны) или на свою производную (H-волны) на границе раздела r1. Деление
// внесено внутрь потому, что по отдельности числитель и знаменатель при
// большом порядке выходят за пределы double: Y_nu(x) растёт как
// Gamma(nu) (2/x)^nu, а J_nu(x) ровно на столько же убывает. Здесь хранится не
// значение, а логарифмы и логарифмические производные, из которых значение
// собирается в любой точке области без переполнения.
struct RidgedRadialProfile
{
    double order = 0.0;
    double inner_argument = 0.0;    // beta_w * r1
    double outer_argument = 0.0;    // beta_w * r2 (только область 2)
    bool inner_region = true;
    bool neumann_outer = true;      // H-волны: dR/dr = 0 на стенке; E-волны: R = 0
    bool divide_by_derivative = true;
    double log_inner_j = 0.0;
    double sign_inner_j = 1.0;
    double log_inner_y = 0.0;
    double sign_inner_y = 1.0;
    double log_outer_factor = 0.0;  // ln|L|, L = J'(x2)/Y'(x2) или J(x2)/Y(x2)
    double sign_outer_factor = 1.0;
    double inner_log_derivative_j = 0.0;   // J'/J в x1
    double inner_log_derivative_y = 0.0;   // Y'/Y в x1
    double normalization = 1.0;
    bool valid = false;
};

// Поле моды. Продольная составляющая (H_z у H-волн, E_z у E-волн) строится
// рядами частичных областей, поперечные — по уравнениям Максвелла. Поле
// продолжается с расчётного сектора на всё сечение отражениями относительно
// плоскостей симметрии, со знаком, который диктуют g1 и g2.
class RidgedCircularModeField final : public IFieldEvaluator
{
public:
    RidgedCircularModeField(const SimulationRequest &request,
                            const RidgedCircularProblem &problem,
                            const ModeDescriptor &mode,
                            const std::vector<double> &edge_coefficients,
                            Complex forward_longitudinal_amplitude);

    bool contains(const Vec3 &position_m) const override;
    FieldPhasor evaluate(const Vec3 &position_m) const override;

private:
    // Один член ряда: угловая функция, коэффициент разложения и вся радиальная
    // подготовка. Радиальный профиль считается один раз при построении моды —
    // при обходе поперечного сечения он же переиспользуется в каждой точке.
    struct Term
    {
        int region = 1;              // 1 или 2
        double angular_order = 0.0;  // nu_{w,m}
        double coefficient = 0.0;    // C^w_m
        RidgedRadialProfile radial;
    };

    WaveguideGeometry geometry_;
    RidgedCircularProblem problem_;
    ModeDescriptor mode_;
    double angular_frequency_rad_per_s_ = 0.0;
    Complex permeability_h_per_m_ = vacuum_permeability_h_per_m;
    Complex core_permittivity_f_per_m_ = vacuum_permittivity_f_per_m;
    Complex shell_permittivity_f_per_m_ = vacuum_permittivity_f_per_m;
    Complex forward_longitudinal_amplitude_ = 1.0;
    double transverse_wavenumber_per_m_[3] = {0.0, 0.0, 0.0};  // [1] и [2]
    std::vector<Term> terms_;
};

// Решатель структуры. Считает модовый состав всех четырёх пар граничных
// условий, выбирает возбуждаемую моду и строит её поле.
class RidgedCircularWaveguideSolver final : public IEmSolver
{
public:
    static bool canSolve(const SimulationRequest &request, std::string *reason = nullptr);

    FieldSolution solve(const SimulationRequest &request,
                        const SolveControl &control = {}) const override;
};
}
