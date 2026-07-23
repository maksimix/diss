#include "main_window.h"
#include "waveguide_types.h"

#include <QtGui/QSurfaceFormat>
#include <QtWidgets/QApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

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

    return app.exec();
}
