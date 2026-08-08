#pragma once

// Функции Бесселя первого рода и их нули — то, чем задаются моды круглого
// волновода. Для TM-мод критическое волновое число k_c = p_mn / a, где p_mn —
// n-й положительный нуль J_m; для TE-мод k_c = p'_mn / a, где p'_mn — n-й
// положительный нуль производной J_m'.
namespace em
{
// J_m(x) для целого порядка m >= 0.
double besselJ(int order, double argument);

// Производная dJ_m/dx, вычисленная по тождеству
// J_m'(x) = (J_{m-1}(x) - J_{m+1}(x)) / 2, а для m = 0 как -J_1(x).
double besselJDerivative(int order, double argument);

// n-й (n >= 1) положительный нуль J_m. Возвращает 0 при недопустимых
// аргументах или если нуль не удалось локализовать.
double besselJZero(int order, int index);

// n-й (n >= 1) положительный нуль J_m'. Тривиальный нуль в x = 0, который есть
// у J_m' при m >= 2, не считается.
double besselJDerivativeZero(int order, int index);

// ----------------------------------------------------- вещественный порядок --
// У круглого волновода с гребнями порядок nu = m*pi/phi + g*pi/(2*phi) нецелый,
// а аргумент бывает много меньше порядка. Тогда J_nu(x) исчезает, а Y_nu(x)
// переполняет double задолго до того, как ряд по m перестаёт быть нужным:
// например Y_130(0.35) уже вне диапазона. Поэтому наружу отдаются не только
// сами функции, но и отношения соседних порядков и логарифмы модуля — из них
// собираются те комбинации, которые остаются конечными.

double besselJ(double order, double argument);
double besselY(double order, double argument);

// Производные по аргументу через тождества J_nu' = nu/x * J_nu - J_{nu+1} и
// Y_nu' = nu/x * Y_nu - Y_{nu+1}: они не требуют порядка nu - 1, который при
// nu < 1 выходит за область определения библиотечных функций.
double besselJDerivative(double order, double argument);
double besselYDerivative(double order, double argument);

// J_{nu+1}(x) / J_nu(x) — непрерывная дробь, сходящаяся снизу вверх по порядку.
// Конечна и там, где обе функции по отдельности исчезают в нуль машинного нуля.
double besselJRatio(double order, double argument);

// Y_{nu+1}(x) / Y_nu(x) — рекурсия вверх по порядку, устойчивая для функции
// второго рода. Конечна и там, где обе функции переполняют double.
double besselYRatio(double order, double argument);

// ln|J_nu(x)| и ln|Y_nu(x)|; знак возвращается через sign, если он не nullptr.
double besselJLogMagnitude(double order, double argument, double *sign = nullptr);
double besselYLogMagnitude(double order, double argument, double *sign = nullptr);
}
