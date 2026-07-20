#pragma once

#include "em_math.h"
#include "em_solution.h"

namespace em
{
inline ComplexVec3 surfaceCurrent(const Vec3 &normal_from_metal_to_field,
                                  const ComplexVec3 &magnetic_a_per_m)
{
    return cross(normal_from_metal_to_field, magnetic_a_per_m);
}

inline ComplexVec3 volumeConductionCurrent(double conductivity_s_per_m,
                                            const ComplexVec3 &electric_v_per_m)
{
    return electric_v_per_m * Complex(conductivity_s_per_m, 0.0);
}

inline ComplexVec3 displacementCurrent(double angular_frequency_rad_per_s,
                                        const Complex &permittivity_f_per_m,
                                        const ComplexVec3 &electric_v_per_m)
{
    const Complex imaginary_unit(0.0, 1.0);
    return electric_v_per_m *
           (imaginary_unit * angular_frequency_rad_per_s * permittivity_f_per_m);
}

inline Vec3 instantaneousElectric(const FieldPhasor &field, double phase_rad)
{
    return realAtPhase(field.electric_v_per_m, phase_rad);
}

inline Vec3 instantaneousMagnetic(const FieldPhasor &field, double phase_rad)
{
    return realAtPhase(field.magnetic_a_per_m, phase_rad);
}
}
