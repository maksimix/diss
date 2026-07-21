#include "main_window.h"

#include "calculation_worker.h"
#include "cst_ribbon.h"
#include "expression_spin_box.h"
#include "model_serialization.h"
#include "parameter_list_widget.h"
#include "waveguide_opengl_widget.h"

#include <QtCore/QFileInfo>
#include <QtCore/QLocale>
#include <QtCore/QSignalBlocker>
#include <QtGui/QAction>
#include <QtGui/QPainter>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialog>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QDockWidget>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QStyle>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QTreeWidgetItem>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace
{
constexpr int object_type_role = Qt::UserRole + 1;
constexpr int object_index_role = Qt::UserRole + 2;

QString number(double value, int precision = 3)
{
    return QLocale::system().toString(value, 'f', precision);
}

QString slotSurfaceName(int surface)
{
    if (surface == 1) {
        return QStringLiteral("right");
    }
    if (surface == 2) {
        return QStringLiteral("bottom");
    }
    if (surface == 3) {
        return QStringLiteral("left");
    }

    return QStringLiteral("top");
}

QString formattedDuration(qint64 milliseconds)
{
    const qint64 total_seconds = std::max<qint64>(0, milliseconds / 1000);
    const qint64 minutes = total_seconds / 60;
    const qint64 seconds = total_seconds % 60;
    return minutes > 0
               ? QStringLiteral("%1 мин %2 с").arg(minutes).arg(seconds, 2, 10, QLatin1Char('0'))
               : QStringLiteral("%1 с").arg(seconds);
}

QString cstStyleSheet()
{
    return QStringLiteral(R"(
QMainWindow, QWidget {
    background: #f4f4f4;
    color: #1f1f1f;
    font-family: "Segoe UI";
    font-size: 9pt;
}
QTreeWidget {
    background: #ffffff;
    border: 1px solid #b9b9b9;
    alternate-background-color: #f7f7f7;
    outline: 0;
}
QTreeWidget::item {
    height: 20px;
    padding: 1px 4px;
}
QTreeWidget::item:selected {
    background: #c8c8c8;
    color: #111111;
}
QTreeWidget::branch:closed:has-children {
    image: none;
}
QTreeWidget::branch:open:has-children {
    image: none;
}
QLineEdit, QDoubleSpinBox, QComboBox, QPlainTextEdit {
    background: #ffffff;
    border: 1px solid #b8b8b8;
    border-radius: 1px;
    padding: 2px 4px;
}
QLineEdit:focus, QDoubleSpinBox:focus, QComboBox:focus {
    border: 1px solid #3aa6d8;
}
QCheckBox {
    spacing: 6px;
}
QPushButton {
    background: #ededed;
    border: 1px solid #9c9c9c;
    border-radius: 2px;
    min-height: 21px;
    padding: 2px 14px;
}
QPushButton:hover {
    background: #f7f7f7;
    border-color: #55a8d7;
}
QPushButton:default {
    border: 1px solid #1590d0;
    background: #eaf6fd;
}
QGroupBox {
    border: 1px solid #c7c7c7;
    margin-top: 8px;
    padding-top: 10px;
    font-weight: 600;
}
QGroupBox::title {
    subcontrol-origin: margin;
    left: 8px;
    padding: 0 3px;
}
QDialog {
    background: #ececec;
}
QDialog QLabel {
    background: transparent;
}
QSplitter::handle {
    background: #d0d0d0;
}
QProgressBar {
    border: 1px solid #9aa5ac;
    background: #dfe4e7;
}
QProgressBar::chunk {
    background: #2aa7d6;
}
QTabBar#cstRibbonTabBar {
    background: #f4f4f4;
}
QTabBar#cstRibbonTabBar::tab {
    background: transparent;
    border: 1px solid transparent;
    border-bottom: none;
    padding: 4px 14px;
    margin-right: 1px;
    color: #24384a;
}
QTabBar#cstRibbonTabBar::tab:hover {
    background: #e2eef7;
}
QTabBar#cstRibbonTabBar::tab:selected {
    background: #ffffff;
    border: 1px solid #c4c4c4;
    border-bottom: 1px solid #ffffff;
    color: #10528a;
    font-weight: 600;
}
QStackedWidget#cstRibbonPages {
    background: #ffffff;
    border: 1px solid #c4c4c4;
}
QStackedWidget#cstRibbonPages QToolButton {
    border: 1px solid transparent;
    border-radius: 2px;
    padding: 2px 4px;
    color: #1f1f1f;
}
QStackedWidget#cstRibbonPages QToolButton:hover {
    background: #e2eef7;
    border-color: #9dc7e4;
}
QStackedWidget#cstRibbonPages QToolButton:pressed {
    background: #c9e2f3;
    border-color: #5fa8d3;
}
QStackedWidget#cstRibbonPages QToolButton:disabled {
    color: #a0a0a0;
}
QStackedWidget#cstRibbonPages QWidget {
    background: #ffffff;
}
QStackedWidget#cstRibbonPages QComboBox,
QStackedWidget#cstRibbonPages QDoubleSpinBox {
    background: #ffffff;
}
QDockWidget {
    titlebar-close-icon: none;
    font-weight: 600;
}
QDockWidget::title {
    background: #e3e3e3;
    border: 1px solid #c4c4c4;
    padding: 3px 6px;
}
QTableWidget {
    background: #ffffff;
    border: 1px solid #b9b9b9;
    gridline-color: #dcdcdc;
    alternate-background-color: #f7f7f7;
}
QHeaderView::section {
    background: #ededed;
    border: 1px solid #c9c9c9;
    padding: 2px 6px;
}
)");
}

}

// Colour-scale legend for the |E| slice fill. The fill uses a logarithmic
// mapping so weak zones stay visible, so the ticks report the real field value
// at a few positions along the (non-linear) scale.
class FieldColorBar : public QWidget
{
public:
    explicit FieldColorBar(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setMinimumHeight(52);
        setMaximumHeight(64);
    }

    void setMaximum(double maximum_value)
    {
        maximum_value_ = maximum_value;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, false);
        const int margin = 8;
        const int caption_height = 15;
        const int tick_height = 15;
        const QRect bar(margin,
                        margin + caption_height,
                        std::max(1, width() - 2 * margin),
                        std::max(6, height() - 2 * margin - caption_height - tick_height));

        for (int x = 0; x < bar.width(); ++x) {
            const double t = bar.width() > 1
                                 ? static_cast<double>(x) / (bar.width() - 1)
                                 : 0.0;
            painter.setPen(fieldHeatColor(t));
            painter.drawLine(bar.left() + x, bar.top(), bar.left() + x, bar.bottom());
        }
        painter.setPen(QColor(120, 120, 120));
        painter.drawRect(bar);

        painter.setPen(palette().color(QPalette::WindowText));
        painter.drawText(QRect(margin, margin, width() - 2 * margin, caption_height),
                         Qt::AlignHCenter | Qt::AlignVCenter,
                         QStringLiteral("|E| на срезе, В/м (лог-шкала)"));

        const double lift = 40.0;
        const auto value_at = [this, lift](double t) {
            return maximum_value_ * (std::pow(1.0 + lift, t) - 1.0) / lift;
        };
        const double ticks[] = {0.0, 0.5, 1.0};
        const Qt::Alignment aligns[] = {Qt::AlignLeft, Qt::AlignHCenter, Qt::AlignRight};
        for (int index = 0; index < 3; ++index) {
            const double t = ticks[index];
            const int x = bar.left() + static_cast<int>(t * bar.width());
            const QString text = maximum_value_ > 0.0
                                     ? QLocale::system().toString(value_at(t), 'g', 3)
                                     : QStringLiteral("—");
            painter.drawText(QRect(x - 60, bar.bottom() + 1, 120, tick_height),
                             aligns[index] | Qt::AlignVCenter,
                             text);
        }
    }

private:
    double maximum_value_ = 0.0;
};

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    updateWindowTitle();
    applyCstStyle();

    open_gl_widget_ = new WaveguideOpenGLWidget(this);
    top_projection_widget_ = new WaveguideOpenGLWidget(this);
    side_projection_widget_ = new WaveguideOpenGLWidget(this);
    top_projection_widget_->setViewPreset(WaveguideViewPreset::Top);
    side_projection_widget_->setViewPreset(WaveguideViewPreset::Side);
    top_projection_widget_->setMinimumSize(260, 180);
    side_projection_widget_->setMinimumSize(260, 180);
    // The top view looks down the y axis (H-plane), the side view looks along x
    // (E-plane), so each projection carries the matching cut plane.
    top_projection_widget_->setSlicePlane(FieldSlicePlane::HorizontalXZ);
    side_projection_widget_->setSlicePlane(FieldSlicePlane::VerticalYZ);

    result_text_edit_ = new QPlainTextEdit(this);
    result_text_edit_->setReadOnly(true);
    result_text_edit_->setMinimumHeight(150);
    result_text_edit_->setMaximumBlockCount(1000);

    QWidget *parameter_panel = createParameterPanel();
    QWidget *projection_panel = createProjectionPanel();
    QWidget *result_panel = createResultPanel();
    createRibbon();
    createParameterDock();

    QSplitter *main_splitter = new QSplitter(Qt::Horizontal, this);
    main_splitter->addWidget(parameter_panel);
    main_splitter->addWidget(open_gl_widget_);
    main_splitter->addWidget(projection_panel);
    main_splitter->setStretchFactor(0, 0);
    main_splitter->setStretchFactor(1, 1);
    main_splitter->setStretchFactor(2, 0);
    main_splitter->setSizes({300, 780, 360});

    QSplitter *vertical_splitter = new QSplitter(Qt::Vertical, this);
    vertical_splitter->addWidget(main_splitter);
    vertical_splitter->addWidget(result_panel);
    vertical_splitter->setStretchFactor(0, 1);
    vertical_splitter->setStretchFactor(1, 0);

    // Лента живёт над центральной областью, как в CST: строка вкладок сразу под
    // заголовком окна, ниже — рабочая область.
    QWidget *central = new QWidget(this);
    QVBoxLayout *central_layout = new QVBoxLayout(central);
    central_layout->setContentsMargins(0, 0, 0, 0);
    central_layout->setSpacing(0);
    central_layout->addWidget(ribbon_bar_);
    central_layout->addWidget(vertical_splitter, 1);
    setCentralWidget(central);

    open_gl_widget_->setSlotEditedCallback([this](const WaveguideParameters &parameters) {
        applyInteractiveSlotParameters(parameters);
    });

    progress_timer_.setInterval(1000);
    connect(&progress_timer_, &QTimer::timeout, this, &MainWindow::updateCalculationProgress);

    worker_ = new CalculationWorker();
    worker_->moveToThread(&worker_thread_);
    connect(&worker_thread_, &QThread::finished, worker_, &QObject::deleteLater);
    connect(this, &MainWindow::requestCalculation, worker_, &CalculationWorker::calculate);
    connect(worker_, &CalculationWorker::calculated, this, &MainWindow::handleCalculationResult);
    connect(worker_, &CalculationWorker::progressed, this, &MainWindow::handleCalculationProgress);
    worker_thread_.start();

    // Стартовое состояние: геометрия показана, решатель ждёт кнопки Start.
    updateModelPreview();
    updateSimulationActionState();
    setStatus(QStringLiteral("Модель готова. Нажмите «Начать расчёт» на вкладке Simulation."),
              false);
}

MainWindow::~MainWindow()
{
    if (worker_ != nullptr) {
        worker_->setLatestRequestId(0);
    }
    worker_thread_.quit();
    worker_thread_.wait();
}

void MainWindow::markModelChanged()
{
    // Изменение геометрии отменяет незавершённый расчёт: его результат уже
    // относился бы к прежней модели.
    latest_request_id_ = 0;
    if (worker_ != nullptr) {
        worker_->setLatestRequestId(0);
    }
    if (calculation_running_) {
        calculation_running_ = false;
        progress_timer_.stop();
        calculation_progress_bar_->setVisible(false);
        calculation_time_label_->setVisible(false);
    }

    model_changed_since_run_ = true;
    updateModelPreview();
    updateSimulationActionState();
    setStatus(QStringLiteral("Модель изменена. Нажмите «Начать расчёт», чтобы пересчитать поле."),
              false);
}

void MainWindow::runCalculation()
{
    if (calculation_running_) {
        return;
    }

    const int request_id = next_request_id_++;
    latest_request_id_ = request_id;
    worker_->setLatestRequestId(request_id);
    active_calculation_is_fem_ = std::any_of(parameters_.pec_plates.cbegin(),
                                             parameters_.pec_plates.cend(),
                                             [](const PecPlateParameters &plate) {
                                                 return plate.enabled;
                                             });
    estimated_duration_ms_ = active_calculation_is_fem_
                                 ? (measured_fem_duration_ms_ > 0
                                        ? measured_fem_duration_ms_
                                        : 120000)
                                 : 3000;
    calculation_running_ = true;
    calculation_stage_ = QStringLiteral("Подготовка модели...");
    calculation_elapsed_timer_.restart();
    calculation_progress_bar_->setValue(0);
    calculation_progress_bar_->setVisible(true);
    calculation_time_label_->setVisible(true);
    progress_timer_.start();
    updateCalculationProgress();
    updateSimulationActionState();
    setStatus(active_calculation_is_fem_
                  ? QStringLiteral("FEM-расчет выполняется: пока показано предыдущее поле, оно еще не учитывает новую геометрию.")
                  : QStringLiteral("Расчет поля выполняется в отдельном потоке..."),
              false);
    emit requestCalculation(request_id, readParameters());
}

