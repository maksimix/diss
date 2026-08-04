#pragma once

#include "waveguide_types.h"

#include <QtCore/QTimer>
#include <QtWidgets/QWidget>

// Маленький вращающийся предпросмотр модели для карточки недавнего проекта.
//
// Рисуется обычным QPainter, а не OpenGL: карточек на стартовой странице бывает
// с десяток, и заводить под каждую свой графический контекст расточительно.
// Каркас корпуса, диафрагмы, пластины и свободные тела дают достаточно узнаваемый
// силуэт, чтобы отличить проекты один от другого с одного взгляда.
class ProjectPreviewWidget : public QWidget
{
    Q_OBJECT

public:
    explicit ProjectPreviewWidget(QWidget *parent = nullptr);

    void setModel(const WaveguideParameters &parameters);
    // Вращение идёт только у видимых карточек: скрытая страница не должна
    // будить таймер и жечь батарею.
    void setSpinning(bool spinning);

protected:
    void paintEvent(QPaintEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    QPointF project(const QVector3D &point) const;

    WaveguideParameters parameters_;
    QTimer timer_;
    double azimuth_deg_ = 35.0;
    double scale_ = 1.0;
    bool has_model_ = false;
};
