#pragma once

#include <cmath>
#include <complex>

namespace em
{
using Complex = std::complex<double>;

constexpr double pi = 3.1415926535897932384626433832795;
constexpr double speed_of_light_m_per_s = 299792458.0;
constexpr double vacuum_permeability_h_per_m = 1.25663706212e-6;
constexpr double vacuum_permittivity_f_per_m =
    1.0 / (vacuum_permeability_h_per_m * speed_of_light_m_per_s * speed_of_light_m_per_s);

struct Vec2
{
    double x = 0.0;
    double y = 0.0;
};

struct Vec3
{
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

struct ComplexVec3
{
    Complex x = 0.0;
    Complex y = 0.0;
    Complex z = 0.0;
};

inline Vec3 operator+(const Vec3 &left, const Vec3 &right)
{
    return {left.x + right.x, left.y + right.y, left.z + right.z};
}

inline Vec3 operator-(const Vec3 &left, const Vec3 &right)
{
    return {left.x - right.x, left.y - right.y, left.z - right.z};
}

inline Vec3 operator*(const Vec3 &vector, double scale)
{
    return {vector.x * scale, vector.y * scale, vector.z * scale};
}

inline Vec3 operator/(const Vec3 &vector, double scale)
{
    return {vector.x / scale, vector.y / scale, vector.z / scale};
}

inline ComplexVec3 operator+(const ComplexVec3 &left, const ComplexVec3 &right)
{
    return {left.x + right.x, left.y + right.y, left.z + right.z};
}

inline ComplexVec3 operator-(const ComplexVec3 &left, const ComplexVec3 &right)
{
    return {left.x - right.x, left.y - right.y, left.z - right.z};
}

inline ComplexVec3 operator*(const ComplexVec3 &vector, const Complex &scale)
{
    return {vector.x * scale, vector.y * scale, vector.z * scale};
}

inline ComplexVec3 operator*(const Complex &scale, const ComplexVec3 &vector)
{
    return vector * scale;
}

inline ComplexVec3 operator/(const ComplexVec3 &vector, const Complex &scale)
{
    return {vector.x / scale, vector.y / scale, vector.z / scale};
}

inline double dot(const Vec3 &left, const Vec3 &right)
{
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

inline double magnitude(const Vec3 &vector)
{
    return std::sqrt(dot(vector, vector));
}

inline double magnitude(const ComplexVec3 &vector)
{
    return std::sqrt(std::norm(vector.x) + std::norm(vector.y) + std::norm(vector.z));
}

inline Vec3 normalized(const Vec3 &vector)
{
    const double vector_magnitude = magnitude(vector);
    return vector_magnitude > 0.0 ? vector / vector_magnitude : Vec3{};
}

inline ComplexVec3 conjugate(const ComplexVec3 &vector)
{
    return {std::conj(vector.x), std::conj(vector.y), std::conj(vector.z)};
}

inline ComplexVec3 cross(const ComplexVec3 &left, const ComplexVec3 &right)
{
    return {
        left.y * right.z - left.z * right.y,
        left.z * right.x - left.x * right.z,
        left.x * right.y - left.y * right.x,
    };
}

inline ComplexVec3 cross(const Vec3 &left, const ComplexVec3 &right)
{
    return {
        left.y * right.z - left.z * right.y,
        left.z * right.x - left.x * right.z,
        left.x * right.y - left.y * right.x,
    };
}

inline Vec3 realAtPhase(const ComplexVec3 &phasor, double phase_rad)
{
    const Complex phase = std::polar(1.0, phase_rad);
    return {
        std::real(phasor.x * phase),
        std::real(phasor.y * phase),
        std::real(phasor.z * phase),
    };
}

inline Vec3 timeAveragePoynting(const ComplexVec3 &electric,
                                const ComplexVec3 &magnetic)
{
    const ComplexVec3 complex_power = cross(electric, conjugate(magnetic));
    return {
        0.5 * std::real(complex_power.x),
        0.5 * std::real(complex_power.y),
        0.5 * std::real(complex_power.z),
    };
}

inline bool isFinite(const ComplexVec3 &vector)
{
    return std::isfinite(std::real(vector.x)) && std::isfinite(std::imag(vector.x)) &&
           std::isfinite(std::real(vector.y)) && std::isfinite(std::imag(vector.y)) &&
           std::isfinite(std::real(vector.z)) && std::isfinite(std::imag(vector.z));
}
}