void MainWindow::handleCalculationResult(int request_id, const WaveguideCalculationResult &result)
{
    if (request_id != latest_request_id_) {
        return;
    }

    const qint64 elapsed_ms = calculation_elapsed_timer_.isValid()
                                  ? calculation_elapsed_timer_.elapsed()
                                  : 0;
    calculation_running_ = false;
    model_changed_since_run_ = false;
    calculation_stage_.clear();
    progress_timer_.stop();
    updateSimulationActionState();
    calculation_progress_bar_->setValue(100);
    calculation_time_label_->setText(
        QStringLiteral("Завершено за %1").arg(formattedDuration(elapsed_ms)));
    if (active_calculation_is_fem_ && elapsed_ms > 0) {
        measured_fem_duration_ms_ = measured_fem_duration_ms_ > 0
                                        ? (2 * measured_fem_duration_ms_ + elapsed_ms) / 3
                                        : elapsed_ms;
    }

    showResult(result);
}

void MainWindow::showResult(const WaveguideCalculationResult &result)
{
    last_result_ = result;
    result_text_edit_->setPlainText(buildResultText(result));

    if (!result.valid) {
        updateModelPreview();
        setStatus(result.error_message, true);
        return;
    }

    open_gl_widget_->setCalculationResult(result);
    top_projection_widget_->setCalculationResult(result);
    side_projection_widget_->setCalculationResult(result);
    if (color_bar_ != nullptr) {
        color_bar_->setMaximum(std::max(result.horizontal_slice.maximum_value,
                                        result.vertical_slice.maximum_value));
    }
    if (result.has_propagating_mode) {
        setStatus(QStringLiteral("Расчет готов: отображается мода %1.").arg(result.selected_mode.name), false);
    } else {
        setStatus(QStringLiteral("Расчет готов: на заданной частоте распространяющейся моды нет."), true);
    }
}

void MainWindow::updateCalculationProgress()
{
    if (!calculation_running_ || !calculation_elapsed_timer_.isValid()) {
        return;
    }

    const qint64 elapsed_ms = calculation_elapsed_timer_.elapsed();
    const int progress = estimated_duration_ms_ > 0
                             ? std::min(95,
                                        static_cast<int>(100 * elapsed_ms /
                                                         estimated_duration_ms_))
                             : 0;
    calculation_progress_bar_->setValue(progress);
    QString text =
        elapsed_ms < estimated_duration_ms_
            ? QStringLiteral("Прошло: %1   |   Примерно осталось: %2   |   %3%")
                  .arg(formattedDuration(elapsed_ms))
                  .arg(formattedDuration(estimated_duration_ms_ - elapsed_ms))
                  .arg(progress)
            : QStringLiteral(
                  "Прошло: %1   |   Завершение расчета, первоначальная оценка превышена   |   95%+")
                  .arg(formattedDuration(elapsed_ms));
    if (!calculation_stage_.isEmpty()) {
        text += QStringLiteral("\nЭтап: %1").arg(calculation_stage_);
    }
    calculation_time_label_->setText(text);
}

void MainWindow::handleCalculationProgress(int request_id, const QString &stage)
{
    if (request_id != latest_request_id_ || !calculation_running_) {
        return;
    }

    calculation_stage_ = stage;
    updateCalculationProgress();
}

void MainWindow::changeFieldDisplayMode(int index)
{
    FieldDisplayMode display_mode = FieldDisplayMode::Current;
    if (index == 0) {
        display_mode = FieldDisplayMode::Fields;
    } else if (index == 1) {
        display_mode = FieldDisplayMode::Both;
    } else if (index == 2) {
        display_mode = FieldDisplayMode::Electric;
    } else if (index == 3) {
        display_mode = FieldDisplayMode::Magnetic;
    } else if (index == 5) {
        display_mode = FieldDisplayMode::Poynting;
    }

    open_gl_widget_->setFieldDisplayMode(display_mode);
    top_projection_widget_->setFieldDisplayMode(display_mode);
    side_projection_widget_->setFieldDisplayMode(display_mode);
}

QWidget *MainWindow::createParameterPanel()
{
    QWidget *panel = new QWidget(this);
    panel->setObjectName(QStringLiteral("cstProjectPanel"));
    panel->setMinimumWidth(260);
    panel->setMaximumWidth(340);

    QVBoxLayout *panel_layout = new QVBoxLayout(panel);
    panel_layout->setContentsMargins(7, 7, 7, 7);
    panel_layout->setSpacing(7);

    QLineEdit *filter_line_edit = new QLineEdit(panel);
    filter_line_edit->setPlaceholderText(QStringLiteral("<Filter>"));
    panel_layout->addWidget(filter_line_edit);

    object_tree_widget_ = new QTreeWidget(panel);
    object_tree_widget_->setHeaderHidden(true);
    object_tree_widget_->setRootIsDecorated(true);
    object_tree_widget_->setSelectionMode(QAbstractItemView::SingleSelection);
    object_tree_widget_->header()->setStretchLastSection(true);
    panel_layout->addWidget(object_tree_widget_, 1);

    slot_enabled_check_box_ = new QCheckBox(QStringLiteral("Щель включена в модель"), panel);
    slot_enabled_check_box_->setChecked(parameters_.slot_enabled);
    panel_layout->addWidget(slot_enabled_check_box_);

    // Кнопки построения и настройки расчёта переехали на ленту; на панели
    // проекта остаётся дерево объектов и управление отображением.
    QGroupBox *view_group = new QGroupBox(QStringLiteral("Отображение"), panel);
    QFormLayout *view_layout = new QFormLayout(view_group);
    field_mode_combo_box_ = new QComboBox(view_group);
    field_mode_combo_box_->addItem(QStringLiteral("E + H"));
    field_mode_combo_box_->addItem(QStringLiteral("E + H + J"));
    field_mode_combo_box_->addItem(QStringLiteral("Только E"));
    field_mode_combo_box_->addItem(QStringLiteral("Только H"));
    field_mode_combo_box_->addItem(QStringLiteral("Только J"));
    field_mode_combo_box_->addItem(QStringLiteral("Поток мощности S"));
    slice_check_box_ = new QCheckBox(QStringLiteral("Заливка |E| на срезе"), view_group);
    animation_check_box_ = new QCheckBox(QStringLiteral("Анимация бегущей волны"), view_group);
    slice_plane_combo_box_ = new QComboBox(view_group);
    slice_plane_combo_box_->addItem(QStringLiteral("Срез: горизонтальный (y = 0)"));
    slice_plane_combo_box_->addItem(QStringLiteral("Срез: вертикальный (x = 0)"));
    color_bar_ = new FieldColorBar(view_group);
    QPushButton *reset_view_button = new QPushButton(QStringLiteral("Сбросить вид"), view_group);
    view_layout->addRow(QStringLiteral("Поля"), field_mode_combo_box_);
    view_layout->addRow(slice_check_box_);
    view_layout->addRow(slice_plane_combo_box_);
    view_layout->addRow(animation_check_box_);
    view_layout->addRow(color_bar_);
    view_layout->addRow(reset_view_button);
    panel_layout->addWidget(view_group);

    connect(slot_enabled_check_box_,
            &QCheckBox::toggled,
            this,
            [this](bool checked) {
                parameters_.slot_enabled = checked;
                rebuildObjectTree();
                markModelChanged();
            });
    connect(object_tree_widget_,
            &QTreeWidget::itemDoubleClicked,
            this,
            &MainWindow::handleObjectDoubleClick);
    connect(object_tree_widget_,
            &QTreeWidget::itemSelectionChanged,
            this,
            &MainWindow::handleObjectSelectionChanged);
    connect(field_mode_combo_box_,
            qOverload<int>(&QComboBox::currentIndexChanged),
            this,
            &MainWindow::changeFieldDisplayMode);
    connect(slice_check_box_, &QCheckBox::toggled, this, [this](bool checked) {
        open_gl_widget_->setSliceVisible(checked);
        top_projection_widget_->setSliceVisible(checked);
        side_projection_widget_->setSliceVisible(checked);
    });
    connect(animation_check_box_, &QCheckBox::toggled, this, [this](bool checked) {
        open_gl_widget_->setAnimationEnabled(checked);
        top_projection_widget_->setAnimationEnabled(checked);
        side_projection_widget_->setAnimationEnabled(checked);
    });
    connect(slice_plane_combo_box_,
            qOverload<int>(&QComboBox::currentIndexChanged),
            this,
            [this](int index) {
                open_gl_widget_->setSlicePlane(index == 1
                                                   ? FieldSlicePlane::VerticalYZ
                                                   : FieldSlicePlane::HorizontalXZ);
            });
    connect(reset_view_button, &QPushButton::clicked, open_gl_widget_, &WaveguideOpenGLWidget::resetView);

    rebuildObjectTree();
    return panel;
}

// H-plane profile templates. A step machined into the side wall is
// geometrically the same cavity as a PEC block filling that corner, so the
// profile reuses the (already meshed and FEM-routed) plate bodies.
void MainWindow::insertProfileTemplate(bool symmetric)
{
    const double inner_width = parameters_.width_mm - 2.0 * parameters_.wall_thickness_mm;
    const double inner_depth = parameters_.depth_mm - 2.0 * parameters_.wall_thickness_mm;
    const double half_width = 0.5 * inner_width;
    const double half_depth = 0.5 * inner_depth;
    const double section_length = std::min(0.35 * parameters_.length_mm, 12.0);
    // Mild default steps: a deeper narrowing pushes the narrow section below
    // cutoff (evanescent), which is a valid design but a confusing default.
    const double inset = symmetric ? 0.15 * inner_width : 0.25 * inner_width;

    const auto make_step = [&](const QString &name, double x_min, double x_max) {
        PecPlateParameters step;
        step.name = name;
        step.enabled = true;
        step.x_min_mm = x_min;
        step.x_max_mm = x_max;
        step.y_min_mm = -half_depth;
        step.y_max_mm = half_depth;
        step.z_min_mm = -0.5 * section_length;
        step.z_max_mm = 0.5 * section_length;
        parameters_.pec_plates.push_back(step);
    };

    const int base = parameters_.pec_plates.size() + 1;
    if (symmetric) {
        make_step(QStringLiteral("step_%1_left").arg(base), -half_width, -half_width + inset);
        make_step(QStringLiteral("step_%1_right").arg(base), half_width - inset, half_width);
    } else {
        make_step(QStringLiteral("step_%1").arg(base), half_width - inset, half_width);
    }
    selected_plate_index_ = parameters_.pec_plates.size() - 1;
    rebuildObjectTree();
    markModelChanged();
}

QWidget *MainWindow::createProjectionPanel()
{
    QWidget *panel = new QWidget(this);
    panel->setMinimumWidth(280);
    panel->setMaximumWidth(460);

    QVBoxLayout *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(5, 5, 5, 5);
    layout->setSpacing(5);

    QSplitter *projection_splitter = new QSplitter(Qt::Vertical, panel);

    QGroupBox *top_group = new QGroupBox(QStringLiteral("Top view E/H"), panel);
    QVBoxLayout *top_layout = new QVBoxLayout(top_group);
    top_layout->setContentsMargins(5, 12, 5, 5);
    top_layout->addWidget(top_projection_widget_);

    QGroupBox *side_group = new QGroupBox(QStringLiteral("Side view E/H"), panel);
    QVBoxLayout *side_layout = new QVBoxLayout(side_group);
    side_layout->setContentsMargins(5, 12, 5, 5);
    side_layout->addWidget(side_projection_widget_);

    projection_splitter->addWidget(top_group);
    projection_splitter->addWidget(side_group);
    projection_splitter->setStretchFactor(0, 1);
    projection_splitter->setStretchFactor(1, 1);
    projection_splitter->setSizes({260, 260});

    layout->addWidget(projection_splitter);
    return panel;
}


QWidget *MainWindow::createResultPanel()
{
    QWidget *panel = new QWidget(this);
    QVBoxLayout *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(12, 8, 12, 12);
    layout->setSpacing(6);

    status_label_ = new QLabel(QStringLiteral("Ожидание расчета..."), panel);
    status_label_->setMinimumHeight(24);
    calculation_time_label_ = new QLabel(panel);
    calculation_time_label_->setStyleSheet(
        QStringLiteral("color: #3d5968; font-weight: 600;"));
    calculation_time_label_->setVisible(false);
    calculation_progress_bar_ = new QProgressBar(panel);
    calculation_progress_bar_->setRange(0, 100);
    calculation_progress_bar_->setTextVisible(false);
    calculation_progress_bar_->setFixedHeight(8);
    calculation_progress_bar_->setVisible(false);

    layout->addWidget(status_label_);
    layout->addWidget(calculation_time_label_);
    layout->addWidget(calculation_progress_bar_);
    layout->addWidget(result_text_edit_);
    return panel;
}

