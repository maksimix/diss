#include "main_window.h"
#include "waveguide_types.h"

#include <QtGui/QSurfaceFormat>
#include <QtWidgets/QApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    // Официальные сведения о ПО: отсюда их берут заголовок окна, стартовая
    // страница, окно «О программе» и путь к настройкам (QSettings без
    // аргументов). Менять название и версию нужно только здесь.
    // setApplicationDisplayName здесь не задаётся намеренно: Qt дописывал бы его
    // ко всем заголовкам окон, и получалось «проект — ПО 1.0 - ПО».
    QApplication::setOrganizationName(QStringLiteral("EM Waveguide Studio"));
    QApplication::setApplicationName(QStringLiteral("EM Waveguide Studio"));
    QApplication::setApplicationVersion(QStringLiteral("1.0"));

    qRegisterMetaType<WaveguideParameters>("WaveguideParameters");
    qRegisterMetaType<WaveguideCalculationResult>("WaveguideCalculationResult");
    qRegisterMetaType<QVector<FieldGlyph>>("QVector<FieldGlyph>");
    qRegisterMetaType<FieldSlice>("FieldSlice");
    qRegisterMetaType<QVector<FieldSlice>>("QVector<FieldSlice>");
    qRegisterMetaType<std::shared_ptr<const em::FieldSolution>>(
        "std::shared_ptr<const em::FieldSolution>");

    QSurfaceFormat surface_format;
    surface_format.setRenderableType(QSurfaceFormat::OpenGL);
    surface_format.setProfile(QSurfaceFormat::CompatibilityProfile);
    surface_format.setVersion(2, 1);
    surface_format.setDepthBufferSize(24);
    surface_format.setSamples(4);
    QSurfaceFormat::setDefaultFormat(surface_format);

    MainWindow window;
    window.resize(1280, 760);
    window.show();

    // Путь к проекту первым аргументом: так открывается двойной щелчок по
    // .wgproj в проводнике. Показ окна идёт раньше, чтобы возможная ошибка
    // чтения легла в строку состояния уже видимого окна.
    //
    // Ключ --run вдобавок сразу запускает решатель: пакетный прогон нескольких
    // проектов подряд и проверка расчёта без нажатия F5 вручную.
    const QStringList arguments = QApplication::arguments();
    QString project_path;
    bool run_after_open = false;
    for (int index = 1; index < arguments.size(); ++index) {
        const QString argument = arguments.at(index);
        if (argument == QStringLiteral("--run")) {
            run_after_open = true;
        } else if (project_path.isEmpty()) {
            project_path = argument;
        }
    }
    if (!project_path.isEmpty()) {
        window.openProjectOnStartup(project_path);
        if (run_after_open) {
            window.startCalculationOnStartup();
        }
    }

    return app.exec();
}
