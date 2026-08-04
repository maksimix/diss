// Проверки хранения проекта: папка с моделью и подпапкой Result должна
// переноситься на другой ПК целиком, а готовый расчёт — подниматься из кэша без
// повторного счёта. Именно это ломалось, когда сохранённый проект всё равно
// требовал пересчёта.
#include "model_serialization.h"
#include "waveguide_types.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QTemporaryDir>

#include <cstdio>
#include <cstdlib>

namespace
{
int failures = 0;

void check(bool condition, const char *what)
{
    if (!condition) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

// Модель с пластиной и щелью: чем больше заполненных полей, тем строже проверка
// кругового обхода JSON.
WaveguideParameters makeModel()
{
    WaveguideParameters parameters;
    parameters.width_mm = 22.86;
    parameters.depth_mm = 10.16;
    parameters.length_mm = 47.5;
    parameters.frequency_ghz = 9.75;
    parameters.accuracy_level = 2;
    parameters.solver_method = 4;
    parameters.linear_solver_method = 1;
    parameters.mode_automatic = false;
    parameters.mode_family = 0;
    parameters.mode_m = 2;
    parameters.mode_n = 1;
    parameters.slot_enabled = true;
    parameters.slot_length_mm = 11.5;
    parameters.slot_offset_z_mm = 3.25;

    PecPlateParameters iris;
    iris.name = QStringLiteral("iris_1");
    iris.aperture_enabled = true;
    iris.aperture_width_mm = 9.5;
    iris.aperture_height_mm = 5.5;
    iris.post_enabled = true;
    parameters.pec_plates.push_back(iris);
    return parameters;
}

// Результат с полями и срезом: проверяем, что из кэша поднимаются не только
// числа, но и картинка поля.
WaveguideCalculationResult makeResult(const WaveguideParameters &parameters)
{
    WaveguideCalculationResult result;
    result.parameters = parameters;
    result.valid = true;
    result.has_propagating_mode = true;
    result.inner_width_mm = 22.66;
    result.inner_depth_mm = 9.96;
    result.guide_wavelength_mm = 39.7;
    result.s11_magnitude = 0.31;
    result.s21_magnitude = 0.95;
    result.solver_backend = QStringLiteral("FEM");

    WaveguideMode mode;
    mode.name = QStringLiteral("TE10");
    mode.cutoff_ghz = 6.55;
    mode.propagates = true;
    result.modes.push_back(mode);
    result.selected_mode = mode;

    FieldGlyph glyph;
    glyph.type = FieldGlyphType::ElectricArrow;
    glyph.points = {QVector3D(0.0f, 0.0f, 0.0f), QVector3D(0.0f, 4.0f, 0.0f)};
    glyph.color = QColor(220, 60, 60);
    glyph.magnitude = 1234.5;
    result.field_glyphs.push_back(glyph);

    FieldSliceCell cell;
    cell.center = QVector3D(1.0f, 0.0f, 2.0f);
    cell.envelope = 987.6;
    result.horizontal_slice.valid = true;
    result.horizontal_slice.cells.push_back(cell);
    result.horizontal_slice.maximum_value = 987.6;
    return result;
}

// Сохраняет проект так же, как это делает окно: модель в <папка>/<имя>.wgproj,
// расчёт в <папка>/Result/<имя>.wgr.
bool writeProject(const QString &project_file,
                  const WaveguideParameters &parameters,
                  const WaveguideCalculationResult &result)
{
    QString error;
    if (!model_io::ensureProjectLayout(project_file, &error)) {
        std::printf("FAIL: не создалась структура проекта: %s\n", error.toUtf8().constData());
        ++failures;
        return false;
    }
    if (!model_io::saveModel(project_file, parameters, nullptr, &error)) {
        std::printf("FAIL: модель не сохранилась: %s\n", error.toUtf8().constData());
        ++failures;
        return false;
    }
    if (!model_io::saveResults(model_io::projectResultsPathFor(project_file),
                               parameters,
                               result,
                               &error)) {
        std::printf("FAIL: расчёт не сохранился: %s\n", error.toUtf8().constData());
        ++failures;
        return false;
    }
    return true;
}

// Копирование папки целиком — так проект переезжает на другой ПК.
bool copyDirectory(const QString &from, const QString &to)
{
    QDir source(from);
    if (!QDir().mkpath(to)) {
        return false;
    }
    for (const QFileInfo &entry :
         source.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString target = QDir(to).filePath(entry.fileName());
        const bool copied = entry.isDir() ? copyDirectory(entry.absoluteFilePath(), target)
                                          : QFile::copy(entry.absoluteFilePath(), target);
        if (!copied) {
            return false;
        }
    }
    return true;
}

void testProjectSurvivesMove()
{
    QTemporaryDir home;
    check(home.isValid(), "временная папка не создалась");
    if (!home.isValid()) {
        return;
    }

    const QString project_dir = QDir(home.path()).filePath(QStringLiteral("wr90_iris"));
    const QString project_file = QDir(project_dir).filePath(QStringLiteral("wr90_iris.wgproj"));
    const WaveguideParameters parameters = makeModel();
    const WaveguideCalculationResult result = makeResult(parameters);
    if (!writeProject(project_file, parameters, result)) {
        return;
    }

    check(QFileInfo::exists(project_file), "файла модели нет на диске");
    check(QFileInfo::exists(QDir(project_dir).filePath(QStringLiteral("Result"))),
          "подпапки Result нет");
    check(QFileInfo::exists(model_io::projectResultsPathFor(project_file)),
          "файла расчёта нет в Result");

    // Переезд: копируем папку в другое место и работаем только с копией, как это
    // было бы на другом ПК.
    const QString moved_dir = QDir(home.path()).filePath(QStringLiteral("other_pc/wr90_iris"));
    check(copyDirectory(project_dir, moved_dir), "папка проекта не скопировалась");
    const QString moved_file = QDir(moved_dir).filePath(QStringLiteral("wr90_iris.wgproj"));

    WaveguideParameters loaded;
    QString error;
    check(model_io::loadModel(moved_file, &loaded, nullptr, &error),
          "перенесённая модель не открылась");

    WaveguideCalculationResult cached;
    const bool cache_ok = model_io::loadResults(model_io::projectResultsPathFor(moved_file),
                                                loaded,
                                                &cached,
                                                &error);
    if (!cache_ok) {
        std::printf("FAIL: расчёт после переноса отвергнут: %s\n", error.toUtf8().constData());
        ++failures;
        return;
    }

    // Пересчёт не нужен ровно тогда, когда из кэша поднялось всё, что рисуется.
    check(cached.valid, "поднятый расчёт помечен недействительным");
    check(cached.has_propagating_mode, "потерян признак распространяющейся моды");
    check(cached.selected_mode.name == QStringLiteral("TE10"), "потеряна выбранная мода");
    check(cached.field_glyphs.size() == 1, "потеряны стрелки поля");
    check(cached.horizontal_slice.cells.size() == 1, "потерян срез |E|");
    check(std::abs(cached.s21_magnitude - 0.95) < 1.0e-9, "потеряны S-параметры");
    check(loaded.mode_m == 2 && !loaded.mode_automatic, "потерян ручной выбор моды");
    check(loaded.pec_plates.size() == 1 && loaded.pec_plates[0].post_enabled,
          "потеряна диафрагма со штырём");
}

// Смена вида корпуса — свойство отображения: кэш расчёта она обесценивать не
// должна, иначе прозрачность стенок заставляла бы считать заново.
void testDisplayModeKeepsCache()
{
    QTemporaryDir home;
    if (!home.isValid()) {
        check(false, "временная папка не создалась");
        return;
    }

    const QString project_file =
        QDir(home.path()).filePath(QStringLiteral("view/view.wgproj"));
    WaveguideParameters parameters = makeModel();
    parameters.shell_display = 0;
    if (!writeProject(project_file, parameters, makeResult(parameters))) {
        return;
    }

    WaveguideParameters with_other_view = parameters;
    with_other_view.shell_display = 2;   // полупрозрачный корпус

    WaveguideCalculationResult cached;
    QString error;
    check(model_io::loadResults(model_io::projectResultsPathFor(project_file),
                                with_other_view,
                                &cached,
                                &error),
          "смена вида корпуса обесценила кэш расчёта");
}

// Обратная проверка: настоящая правка геометрии кэш обязана отвергнуть, иначе
// на экране показывалось бы поле от другой модели.
void testGeometryChangeRejectsCache()
{
    QTemporaryDir home;
    if (!home.isValid()) {
        check(false, "временная папка не создалась");
        return;
    }

    const QString project_file =
        QDir(home.path()).filePath(QStringLiteral("geom/geom.wgproj"));
    const WaveguideParameters parameters = makeModel();
    if (!writeProject(project_file, parameters, makeResult(parameters))) {
        return;
    }

    WaveguideParameters wider = parameters;
    wider.width_mm += 1.5;

    WaveguideCalculationResult cached;
    QString error;
    check(!model_io::loadResults(model_io::projectResultsPathFor(project_file),
                                 wider,
                                 &cached,
                                 &error),
          "кэш приняли для изменённой геометрии");
}
}

int main()
{
    testProjectSurvivesMove();
    testDisplayModeKeepsCache();
    testGeometryChangeRejectsCache();

    if (failures != 0) {
        std::printf("Провалено проверок: %d\n", failures);
        return EXIT_FAILURE;
    }
    std::printf("Хранение проекта: все проверки пройдены\n");
    return EXIT_SUCCESS;
}
