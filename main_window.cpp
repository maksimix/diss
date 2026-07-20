#include "main_window.h"

#include "calculation_worker.h"
#include "waveguide_opengl_widget.h"

#include <QtCore/QLocale>
#include <QtCore/QSignalBlocker>
#include <QtGui/QPainter>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialog>
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
    setWindowTitle(QStringLiteral("Waveguide CST-like OpenGL"));
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
    setCentralWidget(vertical_splitter);

    open_gl_widget_->setSlotEditedCallback([this](const WaveguideParameters &parameters) {
        applyInteractiveSlotParameters(parameters);
    });

    calculation_timer_.setSingleShot(true);
    calculation_timer_.setInterval(240);
    connect(&calculation_timer_, &QTimer::timeout, this, &MainWindow::runCalculation);
    progress_timer_.setInterval(1000);
    connect(&progress_timer_, &QTimer::timeout, this, &MainWindow::updateCalculationProgress);

    worker_ = new CalculationWorker();
    worker_->moveToThread(&worker_thread_);
    connect(&worker_thread_, &QThread::finished, worker_, &QObject::deleteLater);
    connect(this, &MainWindow::requestCalculation, worker_, &CalculationWorker::calculate);
    connect(worker_, &CalculationWorker::calculated, this, &MainWindow::handleCalculationResult);
    connect(worker_, &CalculationWorker::progressed, this, &MainWindow::handleCalculationProgress);
    worker_thread_.start();

    scheduleCalculation();
}

MainWindow::~MainWindow()
{
    if (worker_ != nullptr) {
        worker_->setLatestRequestId(0);
    }
    worker_thread_.quit();
    worker_thread_.wait();
}

void MainWindow::scheduleCalculation()
{
    latest_request_id_ = 0;
    if (worker_ != nullptr) {
        worker_->setLatestRequestId(0);
    }
    updateModelPreview();
    setStatus(QStringLiteral("Параметры изменены, готовлю перерасчет..."), false);
    calculation_timer_.start();
}

void MainWindow::runCalculation()
{
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
    calculation_stage_.clear();
    progress_timer_.stop();
    calculation_progress_bar_->setValue(100);
    calculation_time_label_->setText(
        QStringLiteral("Завершено за %1").arg(formattedDuration(elapsed_ms)));
    if (active_calculation_is_fem_ && elapsed_ms > 0) {
        measured_fem_duration_ms_ = measured_fem_duration_ms_ > 0
                                        ? (2 * measured_fem_duration_ms_ + elapsed_ms) / 3
                                        : elapsed_ms;
    }

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

    add_plate_button_ = new QPushButton(QStringLiteral("Добавить пластину"), panel);
    panel_layout->addWidget(add_plate_button_);

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
                scheduleCalculation();
            });
    connect(add_plate_button_, &QPushButton::clicked, this, [this]() {
        showPlateDialog(-1);
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
    QDoubleSpinBox *spin_box = new QDoubleSpinBox(parent ? parent : this);
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
    scheduleCalculation();
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
        scheduleCalculation();
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
        scheduleCalculation();
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

void MainWindow::showPlateDialog(int plate_index)
{
    const bool creating = plate_index < 0 || plate_index >= parameters_.pec_plates.size();
    PecPlateParameters plate;
    if (!creating) {
        plate = parameters_.pec_plates[plate_index];
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
    grid_layout->addWidget(new QLabel(QStringLiteral("Component:"), &dialog), 12, 0);
    grid_layout->addWidget(new QLabel(QStringLiteral("Material:"), &dialog), 12, 1);
    grid_layout->addWidget(component_combo_box, 13, 0);
    grid_layout->addWidget(material_combo_box, 13, 1);

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
        if (creating) {
            parameters_.pec_plates.push_back(plate);
            selected_plate_index_ = parameters_.pec_plates.size() - 1;
        } else {
            parameters_.pec_plates[plate_index] = plate;
            selected_plate_index_ = plate_index;
        }
        rebuildObjectTree();
        scheduleCalculation();
        dialog.accept();
    });
    connect(cancel_button, &QPushButton::clicked, &dialog, &QDialog::reject);
    connect(delete_button, &QPushButton::clicked, &dialog, [&]() {
        if (!creating && plate_index >= 0 && plate_index < parameters_.pec_plates.size()) {
            parameters_.pec_plates.removeAt(plate_index);
            selected_plate_index_ = -1;
            rebuildObjectTree();
            scheduleCalculation();
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
        scheduleCalculation();
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