QDoubleSpinBox *MainWindow::createSpinBox(double minimum,
                                          double maximum,
                                          double value,
                                          double step,
                                          int decimals,
                                          const QString &suffix,
                                          QWidget *parent)
{
    // Все числовые поля модели принимают выражения с переменными, как в CST.
    QDoubleSpinBox *spin_box = new ExpressionSpinBox(&parameter_store_, parent ? parent : this);
    spin_box->setRange(minimum, maximum);
    spin_box->setValue(value);
    spin_box->setSingleStep(step);
    spin_box->setDecimals(decimals);
    spin_box->setSuffix(suffix);
    spin_box->setKeyboardTracking(false);
    return spin_box;
}

WaveguideParameters MainWindow::readParameters() const
{
    return parameters_;
}

void MainWindow::applyInteractiveSlotParameters(const WaveguideParameters &parameters)
{
    const QSignalBlocker block_slot_enabled(slot_enabled_check_box_);
    parameters_ = parameters;
    if (slot_enabled_check_box_) {
        slot_enabled_check_box_->setChecked(parameters_.slot_enabled);
    }

    rebuildObjectTree();
    markModelChanged();
}

void MainWindow::rebuildObjectTree()
{
    if (!object_tree_widget_) {
        return;
    }

    const QSignalBlocker block_tree(object_tree_widget_);
    object_tree_widget_->clear();

    QTreeWidgetItem *components_item = new QTreeWidgetItem(object_tree_widget_);
    components_item->setText(0, QStringLiteral("Components"));
    components_item->setIcon(0, style()->standardIcon(QStyle::SP_DirIcon));

    QTreeWidgetItem *component_item = new QTreeWidgetItem(components_item);
    component_item->setText(0, QStringLiteral("component1"));
    component_item->setIcon(0, style()->standardIcon(QStyle::SP_DirOpenIcon));

    if (parameters_.slot_enabled) {
        QTreeWidgetItem *slot_item = new QTreeWidgetItem(component_item);
        slot_item->setText(0, slot_name_);
        slot_item->setIcon(0, style()->standardIcon(QStyle::SP_FileIcon));
        slot_item->setData(0, object_type_role, QStringLiteral("slot"));
    }

    for (int plate_index = 0; plate_index < parameters_.pec_plates.size(); ++plate_index) {
        const PecPlateParameters &plate = parameters_.pec_plates[plate_index];
        QTreeWidgetItem *plate_item = new QTreeWidgetItem(component_item);
        plate_item->setText(0, plate.name);
        plate_item->setIcon(0, style()->standardIcon(QStyle::SP_FileIcon));
        plate_item->setData(0, object_type_role, QStringLiteral("pec_plate"));
        plate_item->setData(0, object_index_role, plate_index);
        if (!plate.enabled) {
            plate_item->setForeground(0, QColor(135, 135, 135));
        }
        if (plate_index == selected_plate_index_) {
            plate_item->setSelected(true);
            object_tree_widget_->setCurrentItem(plate_item);
        }
    }

    QTreeWidgetItem *waveguide_item = new QTreeWidgetItem(component_item);
    waveguide_item->setText(0, waveguide_name_);
    waveguide_item->setIcon(0, style()->standardIcon(QStyle::SP_DriveHDIcon));
    waveguide_item->setData(0, object_type_role, QStringLiteral("waveguide"));

    QTreeWidgetItem *excitation_root_item = new QTreeWidgetItem(object_tree_widget_);
    excitation_root_item->setText(0, QStringLiteral("Excitation Signals"));
    excitation_root_item->setIcon(0, style()->standardIcon(QStyle::SP_DirIcon));

    QTreeWidgetItem *excitation_item = new QTreeWidgetItem(excitation_root_item);
    excitation_item->setText(0, excitation_name_);
    excitation_item->setIcon(0, style()->standardIcon(QStyle::SP_MediaPlay));
    excitation_item->setData(0, object_type_role, QStringLiteral("excitation"));

    object_tree_widget_->expandAll();
    open_gl_widget_->setSelectedPlateIndex(selected_plate_index_);
    top_projection_widget_->setSelectedPlateIndex(selected_plate_index_);
    side_projection_widget_->setSelectedPlateIndex(selected_plate_index_);
}

void MainWindow::handleObjectSelectionChanged()
{
    selected_plate_index_ = -1;
    const QList<QTreeWidgetItem *> selected_items = object_tree_widget_->selectedItems();
    if (!selected_items.isEmpty() &&
        selected_items.constFirst()->data(0, object_type_role).toString() == QStringLiteral("pec_plate")) {
        selected_plate_index_ = selected_items.constFirst()->data(0, object_index_role).toInt();
    }

    open_gl_widget_->setSelectedPlateIndex(selected_plate_index_);
    top_projection_widget_->setSelectedPlateIndex(selected_plate_index_);
    side_projection_widget_->setSelectedPlateIndex(selected_plate_index_);
}

void MainWindow::updateModelPreview()
{
    open_gl_widget_->setModelPreview(parameters_);
    top_projection_widget_->setModelPreview(parameters_);
    side_projection_widget_->setModelPreview(parameters_);
}

void MainWindow::handleObjectDoubleClick(QTreeWidgetItem *item, int)
{
    if (!item) {
        return;
    }

    const QString object_type = item->data(0, object_type_role).toString();
    if (object_type == QStringLiteral("waveguide")) {
        showWaveguideDialog();
    } else if (object_type == QStringLiteral("slot")) {
        showSlotDialog();
    } else if (object_type == QStringLiteral("pec_plate")) {
        showPlateDialog(item->data(0, object_index_role).toInt());
    } else if (object_type == QStringLiteral("excitation")) {
        showExcitationDialog();
    }
}

void MainWindow::showWaveguideDialog()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Brick"));
    dialog.setModal(true);
    dialog.setMinimumWidth(390);
    dialog.setStyleSheet(styleSheet());

    QHBoxLayout *dialog_layout = new QHBoxLayout(&dialog);
    QGridLayout *grid_layout = new QGridLayout();
    grid_layout->setHorizontalSpacing(8);
    grid_layout->setVerticalSpacing(7);

    QLineEdit *name_line_edit = new QLineEdit(waveguide_name_, &dialog);
    QDoubleSpinBox *x_min_spin_box = createSpinBox(-10000.0,
                                                   10000.0,
                                                   -0.5 * parameters_.width_mm,
                                                   0.1,
                                                   3,
                                                   QStringLiteral(" mm"),
                                                   &dialog);
    QDoubleSpinBox *x_max_spin_box = createSpinBox(-10000.0,
                                                   10000.0,
                                                   0.5 * parameters_.width_mm,
                                                   0.1,
                                                   3,
                                                   QStringLiteral(" mm"),
                                                   &dialog);
    QDoubleSpinBox *y_min_spin_box = createSpinBox(-10000.0,
                                                   10000.0,
                                                   -0.5 * parameters_.depth_mm,
                                                   0.1,
                                                   3,
                                                   QStringLiteral(" mm"),
                                                   &dialog);
    QDoubleSpinBox *y_max_spin_box = createSpinBox(-10000.0,
                                                   10000.0,
                                                   0.5 * parameters_.depth_mm,
                                                   0.1,
                                                   3,
                                                   QStringLiteral(" mm"),
                                                   &dialog);
    QDoubleSpinBox *z_min_spin_box = createSpinBox(-10000.0,
                                                   10000.0,
                                                   -0.5 * parameters_.length_mm,
                                                   0.1,
                                                   3,
                                                   QStringLiteral(" mm"),
                                                   &dialog);
    QDoubleSpinBox *z_max_spin_box = createSpinBox(-10000.0,
                                                   10000.0,
                                                   0.5 * parameters_.length_mm,
                                                   0.1,
                                                   3,
                                                   QStringLiteral(" mm"),
                                                   &dialog);
    QDoubleSpinBox *wall_thickness_spin_box = createSpinBox(0.01,
                                                            50.0,
                                                            parameters_.wall_thickness_mm,
                                                            0.01,
                                                            3,
                                                            QStringLiteral(" mm"),
                                                            &dialog);
    QDoubleSpinBox *frequency_spin_box = createSpinBox(0.01,
                                                       500.0,
                                                       parameters_.frequency_ghz,
                                                       0.1,
                                                       4,
                                                       QStringLiteral(" GHz"),
                                                       &dialog);
    QComboBox *component_combo_box = new QComboBox(&dialog);
    component_combo_box->addItem(QStringLiteral("component1"));
    component_combo_box->setEnabled(false);
    QComboBox *material_combo_box = new QComboBox(&dialog);
    material_combo_box->addItem(QStringLiteral("Идеальный проводник (PEC)"), 0.0);
    material_combo_box->addItem(QStringLiteral("Медь (5.8e7 См/м)"), 5.8e7);
    material_combo_box->addItem(QStringLiteral("Серебро (6.3e7 См/м)"), 6.3e7);
    material_combo_box->addItem(QStringLiteral("Алюминий (3.5e7 См/м)"), 3.5e7);
    material_combo_box->addItem(QStringLiteral("Латунь (1.5e7 См/м)"), 1.5e7);
    for (int index = 0; index < material_combo_box->count(); ++index) {
        if (std::abs(material_combo_box->itemData(index).toDouble() -
                     parameters_.wall_conductivity_s_per_m) < 1.0) {
            material_combo_box->setCurrentIndex(index);
            break;
        }
    }

    grid_layout->addWidget(new QLabel(QStringLiteral("Name:"), &dialog), 0, 0, 1, 2);
    grid_layout->addWidget(name_line_edit, 1, 0, 1, 2);
    grid_layout->addWidget(new QLabel(QStringLiteral("Xmin:"), &dialog), 2, 0);
    grid_layout->addWidget(new QLabel(QStringLiteral("Xmax:"), &dialog), 2, 1);
    grid_layout->addWidget(x_min_spin_box, 3, 0);
    grid_layout->addWidget(x_max_spin_box, 3, 1);
    grid_layout->addWidget(new QLabel(QStringLiteral("Ymin:"), &dialog), 4, 0);
    grid_layout->addWidget(new QLabel(QStringLiteral("Ymax:"), &dialog), 4, 1);
    grid_layout->addWidget(y_min_spin_box, 5, 0);
    grid_layout->addWidget(y_max_spin_box, 5, 1);
    grid_layout->addWidget(new QLabel(QStringLiteral("Zmin:"), &dialog), 6, 0);
    grid_layout->addWidget(new QLabel(QStringLiteral("Zmax:"), &dialog), 6, 1);
    grid_layout->addWidget(z_min_spin_box, 7, 0);
    grid_layout->addWidget(z_max_spin_box, 7, 1);
    grid_layout->addWidget(new QLabel(QStringLiteral("Wall thickness:"), &dialog), 8, 0);
    grid_layout->addWidget(new QLabel(QStringLiteral("Frequency:"), &dialog), 8, 1);
    grid_layout->addWidget(wall_thickness_spin_box, 9, 0);
    grid_layout->addWidget(frequency_spin_box, 9, 1);
    grid_layout->addWidget(new QLabel(QStringLiteral("Component:"), &dialog), 10, 0, 1, 2);
    grid_layout->addWidget(component_combo_box, 11, 0, 1, 2);
    grid_layout->addWidget(new QLabel(QStringLiteral("Материал стенок:"), &dialog), 12, 0, 1, 2);
    grid_layout->addWidget(material_combo_box, 13, 0, 1, 2);

    QVBoxLayout *button_layout = new QVBoxLayout();
    QPushButton *ok_button = new QPushButton(QStringLiteral("OK"), &dialog);
    QPushButton *cancel_button = new QPushButton(QStringLiteral("Cancel"), &dialog);
    QPushButton *preview_button = new QPushButton(QStringLiteral("Preview"), &dialog);
    QPushButton *help_button = new QPushButton(QStringLiteral("Help"), &dialog);
    ok_button->setDefault(true);
    button_layout->addWidget(ok_button);
    button_layout->addWidget(cancel_button);
    button_layout->addWidget(preview_button);
    button_layout->addWidget(help_button);
    button_layout->addStretch(1);

    dialog_layout->addLayout(grid_layout, 1);
    dialog_layout->addLayout(button_layout);

    const auto apply_values = [&]() {
        const double x_min = std::min(x_min_spin_box->value(), x_max_spin_box->value());
        const double x_max = std::max(x_min_spin_box->value(), x_max_spin_box->value());
        const double y_min = std::min(y_min_spin_box->value(), y_max_spin_box->value());
        const double y_max = std::max(y_min_spin_box->value(), y_max_spin_box->value());
        const double z_min = std::min(z_min_spin_box->value(), z_max_spin_box->value());
        const double z_max = std::max(z_min_spin_box->value(), z_max_spin_box->value());
        if (x_max <= x_min || y_max <= y_min || z_max <= z_min) {
            QMessageBox::warning(&dialog,
                                 QStringLiteral("Brick"),
                                 QStringLiteral("X/Y/Z max must be greater than min."));
            return false;
        }

        waveguide_name_ = name_line_edit->text().trimmed().isEmpty()
                              ? QStringLiteral("wr-90")
                              : name_line_edit->text().trimmed();
        parameters_.width_mm = x_max - x_min;
        parameters_.depth_mm = y_max - y_min;
        parameters_.length_mm = z_max - z_min;
        parameters_.wall_thickness_mm = wall_thickness_spin_box->value();
        parameters_.wall_conductivity_s_per_m =
            material_combo_box->currentData().toDouble();
        parameters_.frequency_ghz = frequency_spin_box->value();
        rebuildObjectTree();
        markModelChanged();
        return true;
    };

    connect(ok_button, &QPushButton::clicked, &dialog, [&]() {
        if (apply_values()) {
            dialog.accept();
        }
    });
    connect(cancel_button, &QPushButton::clicked, &dialog, &QDialog::reject);
    connect(preview_button, &QPushButton::clicked, &dialog, apply_values);
    connect(help_button, &QPushButton::clicked, &dialog, [&]() {
        QMessageBox::information(&dialog,
                                 QStringLiteral("Brick"),
                                 QStringLiteral("Set object bounds with Xmin/Xmax, Ymin/Ymax, Zmin/Zmax."));
    });

    dialog.exec();
}

