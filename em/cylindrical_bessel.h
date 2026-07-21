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
}
