cmake_minimum_required(VERSION 3.24)
project(krutiev_waveguide LANGUAGES CXX)

option(KRUTIEV_ENABLE_MFEM "Build the complex H(curl) FEM backend" ON)
option(KRUTIEV_BUILD_GUI "Build the Qt/OpenGL application" OFF)
set(KRUTIEV_MFEM_SOURCE_DIR "" CACHE PATH "Optional MFEM source include directory")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(EM_CORE_SOURCES
    em/circular_mode_field.cpp
    em/cylindrical_bessel.cpp
    em/em_solver_dispatcher.cpp
    em/fem_frequency_domain_solver.cpp
    em/gmsh_tetrahedral_mesher.cpp
    em/mode_matching_iris_solver.cpp
    em/rectangular_mode_field.cpp
    em/analytic_waveguide_solver.cpp
    em/transverse_pec_partition_solver.cpp
    postprocessing/field_visualization_generator.cpp
    postprocessing/slot_excitation_estimator.cpp)

add_library(krutiev_em_core STATIC ${EM_CORE_SOURCES})
target_include_directories(krutiev_em_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(KRUTIEV_ENABLE_MFEM)
    find_package(MFEM CONFIG QUIET)
    if(NOT MFEM_FOUND)
        include(FetchContent)
        set(MFEM_USE_MPI OFF CACHE BOOL "" FORCE)
        set(MFEM_USE_METIS OFF CACHE BOOL "" FORCE)
        set(MFEM_USE_LAPACK OFF CACHE BOOL "" FORCE)
        set(MFEM_USE_OPENMP OFF CACHE BOOL "" FORCE)
        set(MFEM_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(MFEM_ENABLE_MINIAPPS OFF CACHE BOOL "" FORCE)
        set(MFEM_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(
            mfem
            GIT_REPOSITORY https://github.com/mfem/mfem.git
            GIT_TAG v4.8)
        FetchContent_MakeAvailable(mfem)
    endif()
    target_sources(krutiev_em_core PRIVATE em/mfem_frequency_domain_backend.cpp)
    target_compile_definitions(krutiev_em_core PUBLIC KRUTIEV_WITH_MFEM=1)
    if(KRUTIEV_MFEM_SOURCE_DIR)
        target_include_directories(krutiev_em_core BEFORE PRIVATE ${KRUTIEV_MFEM_SOURCE_DIR})
        if(MFEM_DIR)
            file(TO_CMAKE_PATH "${MFEM_DIR}/config/_config.hpp" KRUTIEV_MFEM_CONFIG_PATH)
            target_compile_definitions(krutiev_em_core PRIVATE
                KRUTIEV_MFEM_CONFIG_FILE="${KRUTIEV_MFEM_CONFIG_PATH}")
        endif()
    endif()
    if(TARGET mfem)
        target_link_libraries(krutiev_em_core PUBLIC mfem)
    elseif(TARGET MFEM::mfem)
        target_link_libraries(krutiev_em_core PUBLIC MFEM::mfem)
    else()
        message(FATAL_ERROR "MFEM target was not found")
    endif()
endif()

add_executable(em_core_tests tests/em_core_tests.cpp)
target_link_libraries(em_core_tests PRIVATE krutiev_em_core)

enable_testing()
add_test(NAME em_core_tests COMMAND em_core_tests)

if(KRUTIEV_BUILD_GUI)
    find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets)
    qt_standard_project_setup()

    # Вычислитель выражений списка параметров зависит только от QtCore, поэтому
    # проверяется отдельным консольным тестом без окна.
    add_executable(parameter_expression_tests
        model_parameters.cpp
        tests/parameter_expression_tests.cpp)
    target_include_directories(parameter_expression_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(parameter_expression_tests PRIVATE Qt6::Core)
    add_test(NAME parameter_expression_tests COMMAND parameter_expression_tests)

    qt_add_executable(krutiev
        calculation_worker.cpp
        cst_ribbon.cpp
        expression_spin_box.cpp
        main.cpp
        main_window.cpp
        model_parameters.cpp
        model_serialization.cpp
        parameter_list_widget.cpp
        postprocessing/qt_field_glyph_adapter.cpp
        waveguide_calculator.cpp
        waveguide_opengl_widget.cpp)
    target_link_libraries(krutiev PRIVATE krutiev_em_core Qt6::Core Qt6::Gui
                          Qt6::Widgets Qt6::OpenGL Qt6::OpenGLWidgets opengl32)
endif()