void MainWindow::showSlotDialog()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Brick"));
    dialog.setModal(true);
    dialog.setMinimumWidth(410);
    dialog.setStyleSheet(styleSheet());

    const double half_slot_width = 0.5 * parameters_.slot_width_mm;
    const double half_slot_length = 0.5 * parameters_.slot_length_mm;
    const double half_wall = std::max(0.02, 0.5 * parameters_.wall_thickness_mm);
    double x_min = -half_slot_width;
    double x_max = half_slot_width;
    double y_min = 0.5 * parameters_.depth_mm - half_wall;
    double y_max = 0.5 * parameters_.depth_mm + half_wall;
    double z_min = parameters_.slot_offset_z_mm - half_slot_length;
    double z_max = parameters_.slot_offset_z_mm + half_slot_length;

    if (parameters_.slot_surface == 1) {
        x_min = 0.5 * parameters_.width_mm - half_wall;
        x_max = 0.5 * parameters_.width_mm + half_wall;
        y_min = parameters_.slot_offset_x_mm - half_slot_width;
        y_max = parameters_.slot_offset_x_mm + half_slot_width;
    } else if (parameters_.slot_surface == 2) {
        x_min = parameters_.slot_offset_x_mm - half_slot_width;
        x_max = parameters_.slot_offset_x_mm + half_slot_width;
        y_min = -0.5 * parameters_.depth_mm - half_wall;
        y_max = -0.5 * parameters_.depth_mm + half_wall;
    } else if (parameters_.slot_surface == 3) {
        x_min = -0.5 * parameters_.width_mm - half_wall;
        x_max = -0.5 * parameters_.width_mm + half_wall;
        y_min = parameters_.slot_offset_x_mm - half_slot_width;
        y_max = parameters_.slot_offset_x_mm + half_slot_width;
    } else {
        x_min = parameters_.slot_offset_x_mm - half_slot_width;
        x_max = parameters_.slot_offset_x_mm + half_slot_width;
    }

    QHBoxLayout *dialog_layout = new QHBoxLayout(&dialog);
    QGridLayout *grid_layout = new QGridLayout();
    grid_layout->setHorizontalSpacing(8);
    grid_layout->setVerticalSpacing(7);

    QLineEdit *name_line_edit = new QLineEdit(slot_name_, &dialog);
    QDoubleSpinBox *x_min_spin_box = createSpinBox(-10000.0, 10000.0, x_min, 0.1, 3, QStringLiteral(" mm"), &dialog);
    QDoubleSpinBox *x_max_spin_box = createSpinBox(-10000.0, 10000.0, x_max, 0.1, 3, QStringLiteral(" mm"), &dialog);
    QDoubleSpinBox *y_min_spin_box = createSpinBox(-10000.0, 10000.0, y_min, 0.1, 3, QStringLiteral(" mm"), &dialog);
    QDoubleSpinBox *y_max_spin_box = createSpinBox(-10000.0, 10000.0, y_max, 0.1, 3, QStringLiteral(" mm"), &dialog);
    QDoubleSpinBox *z_min_spin_box = createSpinBox(-10000.0, 10000.0, z_min, 0.1, 3, QStringLiteral(" mm"), &dialog);
    QDoubleSpinBox *z_max_spin_box = createSpinBox(-10000.0, 10000.0, z_max, 0.1, 3, QStringLiteral(" mm"), &dialog);
    QDoubleSpinBox *rotation_spin_box = createSpinBox(-180.0,
                                                      180.0,
                                                      parameters_.slot_rotation_deg,
                                                      1.0,
                                                      2,
                                                      QStringLiteral(" deg"),
                                                      &dialog);
    QComboBox *surface_combo_box = new QComboBox(&dialog);
    surface_combo_box->addItem(QStringLiteral("Верх"), 0);
    surface_combo_box->addItem(QStringLiteral("Правая стенка"), 1);
    surface_combo_box->addItem(QStringLiteral("Низ"), 2);
    surface_combo_box->addItem(QStringLiteral("Левая стенка"), 3);
    const int surface_index = surface_combo_box->findData(parameters_.slot_surface);
    if (surface_index >= 0) {
        surface_combo_box->setCurrentIndex(surface_index);
    }
    QComboBox *component_combo_box = new QComboBox(&dialog);
    component_combo_box->addItem(QStringLiteral("component1"));
    component_combo_box->setEnabled(false);
    QComboBox *material_combo_box = new QComboBox(&dialog);
    material_combo_box->addItem(QStringLiteral("PEC"));
    material_combo_box->setEnabled(false);

    grid_layout->addWidget(new QLabel(QStringLiteral("Name:"), &dialog), 0, 0, 1, 2);
    grid_layout->addWidget(name_line_edit, 1, 0, 1, 2);
    grid_layout->addWidget(new QLabel(QStringLiteral("Xmin:"), &dialog), 2, 0);
    grid_layout->addWidget(new QLabel(QStringLiteral("Xmax:"), &dialog), 2, 1);
    grid_layout->addWidget(x_min_spin_box, 3, 0);
    grid_layout->addWidget(x_max_spin_box, 3, 1);
    grid_layout->addWidget(new QLabel(QStringLiteral("Ymin:"), &dialog), 4, 0);
    grid_layout->addWidget(new QLabel(QStringLiteral("Ymax:"), &dialog), 4, 1);
    grid_layout->addWidget(y_min_spin_box, 5, 0);
    grid_layout->addWidget(y_max_spin_box, 5, 1);
    grid_layout->addWidget(new QLabel(QStringLiteral("Zmin:"), &dialog), 6, 0);
    grid_layout->addWidget(new QLabel(QStringLiteral("Zmax:"), &dialog), 6, 1);
    grid_layout->addWidget(z_min_spin_box, 7, 0);
    grid_layout->addWidget(z_max_spin_box, 7, 1);
    grid_layout->addWidget(new QLabel(QStringLiteral("Surface:"), &dialog), 8, 0);
    grid_layout->addWidget(new QLabel(QStringLiteral("Rotation:"), &dialog), 8, 1);
    grid_layout->addWidget(surface_combo_box, 9, 0);
    grid_layout->addWidget(rotation_spin_box, 9, 1);
    grid_layout->addWidget(new QLabel(QStringLiteral("Component:"), &dialog), 10, 0, 1, 2);
    grid_layout->addWidget(component_combo_box, 11, 0, 1, 2);
    grid_layout->addWidget(new QLabel(QStringLiteral("Material:"), &dialog), 12, 0, 1, 2);
    grid_layout->addWidget(material_combo_box, 13, 0, 1, 2);

    QVBoxLayout *button_layout = new QVBoxLayout();
    QPushButton *ok_button = new QPushButton(QStringLiteral("OK"), &dialog);
    QPushButton *cancel_button = new QPushButton(QStringLiteral("Cancel"), &dialog);
    QPushButton *preview_button = new QPushButton(QStringLiteral("Preview"), &dialog);
    QPushButton *help_button = new QPushButton(QStringLiteral("Help"), &dialog);
    ok_button->setDefault(true);
    button_layout->addWidget(ok_button);
    button_layout->addWidget(cancel_button);
    button_layout->addWidget(preview_button);
    button_layout->addWidget(help_button);
    button_layout->addStretch(1);

    dialog_layout->addLayout(grid_layout, 1);
    dialog_layout->addLayout(button_layout);

    const auto apply_values = [&]() {
        const double local_x_min = std::min(x_min_spin_box->value(), x_max_spin_box->value());
        const double local_x_max = std::max(x_min_spin_box->value(), x_max_spin_box->value());
        const double local_y_min = std::min(y_min_spin_box->value(), y_max_spin_box->value());
        const double local_y_max = std::max(y_min_spin_box->value(), y_max_spin_box->value());
        const double local_z_min = std::min(z_min_spin_box->value(), z_max_spin_box->value());
        const double local_z_max = std::max(z_min_spin_box->value(), z_max_spin_box->value());
        if (local_z_max <= local_z_min) {
            QMessageBox::warning(&dialog,
                                 QStringLiteral("Brick"),
                                 QStringLiteral("Zmax must be greater than Zmin."));
            return false;
        }

        const int surface = surface_combo_box->currentData().toInt();
        double slot_width = local_x_max - local_x_min;
        double slot_offset = 0.5 * (local_x_min + local_x_max);
        if (surface == 1 || surface == 3) {
            slot_width = local_y_max - local_y_min;
            slot_offset = 0.5 * (local_y_min + local_y_max);
        }

        if (slot_width <= 0.0) {
            QMessageBox::warning(&dialog,
                                 QStringLiteral("Brick"),
                                 QStringLiteral("Slot width must be greater than zero."));
            return false;
        }

        slot_name_ = name_line_edit->text().trimmed().isEmpty()
                         ? QStringLiteral("figure_1")
                         : name_line_edit->text().trimmed();
        parameters_.slot_enabled = true;
        parameters_.slot_surface = surface;
        parameters_.slot_width_mm = slot_width;
        parameters_.slot_length_mm = local_z_max - local_z_min;
        parameters_.slot_offset_x_mm = slot_offset;
        parameters_.slot_offset_z_mm = 0.5 * (local_z_min + local_z_max);
        parameters_.slot_rotation_deg = rotation_spin_box->value();

        const QSignalBlocker block_slot_enabled(slot_enabled_check_box_);
        if (slot_enabled_check_box_) {
            slot_enabled_check_box_->setChecked(true);
        }
        rebuildObjectTree();
        markModelChanged();
        return true;
    };

    connect(ok_button, &QPushButton::clicked, &dialog, [&]() {
        if (apply_values()) {
            dialog.accept();
        }
    });
    connect(cancel_button, &QPushButton::clicked, &dialog, &QDialog::reject);
    connect(preview_button, &QPushButton::clicked, &dialog, apply_values);
    connect(help_button, &QPushButton::clicked, &dialog, [&]() {
        QMessageBox::information(&dialog,
                                 QStringLiteral("Brick"),
                                 QStringLiteral("Set slot bounds. For side walls, Ymin/Ymax define slot width."));
    });

    dialog.exec();
}

void MainWindow::showPlateDialog(int plate_index, bool iris_template, bool round_post_template)
{
    const bool creating = plate_index < 0 || plate_index >= parameters_.pec_plates.size();
    PecPlateParameters plate;
    if (!creating) {
        plate = parameters_.pec_plates[plate_index];
    } else if (iris_template && round_post_template) {
        // Circular diaphragm with a coaxial post: the plate spans the whole
        // cross-section, the hole is centred, and the post stands on its axis.
        const double inner_width = parameters_.width_mm - 2.0 * parameters_.wall_thickness_mm;
        const double inner_depth = parameters_.depth_mm - 2.0 * parameters_.wall_thickness_mm;
        const double thickness = std::max(0.5, 2.0 * parameters_.wall_thickness_mm);
        const double radius = 0.30 * std::min(inner_width, inner_depth) * 2.0 * 0.5;
        plate.name = QStringLiteral("iris_post_%1").arg(parameters_.pec_plates.size() + 1);
        plate.x_min_mm = -0.5 * inner_width;
        plate.x_max_mm = 0.5 * inner_width;
        plate.y_min_mm = -0.5 * inner_depth;
        plate.y_max_mm = 0.5 * inner_depth;
        plate.z_min_mm = -0.5 * thickness;
        plate.z_max_mm = 0.5 * thickness;
        plate.aperture_enabled = true;
        plate.aperture_shape = 1;
        plate.aperture_radius_mm = radius;
        plate.aperture_offset_x_mm = 0.0;
        plate.aperture_offset_y_mm = 0.0;
        plate.post_enabled = true;
        plate.post_width_mm = 0.5 * radius;
        // From the plate bottom edge up to the centre of the hole.
        plate.post_height_mm = 0.5 * inner_depth;
    } else if (iris_template) {
        // Diaphragm template: the plate spans the whole inner cross-section
        // (its edges touch all four walls) and carries a centred window.
        const double inner_width = parameters_.width_mm - 2.0 * parameters_.wall_thickness_mm;
        const double inner_depth = parameters_.depth_mm - 2.0 * parameters_.wall_thickness_mm;
        const double thickness = std::max(0.5, 2.0 * parameters_.wall_thickness_mm);
        plate.name = QStringLiteral("iris_%1").arg(parameters_.pec_plates.size() + 1);
        plate.x_min_mm = -0.5 * inner_width;
        plate.x_max_mm = 0.5 * inner_width;
        plate.y_min_mm = -0.5 * inner_depth;
        plate.y_max_mm = 0.5 * inner_depth;
        plate.z_min_mm = -0.5 * thickness;
        plate.z_max_mm = 0.5 * thickness;
        plate.aperture_enabled = true;
        plate.aperture_width_mm = 0.5 * inner_width;
        plate.aperture_height_mm = 0.5 * inner_depth;
        plate.aperture_offset_x_mm = 0.0;
        plate.aperture_offset_y_mm = 0.0;
    } else {
        plate.name = QStringLiteral("plate_%1").arg(parameters_.pec_plates.size() + 1);
        const double inner_depth = parameters_.depth_mm - 2.0 * parameters_.wall_thickness_mm;
        const double pin_thickness = std::max(0.5, 2.0 * parameters_.wall_thickness_mm);
        plate.x_min_mm = -0.5 * pin_thickness;
        plate.x_max_mm = 0.5 * pin_thickness;
        plate.y_min_mm = -0.5 * inner_depth;
        plate.y_max_mm = 0.5 * inner_depth;
        plate.z_min_mm = -0.5 * pin_thickness;
        plate.z_max_mm = 0.5 * pin_thickness;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("PEC Plate"));
    dialog.setModal(true);
    dialog.setMinimumWidth(430);
    dialog.setStyleSheet(styleSheet());

    QHBoxLayout *dialog_layout = new QHBoxLayout(&dialog);
    QGridLayout *grid_layout = new QGridLayout();
    grid_layout->setHorizontalSpacing(8);
    grid_layout->setVerticalSpacing(7);

    QLineEdit *name_line_edit = new QLineEdit(plate.name, &dialog);
    QDoubleSpinBox *x_min_spin_box = createSpinBox(-10000.0, 10000.0, plate.x_min_mm, 0.1, 3,
                                                   QStringLiteral(" mm"), &dialog);
    QDoubleSpinBox *x_max_spin_box = createSpinBox(-10000.0, 10000.0, plate.x_max_mm, 0.1, 3,
                                                   QStringLiteral(" mm"), &dialog);
    QDoubleSpinBox *y_min_spin_box = createSpinBox(-10000.0, 10000.0, plate.y_min_mm, 0.1, 3,
                                                   QStringLiteral(" mm"), &dialog);
    QDoubleSpinBox *y_max_spin_box = createSpinBox(-10000.0, 10000.0, plate.y_max_mm, 0.1, 3,
                                                   QStringLiteral(" mm"), &dialog);
    QDoubleSpinBox *z_min_spin_box = createSpinBox(-10000.0, 10000.0, plate.z_min_mm, 0.1, 3,
                                                   QStringLiteral(" mm"), &dialog);
    QDoubleSpinBox *z_max_spin_box = createSpinBox(-10000.0, 10000.0, plate.z_max_mm, 0.1, 3,
                                                   QStringLiteral(" mm"), &dialog);
    QDoubleSpinBox *rotation_x_spin_box = createSpinBox(-180.0, 180.0, plate.rotation_x_deg, 1.0, 2,
                                                        QStringLiteral(" deg"), &dialog);
    QDoubleSpinBox *rotation_y_spin_box = createSpinBox(-180.0, 180.0, plate.rotation_y_deg, 1.0, 2,
                                                        QStringLiteral(" deg"), &dialog);
    QDoubleSpinBox *rotation_z_spin_box = createSpinBox(-180.0, 180.0, plate.rotation_z_deg, 1.0, 2,
                                                        QStringLiteral(" deg"), &dialog);
    QCheckBox *enabled_check_box = new QCheckBox(QStringLiteral("Включена в модель"), &dialog);
    enabled_check_box->setChecked(plate.enabled);
    QCheckBox *aperture_check_box =
        new QCheckBox(QStringLiteral("Окно в пластине (диафрагма)"), &dialog);
    aperture_check_box->setChecked(plate.aperture_enabled);
    QComboBox *aperture_shape_combo_box = new QComboBox(&dialog);
    aperture_shape_combo_box->addItem(QStringLiteral("Прямоугольное окно"));
    aperture_shape_combo_box->addItem(QStringLiteral("Круглое отверстие"));
    aperture_shape_combo_box->setCurrentIndex(std::clamp(plate.aperture_shape, 0, 1));
    QDoubleSpinBox *aperture_radius_spin_box = createSpinBox(0.01, 10000.0, plate.aperture_radius_mm,
                                                             0.1, 3, QStringLiteral(" mm"), &dialog);
    QCheckBox *post_check_box =
        new QCheckBox(QStringLiteral("Язычок снизу в окно (в плоскости пластины)"), &dialog);
    post_check_box->setChecked(plate.post_enabled);
    QDoubleSpinBox *post_width_spin_box = createSpinBox(0.01, 10000.0, plate.post_width_mm,
                                                        0.1, 3, QStringLiteral(" mm"), &dialog);
    QDoubleSpinBox *post_height_spin_box = createSpinBox(0.01, 10000.0, plate.post_height_mm,
                                                         0.1, 3, QStringLiteral(" mm"), &dialog);
    QDoubleSpinBox *aperture_width_spin_box = createSpinBox(0.01, 10000.0, plate.aperture_width_mm,
                                                            0.1, 3, QStringLiteral(" mm"), &dialog);
    QDoubleSpinBox *aperture_height_spin_box = createSpinBox(0.01, 10000.0, plate.aperture_height_mm,
                                                             0.1, 3, QStringLiteral(" mm"), &dialog);
    QDoubleSpinBox *aperture_offset_x_spin_box = createSpinBox(-10000.0, 10000.0,
                                                               plate.aperture_offset_x_mm,
                                                               0.1, 3, QStringLiteral(" mm"), &dialog);
    QDoubleSpinBox *aperture_offset_y_spin_box = createSpinBox(-10000.0, 10000.0,
                                                               plate.aperture_offset_y_mm,
                                                               0.1, 3, QStringLiteral(" mm"), &dialog);
    const auto update_aperture_enabled = [&]() {
        const bool on = aperture_check_box->isChecked();
        const bool circular = aperture_shape_combo_box->currentIndex() == 1;
        aperture_shape_combo_box->setEnabled(on);
        aperture_width_spin_box->setEnabled(on && !circular);
        aperture_height_spin_box->setEnabled(on && !circular);
        aperture_radius_spin_box->setEnabled(on && circular);
        aperture_offset_x_spin_box->setEnabled(on);
        aperture_offset_y_spin_box->setEnabled(on);
        post_check_box->setEnabled(on);
        const bool post_on = on && post_check_box->isChecked();
        post_width_spin_box->setEnabled(post_on);
        post_height_spin_box->setEnabled(post_on);
    };
    update_aperture_enabled();
    connect(aperture_check_box, &QCheckBox::toggled, &dialog, update_aperture_enabled);
    connect(post_check_box, &QCheckBox::toggled, &dialog, update_aperture_enabled);
    connect(aperture_shape_combo_box,
            qOverload<int>(&QComboBox::currentIndexChanged),
            &dialog,
            [update_aperture_enabled](int) { update_aperture_enabled(); });
    QComboBox *component_combo_box = new QComboBox(&dialog);
    component_combo_box->addItem(QStringLiteral("component1"));
    component_combo_box->setEnabled(false);
    QComboBox *material_combo_box = new QComboBox(&dialog);
    material_combo_box->addItem(QStringLiteral("PEC"));
    material_combo_box->setEnabled(false);

    grid_layout->addWidget(new QLabel(QStringLiteral("Name:"), &dialog), 0, 0, 1, 2);
    grid_layout->addWidget(name_line_edit, 1, 0, 1, 2);
    grid_layout->addWidget(new QLabel(QStringLiteral("Xmin:"), &dialog), 2, 0);
    grid_layout->addWidget(new QLabel(QStringLiteral("Xmax:"), &dialog), 2, 1);
    grid_layout->addWidget(x_min_spin_box, 3, 0);
    grid_layout->addWidget(x_max_spin_box, 3, 1);
    grid_layout->addWidget(new QLabel(QStringLiteral("Ymin:"), &dialog), 4, 0);
    grid_layout->addWidget(new QLabel(QStringLiteral("Ymax:"), &dialog), 4, 1);
    grid_layout->addWidget(y_min_spin_box, 5, 0);
    grid_layout->addWidget(y_max_spin_box, 5, 1);
    grid_layout->addWidget(new QLabel(QStringLiteral("Zmin:"), &dialog), 6, 0);
    grid_layout->addWidget(new QLabel(QStringLiteral("Zmax:"), &dialog), 6, 1);
    grid_layout->addWidget(z_min_spin_box, 7, 0);
    grid_layout->addWidget(z_max_spin_box, 7, 1);
    grid_layout->addWidget(new QLabel(QStringLiteral("Rotation X:"), &dialog), 8, 0);
    grid_layout->addWidget(new QLabel(QStringLiteral("Rotation Y:"), &dialog), 8, 1);
    grid_layout->addWidget(rotation_x_spin_box, 9, 0);
    grid_layout->addWidget(rotation_y_spin_box, 9, 1);
    grid_layout->addWidget(new QLabel(QStringLiteral("Rotation Z:"), &dialog), 10, 0);
    grid_layout->addWidget(rotation_z_spin_box, 11, 0);
    grid_layout->addWidget(enabled_check_box, 11, 1);
    grid_layout->addWidget(aperture_check_box, 12, 0, 1, 2);
    grid_layout->addWidget(aperture_shape_combo_box, 13, 0, 1, 2);
    grid_layout->addWidget(new QLabel(QStringLiteral("Окно: ширина X:"), &dialog), 14, 0);
    grid_layout->addWidget(new QLabel(QStringLiteral("Окно: высота Y:"), &dialog), 14, 1);
    grid_layout->addWidget(aperture_width_spin_box, 15, 0);
    grid_layout->addWidget(aperture_height_spin_box, 15, 1);
    grid_layout->addWidget(new QLabel(QStringLiteral("Радиус отверстия:"), &dialog), 16, 0);
    grid_layout->addWidget(aperture_radius_spin_box, 17, 0);
    grid_layout->addWidget(new QLabel(QStringLiteral("Окно: смещение X:"), &dialog), 18, 0);
    grid_layout->addWidget(new QLabel(QStringLiteral("Окно: смещение Y:"), &dialog), 18, 1);
    grid_layout->addWidget(aperture_offset_x_spin_box, 19, 0);
    grid_layout->addWidget(aperture_offset_y_spin_box, 19, 1);
    grid_layout->addWidget(post_check_box, 20, 0, 1, 2);
    grid_layout->addWidget(new QLabel(QStringLiteral("Язычок: ширина X:"), &dialog), 21, 0);
    grid_layout->addWidget(new QLabel(QStringLiteral("Язычок: высота снизу:"), &dialog), 21, 1);
    grid_layout->addWidget(post_width_spin_box, 22, 0);
    grid_layout->addWidget(post_height_spin_box, 22, 1);
    grid_layout->addWidget(new QLabel(QStringLiteral("Component:"), &dialog), 23, 0);
    grid_layout->addWidget(new QLabel(QStringLiteral("Material:"), &dialog), 23, 1);
    grid_layout->addWidget(component_combo_box, 24, 0);
    grid_layout->addWidget(material_combo_box, 24, 1);

    QVBoxLayout *button_layout = new QVBoxLayout();
    QPushButton *ok_button = new QPushButton(QStringLiteral("OK"), &dialog);
    QPushButton *cancel_button = new QPushButton(QStringLiteral("Cancel"), &dialog);
    QPushButton *delete_button = new QPushButton(QStringLiteral("Удалить"), &dialog);
    ok_button->setDefault(true);
    delete_button->setEnabled(!creating);
    button_layout->addWidget(ok_button);
    button_layout->addWidget(cancel_button);
    button_layout->addWidget(delete_button);
    button_layout->addStretch(1);
    dialog_layout->addLayout(grid_layout, 1);
    dialog_layout->addLayout(button_layout);

    connect(ok_button, &QPushButton::clicked, &dialog, [&]() {
        const double x_min = std::min(x_min_spin_box->value(), x_max_spin_box->value());
        const double x_max = std::max(x_min_spin_box->value(), x_max_spin_box->value());
        const double y_min = std::min(y_min_spin_box->value(), y_max_spin_box->value());
        const double y_max = std::max(y_min_spin_box->value(), y_max_spin_box->value());
        const double z_min = std::min(z_min_spin_box->value(), z_max_spin_box->value());
        const double z_max = std::max(z_min_spin_box->value(), z_max_spin_box->value());
        if (x_max <= x_min || y_max <= y_min || z_max <= z_min) {
            QMessageBox::warning(&dialog,
                                 QStringLiteral("PEC Plate"),
                                 QStringLiteral("X/Y/Z max must be greater than min."));
            return;
        }

        plate.name = name_line_edit->text().trimmed().isEmpty()
                         ? QStringLiteral("plate_%1").arg(creating
                                                               ? parameters_.pec_plates.size() + 1
                                                               : plate_index + 1)
                         : name_line_edit->text().trimmed();
        plate.enabled = enabled_check_box->isChecked();
        plate.x_min_mm = x_min;
        plate.x_max_mm = x_max;
        plate.y_min_mm = y_min;
        plate.y_max_mm = y_max;
        plate.z_min_mm = z_min;
        plate.z_max_mm = z_max;
        plate.rotation_x_deg = rotation_x_spin_box->value();
        plate.rotation_y_deg = rotation_y_spin_box->value();
        plate.rotation_z_deg = rotation_z_spin_box->value();
        plate.aperture_enabled = aperture_check_box->isChecked();
        plate.aperture_shape = aperture_shape_combo_box->currentIndex();
        plate.aperture_width_mm = aperture_width_spin_box->value();
        plate.aperture_height_mm = aperture_height_spin_box->value();
        plate.aperture_radius_mm = aperture_radius_spin_box->value();
        plate.aperture_offset_x_mm = aperture_offset_x_spin_box->value();
        plate.aperture_offset_y_mm = aperture_offset_y_spin_box->value();
        plate.post_enabled = post_check_box->isChecked();
        plate.post_width_mm = post_width_spin_box->value();
        plate.post_height_mm = post_height_spin_box->value();
        if (plate.aperture_enabled) {
            const bool circular = plate.aperture_shape == 1;
            const double span_x =
                circular ? plate.aperture_radius_mm : 0.5 * plate.aperture_width_mm;
            const double span_y =
                circular ? plate.aperture_radius_mm : 0.5 * plate.aperture_height_mm;
            const double slack = circular ? 0.0 : 1.0e-6;
            const bool exceeds_x = circular
                                       ? std::abs(plate.aperture_offset_x_mm) + span_x >=
                                             0.5 * (x_max - x_min)
                                       : std::abs(plate.aperture_offset_x_mm) + span_x >
                                             0.5 * (x_max - x_min) + slack;
            const bool exceeds_y = circular
                                       ? std::abs(plate.aperture_offset_y_mm) + span_y >=
                                             0.5 * (y_max - y_min)
                                       : std::abs(plate.aperture_offset_y_mm) + span_y >
                                             0.5 * (y_max - y_min) + slack;
            if (exceeds_x || exceeds_y) {
                QMessageBox::warning(&dialog,
                                     QStringLiteral("PEC Plate"),
                                     QStringLiteral("Окно должно помещаться внутри пластины "
                                                    "(круглое — не касаясь краёв)."));
                return;
            }
            if (plate.post_enabled &&
                (plate.post_width_mm > x_max - x_min ||
                 plate.post_height_mm > y_max - y_min)) {
                QMessageBox::warning(&dialog,
                                     QStringLiteral("PEC Plate"),
                                     QStringLiteral("Язычок не помещается в пластину."));
                return;
            }
        }
        if (creating) {
            parameters_.pec_plates.push_back(plate);
            selected_plate_index_ = parameters_.pec_plates.size() - 1;
        } else {
            parameters_.pec_plates[plate_index] = plate;
            selected_plate_index_ = plate_index;
        }
        rebuildObjectTree();
        markModelChanged();
        dialog.accept();
    });
    connect(cancel_button, &QPushButton::clicked, &dialog, &QDialog::reject);
    connect(delete_button, &QPushButton::clicked, &dialog, [&]() {
        if (!creating && plate_index >= 0 && plate_index < parameters_.pec_plates.size()) {
            parameters_.pec_plates.removeAt(plate_index);
            selected_plate_index_ = -1;
            rebuildObjectTree();
            markModelChanged();
        }
        dialog.accept();
    });

    dialog.exec();
}

void MainWindow::showExcitationDialog()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Excitation Signal"));
    dialog.setModal(true);
    dialog.setMinimumWidth(360);
    dialog.setStyleSheet(styleSheet());

    QHBoxLayout *dialog_layout = new QHBoxLayout(&dialog);
    QGridLayout *grid_layout = new QGridLayout();
    grid_layout->setHorizontalSpacing(8);
    grid_layout->setVerticalSpacing(7);

    QLineEdit *name_line_edit = new QLineEdit(excitation_name_, &dialog);
    QDoubleSpinBox *frequency_spin_box = createSpinBox(0.01,
                                                       500.0,
                                                       parameters_.frequency_ghz,
                                                       0.1,
                                                       4,
                                                       QStringLiteral(" GHz"),
                                                       &dialog);
    QComboBox *mode_policy_combo_box = new QComboBox(&dialog);
    mode_policy_combo_box->addItem(QStringLiteral("Auto: first propagating mode"));
    mode_policy_combo_box->setEnabled(false);

    grid_layout->addWidget(new QLabel(QStringLiteral("Name:"), &dialog), 0, 0, 1, 2);
    grid_layout->addWidget(name_line_edit, 1, 0, 1, 2);
    grid_layout->addWidget(new QLabel(QStringLiteral("Frequency:"), &dialog), 2, 0, 1, 2);
    grid_layout->addWidget(frequency_spin_box, 3, 0, 1, 2);
    grid_layout->addWidget(new QLabel(QStringLiteral("Mode selection:"), &dialog), 4, 0, 1, 2);
    grid_layout->addWidget(mode_policy_combo_box, 5, 0, 1, 2);

    QVBoxLayout *button_layout = new QVBoxLayout();
    QPushButton *ok_button = new QPushButton(QStringLiteral("OK"), &dialog);
    QPushButton *cancel_button = new QPushButton(QStringLiteral("Cancel"), &dialog);
    QPushButton *preview_button = new QPushButton(QStringLiteral("Preview"), &dialog);
    QPushButton *help_button = new QPushButton(QStringLiteral("Help"), &dialog);
    ok_button->setDefault(true);
    button_layout->addWidget(ok_button);
    button_layout->addWidget(cancel_button);
    button_layout->addWidget(preview_button);
    button_layout->addWidget(help_button);
    button_layout->addStretch(1);

    dialog_layout->addLayout(grid_layout, 1);
    dialog_layout->addLayout(button_layout);

    const auto apply_values = [&]() {
        if (frequency_spin_box->value() <= 0.0) {
            QMessageBox::warning(&dialog,
                                 QStringLiteral("Excitation Signal"),
                                 QStringLiteral("Frequency must be greater than zero."));
            return false;
        }

        excitation_name_ = name_line_edit->text().trimmed().isEmpty()
                               ? QStringLiteral("signal1")
                               : name_line_edit->text().trimmed();
        parameters_.frequency_ghz = frequency_spin_box->value();
        rebuildObjectTree();
        markModelChanged();
        return true;
    };

    connect(ok_button, &QPushButton::clicked, &dialog, [&]() {
        if (apply_values()) {
            dialog.accept();
        }
    });
    connect(cancel_button, &QPushButton::clicked, &dialog, &QDialog::reject);
    connect(preview_button, &QPushButton::clicked, &dialog, apply_values);
    connect(help_button, &QPushButton::clicked, &dialog, [&]() {
        QMessageBox::information(&dialog,
                                 QStringLiteral("Excitation Signal"),
                                 QStringLiteral("Frequency controls cutoff check, guide wavelength, beta and selected propagating mode."));
    });

    dialog.exec();
}

void MainWindow::applyCstStyle()
{
    setStyleSheet(cstStyleSheet());
}

QString MainWindow::solverMethodName(int method) const
{
    static const char *const names[] = {
        "Автоматически",
        "Аналитический (пустой волновод)",
        "Метод поперечных сечений",
        "Метод частичных областей",
        "Метод конечных элементов (FEM)",
    };
    return QString::fromUtf8(names[std::clamp(method, 0, 4)]);
}

void MainWindow::createRibbon()
{
    ribbon_bar_ = new RibbonBar(this);

    // Действие принадлежит окну и добавляется в его список: иначе горячая
    // клавиша не сработает, пока вкладка ленты с этой кнопкой скрыта.
    const auto make_action = [this](const QString &text, const QString &tip) {
        QAction *action = new QAction(text, this);
        action->setToolTip(tip);
        addAction(action);
        return action;
    };

    // ------------------------------------------------------------- File ----
    RibbonTab *file_tab = ribbon_bar_->addRibbonTab(QStringLiteral("File"));
    RibbonGroup *project_group = file_tab->addGroup(QStringLiteral("Проект"));

    QAction *open_action =
        make_action(QStringLiteral("Открыть"), QStringLiteral("Открыть модель волновода (.wgm)"));
    open_action->setShortcut(QKeySequence::Open);
    connect(open_action, &QAction::triggered, this, &MainWindow::openModelFile);
    project_group->addLargeButton(open_action, RibbonIcon::Open);

    QAction *save_action =
        make_action(QStringLiteral("Сохранить"), QStringLiteral("Сохранить модель и расчёт"));
    save_action->setShortcut(QKeySequence::Save);
    connect(save_action, &QAction::triggered, this, [this]() { saveModelFile(); });
    project_group->addLargeButton(save_action, RibbonIcon::Save);

    QAction *save_as_action = make_action(QStringLiteral("Сохранить\nкак"),
                                          QStringLiteral("Сохранить модель в другой файл"));
    save_as_action->setShortcut(QKeySequence::SaveAs);
    connect(save_as_action, &QAction::triggered, this, [this]() { saveModelFileAs(); });
    project_group->addLargeButton(save_as_action, RibbonIcon::SaveAs);

    RibbonGroup *exit_group = file_tab->addGroup(QStringLiteral("Выход"));
    QAction *quit_action =
        make_action(QStringLiteral("Закрыть"), QStringLiteral("Закрыть приложение"));
    quit_action->setShortcut(QKeySequence::Quit);
    connect(quit_action, &QAction::triggered, this, &QWidget::close);
    exit_group->addLargeButton(quit_action, RibbonIcon::Quit);

    // ------------------------------------------------------------- Home ----
    RibbonTab *home_tab = ribbon_bar_->addRibbonTab(QStringLiteral("Home"));
    RibbonGroup *home_file_group = home_tab->addGroup(QStringLiteral("Файл"));
    home_file_group->addSmallButton(open_action, RibbonIcon::Open);
    home_file_group->addSmallButton(save_action, RibbonIcon::Save);

    RibbonGroup *home_simulation_group = home_tab->addGroup(QStringLiteral("Simulation"));

    start_simulation_action_ = make_action(
        QStringLiteral("Начать\nрасчёт"),
        QStringLiteral("Запустить решатель выбранным методом (F5). Пока кнопка не нажата, "
                       "изменения геометрии не пересчитываются."));
    start_simulation_action_->setShortcut(QKeySequence(Qt::Key_F5));
    connect(start_simulation_action_, &QAction::triggered, this, &MainWindow::runCalculation);
    home_simulation_group->addLargeButton(start_simulation_action_, RibbonIcon::Start);

    QAction *setup_solver_action =
        make_action(QStringLiteral("Настройка\nрешателя"),
                    QStringLiteral("Выбрать метод расчёта и уровень качества сетки"));
    connect(setup_solver_action, &QAction::triggered, this, &MainWindow::showSolverSetupDialog);
    home_simulation_group->addLargeButton(setup_solver_action, RibbonIcon::Setup);

    RibbonGroup *home_edit_group = home_tab->addGroup(QStringLiteral("Правка"));
    QAction *parameters_action =
        make_action(QStringLiteral("Параметры"),
                    QStringLiteral("Показать или скрыть список переменных модели"));
    parameters_action->setCheckable(true);
    parameters_action->setChecked(true);
    connect(parameters_action, &QAction::toggled, this, [this](bool visible) {
        if (parameter_dock_ != nullptr) {
            parameter_dock_->setVisible(visible);
        }
    });
    home_edit_group->addLargeButton(parameters_action, RibbonIcon::Parameters);

    QAction *waveguide_action = make_action(QStringLiteral("Волновод"),
                                            QStringLiteral("Размеры и материал стенок волновода"));
    connect(waveguide_action, &QAction::triggered, this, &MainWindow::showWaveguideDialog);
    home_edit_group->addSmallButton(waveguide_action, RibbonIcon::Waveguide);

    QAction *excitation_action =
        make_action(QStringLiteral("Возбуждение"), QStringLiteral("Частота и мода возбуждения"));
    connect(excitation_action, &QAction::triggered, this, &MainWindow::showExcitationDialog);
    home_edit_group->addSmallButton(excitation_action, RibbonIcon::Excitation);

    QAction *slot_action =
        make_action(QStringLiteral("Щель"), QStringLiteral("Параметры щели в стенке волновода"));
    connect(slot_action, &QAction::triggered, this, &MainWindow::showSlotDialog);
    home_edit_group->addSmallButton(slot_action, RibbonIcon::Slot);

    // ---------------------------------------------------------- Modeling ---
    RibbonTab *modeling_tab = ribbon_bar_->addRibbonTab(QStringLiteral("Modeling"));
    RibbonGroup *shapes_group = modeling_tab->addGroup(QStringLiteral("Объекты"));

    QAction *add_plate_action =
        make_action(QStringLiteral("Пластина"), QStringLiteral("Добавить металлическую пластину"));
    connect(add_plate_action, &QAction::triggered, this, [this]() { showPlateDialog(-1); });
    shapes_group->addLargeButton(add_plate_action, RibbonIcon::Plate);

    QAction *add_iris_action = make_action(
        QStringLiteral("Диафрагма"),
        QStringLiteral("Пластина во всё сечение волновода с прямоугольным окном внутри"));
    connect(add_iris_action, &QAction::triggered, this, [this]() { showPlateDialog(-1, true); });
    shapes_group->addLargeButton(add_iris_action, RibbonIcon::Iris);

    QAction *add_round_iris_action = make_action(
        QStringLiteral("Круглая\nсо штырём"),
        QStringLiteral("Пластина во всё сечение с круглым отверстием и соосным штырём внутри"));
    connect(add_round_iris_action, &QAction::triggered, this, [this]() {
        showPlateDialog(-1, true, true);
    });
    shapes_group->addLargeButton(add_round_iris_action, RibbonIcon::RoundIris);

    RibbonGroup *profile_group = modeling_tab->addGroup(QStringLiteral("Профиль волновода"));
    QAction *symmetric_profile_action =
        make_action(QStringLiteral("Симметричное\nсужение"),
                    QStringLiteral("Волновод сужается с обеих боковых стенок на участке по длине"));
    connect(symmetric_profile_action, &QAction::triggered, this, [this]() {
        insertProfileTemplate(true);
    });
    profile_group->addLargeButton(symmetric_profile_action, RibbonIcon::Profile);

    QAction *step_profile_action =
        make_action(QStringLiteral("Уступ\nна стенке"),
                    QStringLiteral("Волновод сужается с одной боковой стенки на участке по длине"));
    connect(step_profile_action, &QAction::triggered, this, [this]() {
        insertProfileTemplate(false);
    });
    profile_group->addLargeButton(step_profile_action, RibbonIcon::Profile);

    RibbonGroup *modeling_edit_group = modeling_tab->addGroup(QStringLiteral("Правка"));
    modeling_edit_group->addSmallButton(waveguide_action, RibbonIcon::Waveguide);
    modeling_edit_group->addSmallButton(slot_action, RibbonIcon::Slot);
    modeling_edit_group->addSmallButton(excitation_action, RibbonIcon::Excitation);

    // -------------------------------------------------------- Simulation ---
    RibbonTab *simulation_tab = ribbon_bar_->addRibbonTab(QStringLiteral("Simulation"));
    RibbonGroup *solver_group = simulation_tab->addGroup(QStringLiteral("Решатель"));
    solver_group->addLargeButton(start_simulation_action_, RibbonIcon::Start);
    solver_group->addLargeButton(setup_solver_action, RibbonIcon::Setup);

    RibbonGroup *settings_group = simulation_tab->addGroup(QStringLiteral("Настройки расчёта"));

    solver_method_combo_box_ = new QComboBox();
    for (int method = 0; method <= 4; ++method) {
        solver_method_combo_box_->addItem(solverMethodName(method));
    }
    solver_method_combo_box_->setCurrentIndex(std::clamp(parameters_.solver_method, 0, 4));
    solver_method_combo_box_->setToolTip(
        QStringLiteral("«Автоматически» подбирает самый быстрый подходящий метод. Явно выбранный "
                       "метод сообщит об ошибке, если геометрия ему не по силам."));
    connect(solver_method_combo_box_,
            qOverload<int>(&QComboBox::currentIndexChanged),
            this,
            [this](int index) {
                parameters_.solver_method = index;
                markModelChanged();
            });
    settings_group->addLabeledWidget(QStringLiteral("Метод"), solver_method_combo_box_);

    accuracy_combo_box_ = new QComboBox();
    accuracy_combo_box_->addItem(QStringLiteral("Быстро (грубая сетка)"));
    accuracy_combo_box_->addItem(QStringLiteral("Обычное"));
    accuracy_combo_box_->addItem(QStringLiteral("Высокое (мелкая сетка)"));
    accuracy_combo_box_->setCurrentIndex(std::clamp(parameters_.accuracy_level, 0, 2));
    accuracy_combo_box_->setToolTip(
        QStringLiteral("Влияет только на расчёт с пластинами, диафрагмой или щелью (FEM).\n"
                       "Более высокое качество убирает «мозаику» поля ценой времени расчёта."));
    connect(accuracy_combo_box_,
            qOverload<int>(&QComboBox::currentIndexChanged),
            this,
            [this](int index) {
                parameters_.accuracy_level = index;
                markModelChanged();
            });
    settings_group->addLabeledWidget(QStringLiteral("Качество"), accuracy_combo_box_);

    RibbonGroup *simulation_edit_group = simulation_tab->addGroup(QStringLiteral("Возбуждение"));
    simulation_edit_group->addLargeButton(excitation_action, RibbonIcon::Excitation);

    // --------------------------------------------------- Post-Processing ---
    RibbonTab *post_tab = ribbon_bar_->addRibbonTab(QStringLiteral("Post-Processing"));
    RibbonGroup *field_group = post_tab->addGroup(QStringLiteral("Поля"));
    QAction *fields_action =
        make_action(QStringLiteral("Отображение\nполей"),
                    QStringLiteral("Панель управления отображением полей слева"));
    connect(fields_action, &QAction::triggered, this, [this]() {
        if (field_mode_combo_box_ != nullptr) {
            field_mode_combo_box_->setFocus();
        }
    });
    field_group->addLargeButton(fields_action, RibbonIcon::Fields);

    RibbonGroup *report_group = post_tab->addGroup(QStringLiteral("Отчёт"));
    QAction *report_action = make_action(QStringLiteral("Итоги\nрасчёта"),
                                         QStringLiteral("Перейти к текстовому отчёту внизу окна"));
    connect(report_action, &QAction::triggered, this, [this]() {
        if (result_text_edit_ != nullptr) {
            result_text_edit_->setFocus();
        }
    });
    report_group->addLargeButton(report_action, RibbonIcon::Report);

    // ------------------------------------------------------------- View ----
    RibbonTab *view_tab = ribbon_bar_->addRibbonTab(QStringLiteral("View"));
    RibbonGroup *view_group = view_tab->addGroup(QStringLiteral("Вид"));
    QAction *reset_view_action =
        make_action(QStringLiteral("Сбросить\nвид"), QStringLiteral("Вернуть камеру в исходное положение"));
    connect(reset_view_action, &QAction::triggered, open_gl_widget_, &WaveguideOpenGLWidget::resetView);
    view_group->addLargeButton(reset_view_action, RibbonIcon::ResetView);

    RibbonGroup *panels_group = view_tab->addGroup(QStringLiteral("Панели"));
    panels_group->addSmallButton(parameters_action, RibbonIcon::Parameters);

    ribbon_bar_->setCurrentTabIndex(1);
}

void MainWindow::createParameterDock()
{
    parameter_dock_ = new QDockWidget(QStringLiteral("Список параметров"), this);
    parameter_dock_->setObjectName(QStringLiteral("parameterDock"));
    parameter_dock_->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);

    parameter_list_widget_ = new ParameterListWidget(&parameter_store_, parameter_dock_);
    parameter_dock_->setWidget(parameter_list_widget_);
    addDockWidget(Qt::BottomDockWidgetArea, parameter_dock_);
    resizeDocks({parameter_dock_}, {150}, Qt::Vertical);

    // Переменная сама по себе ничего не двигает: она влияет на модель только
    // через поля, куда вписано выражение. Поэтому здесь достаточно пометить
    // модель изменённой, чтобы пользователь заново нажал «Начать расчёт».
    connect(parameter_list_widget_,
            &ParameterListWidget::parametersEdited,
            this,
            &MainWindow::markModelChanged);
}

void MainWindow::showSolverSetupDialog()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Настройка решателя"));
    QVBoxLayout *layout = new QVBoxLayout(&dialog);

    QGroupBox *method_group = new QGroupBox(QStringLiteral("Метод расчёта"), &dialog);
    QFormLayout *method_layout = new QFormLayout(method_group);
    QComboBox *method_combo_box = new QComboBox(method_group);
    for (int method = 0; method <= 4; ++method) {
        method_combo_box->addItem(solverMethodName(method));
    }
    method_combo_box->setCurrentIndex(std::clamp(parameters_.solver_method, 0, 4));
    QLabel *method_hint = new QLabel(method_group);
    method_hint->setWordWrap(true);
    method_hint->setMinimumWidth(380);
    method_hint->setStyleSheet(QStringLiteral("color: #4a5a66;"));
    const auto update_method_hint = [method_hint](int method) {
        static const char *const hints[] = {
            "Диспетчер сам выбирает самый быстрый метод, способный описать текущую "
            "геометрию, и переходит к FEM, если другие неприменимы.",
            "Замкнутые формулы прямоугольного волновода. Мгновенно, но пластины, "
            "диафрагмы и диэлектрики игнорировать нельзя — расчёт откажется идти.",
            "Сшивание полей на плоской поперечной пластине. Быстро и точно для "
            "перегородок, перекрывающих часть сечения.",
            "Метод частичных областей для диафрагмы с прямоугольным окном: почти "
            "аналитическая точность S-параметров за секунды.",
            "Универсальный конечно-элементный расчёт. Считает любую геометрию, но "
            "заметно дольше; качество сетки задаётся ниже.",
        };
        method_hint->setText(QString::fromUtf8(hints[std::clamp(method, 0, 4)]));
    };
    update_method_hint(method_combo_box->currentIndex());
    connect(method_combo_box,
            qOverload<int>(&QComboBox::currentIndexChanged),
            &dialog,
            update_method_hint);
    method_layout->addRow(QStringLiteral("Метод"), method_combo_box);
    method_layout->addRow(method_hint);
    layout->addWidget(method_group);

    QGroupBox *quality_group = new QGroupBox(QStringLiteral("Качество сетки (FEM)"), &dialog);
    QFormLayout *quality_layout = new QFormLayout(quality_group);
    QComboBox *quality_combo_box = new QComboBox(quality_group);
    quality_combo_box->addItem(QStringLiteral("Быстро (грубая сетка)"));
    quality_combo_box->addItem(QStringLiteral("Обычное"));
    quality_combo_box->addItem(QStringLiteral("Высокое (мелкая сетка)"));
    quality_combo_box->setCurrentIndex(std::clamp(parameters_.accuracy_level, 0, 2));
    QLabel *quality_hint = new QLabel(quality_group);
    quality_hint->setWordWrap(true);
    quality_hint->setStyleSheet(QStringLiteral("color: #4a5a66;"));
    const auto update_quality_hint = [quality_hint](int level) {
        static const char *const hints[] = {
            "Замер: ~13 с (7 тыс. неизвестных). Поле заметно «мозаичное».",
            "Замер: ~1 мин (17 тыс.). Базовое качество.",
            "Замер: ~5 мин (40 тыс.). Мозаика слабее; S-параметры заметно точнее.",
        };
        quality_hint->setText(QString::fromUtf8(hints[std::clamp(level, 0, 2)]));
    };
    update_quality_hint(quality_combo_box->currentIndex());
    connect(quality_combo_box,
            qOverload<int>(&QComboBox::currentIndexChanged),
            &dialog,
            update_quality_hint);
    quality_layout->addRow(QStringLiteral("Качество"), quality_combo_box);
    quality_layout->addRow(quality_hint);
    layout->addWidget(quality_group);

    QDialogButtonBox *buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    const int method = method_combo_box->currentIndex();
    const int quality = quality_combo_box->currentIndex();
    if (method == parameters_.solver_method && quality == parameters_.accuracy_level) {
        return;
    }

    parameters_.solver_method = method;
    parameters_.accuracy_level = quality;
    if (solver_method_combo_box_ != nullptr) {
        const QSignalBlocker blocker(solver_method_combo_box_);
        solver_method_combo_box_->setCurrentIndex(method);
    }
    if (accuracy_combo_box_ != nullptr) {
        const QSignalBlocker blocker(accuracy_combo_box_);
        accuracy_combo_box_->setCurrentIndex(quality);
    }
    markModelChanged();
}

void MainWindow::updateSimulationActionState()
{
    if (start_simulation_action_ == nullptr) {
        return;
    }

    start_simulation_action_->setEnabled(!calculation_running_);
    if (calculation_running_) {
        start_simulation_action_->setText(QStringLiteral("Идёт\nрасчёт"));
    } else if (model_changed_since_run_) {
        start_simulation_action_->setText(QStringLiteral("Начать\nрасчёт"));
    } else {
        start_simulation_action_->setText(QStringLiteral("Пересчитать"));
    }
}

void MainWindow::updateWindowTitle()
{
    const QString base = QStringLiteral("Waveguide CST-like OpenGL");
    if (current_model_path_.isEmpty()) {
        setWindowTitle(base);
        return;
    }
    setWindowTitle(QStringLiteral("%1 — %2")
                       .arg(QFileInfo(current_model_path_).fileName(), base));
}

void MainWindow::applyLoadedParameters(const WaveguideParameters &parameters)
{
    parameters_ = parameters;
    selected_plate_index_ = parameters_.pec_plates.isEmpty() ? -1 : 0;

    if (slot_enabled_check_box_ != nullptr) {
        const QSignalBlocker blocker(slot_enabled_check_box_);
        slot_enabled_check_box_->setChecked(parameters_.slot_enabled);
    }
    if (accuracy_combo_box_ != nullptr) {
        const QSignalBlocker blocker(accuracy_combo_box_);
        accuracy_combo_box_->setCurrentIndex(std::clamp(parameters_.accuracy_level, 0, 2));
    }
    if (solver_method_combo_box_ != nullptr) {
        const QSignalBlocker blocker(solver_method_combo_box_);
        solver_method_combo_box_->setCurrentIndex(std::clamp(parameters_.solver_method, 0, 4));
    }
    if (parameter_list_widget_ != nullptr) {
        parameter_list_widget_->reload();
    }
    rebuildObjectTree();
    updateModelPreview();
}

void MainWindow::openModelFile()
{
    const QString path = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("Открыть модель волновода"),
        current_model_path_,
        QStringLiteral("Модель волновода (*.wgm);;Все файлы (*)"));
    if (path.isEmpty()) {
        return;
    }

    WaveguideParameters loaded;
    QString error;
    if (!model_io::loadModel(path, &loaded, &parameter_store_, &error)) {
        QMessageBox::warning(this, QStringLiteral("Открытие модели"), error);
        return;
    }

    current_model_path_ = path;
    updateWindowTitle();
    applyLoadedParameters(loaded);

    // Если рядом лежит файл расчёта именно для этой модели — показываем его
    // сразу, без пересчёта. Иначе считаем заново.
    const QString results_path = model_io::resultsPathFor(path);
    WaveguideCalculationResult cached;
    QString results_error;
    if (QFileInfo::exists(results_path) &&
        model_io::loadResults(results_path, parameters_, &cached, &results_error)) {
        // Отменяем возможный текущий расчёт, чтобы он не затёр загруженное.
        latest_request_id_ = 0;
        if (worker_ != nullptr) {
            worker_->setLatestRequestId(0);
        }
        calculation_running_ = false;
        model_changed_since_run_ = false;
        progress_timer_.stop();
        calculation_progress_bar_->setVisible(false);
        calculation_time_label_->setVisible(false);
        showResult(cached);
        updateSimulationActionState();
        setStatus(QStringLiteral("Модель и готовый расчёт загружены из %1")
                      .arg(QFileInfo(results_path).fileName()),
                  false);
        return;
    }

    if (!results_error.isEmpty()) {
        setStatus(QStringLiteral("%1 Пересчитываю...").arg(results_error), false);
    }
    markModelChanged();
}

bool MainWindow::writeModel(const QString &path)
{
    QString error;
    if (!model_io::saveModel(path, parameters_, &parameter_store_, &error)) {
        QMessageBox::warning(this, QStringLiteral("Сохранение модели"), error);
        return false;
    }
    current_model_path_ = path;
    updateWindowTitle();

    // Рядом с моделью кладём готовый расчёт, если он есть и актуален.
    const QString results_path = model_io::resultsPathFor(path);
    if (last_result_.valid) {
        QString results_error;
        if (model_io::saveResults(results_path, parameters_, last_result_, &results_error)) {
            setStatus(QStringLiteral("Сохранены модель и расчёт: %1 + %2")
                          .arg(QFileInfo(path).fileName(),
                               QFileInfo(results_path).fileName()),
                      false);
            return true;
        }
        setStatus(QStringLiteral("Модель сохранена, но расчёт — нет: %1").arg(results_error), true);
        return true;
    }
    setStatus(QStringLiteral("Модель сохранена: %1 (расчёт ещё не готов)")
                  .arg(QFileInfo(path).fileName()),
              false);
    return true;
}

bool MainWindow::saveModelFile()
{
    if (current_model_path_.isEmpty()) {
        return saveModelFileAs();
    }
    return writeModel(current_model_path_);
}

bool MainWindow::saveModelFileAs()
{
    QString path = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("Сохранить модель волновода"),
        current_model_path_.isEmpty() ? QStringLiteral("waveguide.wgm") : current_model_path_,
        QStringLiteral("Модель волновода (*.wgm)"));
    if (path.isEmpty()) {
        return false;
    }
    if (!path.endsWith(QStringLiteral(".wgm"), Qt::CaseInsensitive)) {
        path += QStringLiteral(".wgm");
    }
    return writeModel(path);
}

QString MainWindow::buildResultText(const WaveguideCalculationResult &result) const
{
    if (!result.valid) {
        return QStringLiteral("Расчет не выполнен:\n%1").arg(result.error_message);
    }

    QString text;
    text += QStringLiteral("Excitation\n");
    text += QStringLiteral("  Frequency: %1 GHz\n\n").arg(number(result.parameters.frequency_ghz, 4));

    text += QStringLiteral("Геометрия\n");
    text += QStringLiteral("  Внутренняя ширина: %1 мм\n").arg(number(result.inner_width_mm));
    text += QStringLiteral("  Внутренняя глубина: %1 мм\n").arg(number(result.inner_depth_mm));
    text += QStringLiteral("  Площадь сечения: %1 мм^2\n").arg(number(result.area_mm2));
    text += QStringLiteral("  Объем полости: %1 мм^3\n").arg(number(result.cavity_volume_mm3));
    text += QStringLiteral("  Объем металла: %1 мм^3\n\n").arg(number(result.metal_volume_mm3));

    if (result.parameters.slot_enabled) {
        text += QStringLiteral("Щель на выбранной стенке\n");
        text += QStringLiteral("  length z: %1 мм, width x: %2 мм\n")
                    .arg(number(result.parameters.slot_length_mm))
                    .arg(number(result.parameters.slot_width_mm));
        text += QStringLiteral("  offset x: %1 мм, offset z: %2 мм\n")
                    .arg(number(result.parameters.slot_offset_x_mm))
                    .arg(number(result.parameters.slot_offset_z_mm));
        text += QStringLiteral("  surface: %1, rotation: %2 deg\n")
                    .arg(slotSurfaceName(result.parameters.slot_surface))
                    .arg(number(result.parameters.slot_rotation_deg, 2));
        text += QStringLiteral("  Оценка возбуждения: k_slot = |J_perp| * |sinc(beta*l_slot/2)| = %1\n")
                    .arg(number(result.slot_normalized_coupling, 4));
        text += QStringLiteral("  Текущий backend не решает рассеяние щелью: ложное fringing-поле и токи в воздухе не синтезируются.\n\n");
    }

    if (!result.parameters.pec_plates.isEmpty()) {
        text += QStringLiteral("PEC-пластины\n");
        for (const PecPlateParameters &plate : result.parameters.pec_plates) {
            text += QStringLiteral("  %1: %2, [%3..%4] x [%5..%6] x [%7..%8] мм\n")
                        .arg(plate.name)
                        .arg(plate.enabled ? QStringLiteral("включена")
                                           : QStringLiteral("отключена"))
                        .arg(number(plate.x_min_mm))
                        .arg(number(plate.x_max_mm))
                        .arg(number(plate.y_min_mm))
                        .arg(number(plate.y_max_mm))
                        .arg(number(plate.z_min_mm))
                        .arg(number(plate.z_max_mm));
        }
        text += QLatin1Char('\n');
    }

    text += QStringLiteral("Решатель\n");
    text += QStringLiteral("  Backend: %1\n").arg(result.solver_backend);
    {
        static const char *const accuracy_names[] = {
            "быстро (грубая сетка)",
            "обычное",
            "высокое (мелкая сетка)",
        };
        const int level = std::clamp(result.parameters.accuracy_level, 0, 2);
        text += QStringLiteral("  Качество расчёта: %1\n")
                    .arg(QString::fromUtf8(accuracy_names[level]));
    }
    text += QStringLiteral("  Падающая мощность: %1 W\n")
                .arg(number(result.incident_power_w, 8));
    text += QStringLiteral("  Отраженная мощность: %1 W\n")
                .arg(number(result.reflected_power_w, 8));
    text += QStringLiteral("  Прошедшая мощность: %1 W\n")
                .arg(number(result.transmitted_power_w, 8));
    text += QStringLiteral("  Потери: %1 W\n")
                .arg(number(result.dissipated_power_w, 8));
    text += QStringLiteral("  |S11|: %1, |S21|: %2\n")
                .arg(number(result.s11_magnitude, 8))
                .arg(number(result.s21_magnitude, 8));
    text += QStringLiteral("  Ошибка баланса мощности: %1\n")
                .arg(number(result.power_balance_relative_error, 10));
    for (const QString &warning : result.solver_warnings) {
        text += QStringLiteral("  Предупреждение: %1\n").arg(warning);
    }
    text += QLatin1Char('\n');

    text += QStringLiteral("Моды\n");
    text += formatModeTable(result);
    text += QLatin1Char('\n');

    if (result.has_propagating_mode) {
        text += QStringLiteral("Выбранная распространяющаяся мода: %1\n").arg(result.selected_mode.name);
        text += QStringLiteral("  fc: %1 ГГц\n").arg(number(result.selected_mode.cutoff_ghz, 4));
        text += QStringLiteral("  lambda0: %1 мм\n").arg(number(result.wavelength0_mm));
        text += QStringLiteral("  lambda_g: %1 мм\n").arg(number(result.guide_wavelength_mm));
        text += QStringLiteral("  beta: %1 рад/м\n").arg(number(result.beta_rad_per_m));
        text += QStringLiteral("  alpha (полное): %1 Нп/м\n").arg(number(result.attenuation_np_per_m, 8));
        if (result.parameters.wall_conductivity_s_per_m > 0.0) {
            text += QStringLiteral("  alpha_c (стенки): %1 Нп/м = %2 дБ/м\n")
                        .arg(number(result.conductor_attenuation_np_per_m, 8))
                        .arg(number(result.conductor_attenuation_np_per_m * 8.685889638, 5));
        } else {
            text += QStringLiteral("  alpha_c (стенки): 0 (идеальный проводник)\n");
        }
        if (result.quality_factor > 0.0) {
            text += QStringLiteral("  Добротность Q (по потерям): %1\n")
                        .arg(number(result.quality_factor, 1));
        } else {
            text += QStringLiteral("  Добротность Q: бесконечна (потерь нет)\n");
        }
        text += QStringLiteral("  Запасённая энергия: W_e = %1 нДж, W_m = %2 нДж (при %3 Вт)\n")
                    .arg(number(result.stored_electric_energy_j * 1.0e9, 4))
                    .arg(number(result.stored_magnetic_energy_j * 1.0e9, 4))
                    .arg(number(result.input_power_w, 4));
        text += QStringLiteral("  Фазоры используют e^(j*w*t), распространение e^(-gamma*z).\n");
        text += QStringLiteral("  Линии E/H интегрируются по Re(F*e^(j*pi/4)) методом RK4.\n");
        text += QStringLiteral("  Поверхностный ток вычисляется строго как J_s = n x H.\n");
        text += QStringLiteral("  S = 0.5*Re(E x conj(H)) показан отдельным режимом.\n");
    } else {
        text += QStringLiteral("На заданной частоте ниже cutoff нет распространяющейся моды.\n");
    }

    return text;
}

QString MainWindow::formatModeTable(const WaveguideCalculationResult &result) const
{
    QString text;
    for (const WaveguideMode &mode : result.modes) {
        text += QStringLiteral("  %1  fc=%2 ГГц  %3\n")
                    .arg(mode.name, -5)
                    .arg(number(mode.cutoff_ghz, 4), 9)
                    .arg(mode.propagates ? QStringLiteral("распространяется")
                                         : QStringLiteral("ниже cutoff"));
    }

    return text;
}

void MainWindow::setStatus(const QString &message, bool error)
{
    status_label_->setText(message);
    status_label_->setStyleSheet(error
                                     ? QStringLiteral("color: #ff7b72; font-weight: 600;")
                                     : QStringLiteral("color: #8fd694; font-weight: 600;"));
}

