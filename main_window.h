#pragma once

#include "model_parameters.h"
#include "waveguide_types.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtWidgets/QMainWindow>

class CalculationWorker;
class FieldColorBar;
class ParameterListWidget;
class RibbonBar;
class RibbonGroup;
class QAction;
class QDockWidget;
class QLabel;
class QCheckBox;
class QComboBox;
class QLineEdit;
class QDoubleSpinBox;
class QPlainTextEdit;
class QPushButton;
class QProgressBar;
class QTreeWidget;
class QTreeWidgetItem;
class WaveguideOpenGLWidget;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

signals:
    void requestCalculation(int request_id, const WaveguideParameters &parameters);

private slots:
    void openModelFile();
    bool saveModelFile();
    bool saveModelFileAs();
    // Помечает модель изменённой и обновляет предпросмотр, но расчёт не
    // запускает: в CST решатель стартует только по кнопке Start Simulation.
    void markModelChanged();
    void runCalculation();
    void handleCalculationResult(int request_id, const WaveguideCalculationResult &result);
    void handleCalculationProgress(int request_id, const QString &stage);
    void changeFieldDisplayMode(int index);
    void updateCalculationProgress();

private:
    QWidget *createParameterPanel();
    QWidget *createResultPanel();
    QWidget *createProjectionPanel();
    QDoubleSpinBox *createSpinBox(double minimum,
                                  double maximum,
                                  double value,
                                  double step,
                                  int decimals,
                                  const QString &suffix,
                                  QWidget *parent = nullptr);
    WaveguideParameters readParameters() const;
    QString buildResultText(const WaveguideCalculationResult &result) const;
    QString formatModeTable(const WaveguideCalculationResult &result) const;
    void applyInteractiveSlotParameters(const WaveguideParameters &parameters);
    void rebuildObjectTree();
    void handleObjectSelectionChanged();
    void updateModelPreview();
    void handleObjectDoubleClick(QTreeWidgetItem *item, int column);
    void showWaveguideDialog();
    void showSlotDialog();
    void insertProfileTemplate(bool symmetric);
    void showPlateDialog(int plate_index = -1,
                         bool iris_template = false,
                         bool round_post_template = false);
    void showExcitationDialog();
    void showSolverSetupDialog();
    void applyCstStyle();
    void setStatus(const QString &message, bool error);
    void createRibbon();
    void createParameterDock();
    // Приводит подписи и доступность кнопок ленты к текущему состоянию:
    // запущен ли расчёт и есть ли несохранённые изменения геометрии.
    void updateSimulationActionState();
    QString solverMethodName(int method) const;
    void applyLoadedParameters(const WaveguideParameters &parameters);
    void showResult(const WaveguideCalculationResult &result);
    bool writeModel(const QString &path);
    void updateWindowTitle();

    WaveguideOpenGLWidget *open_gl_widget_ = nullptr;
    WaveguideOpenGLWidget *top_projection_widget_ = nullptr;
    WaveguideOpenGLWidget *side_projection_widget_ = nullptr;
    QCheckBox *slot_enabled_check_box_ = nullptr;
    QTreeWidget *object_tree_widget_ = nullptr;
    QComboBox *field_mode_combo_box_ = nullptr;
    QCheckBox *slice_check_box_ = nullptr;
    QCheckBox *animation_check_box_ = nullptr;
    QComboBox *slice_plane_combo_box_ = nullptr;
    QComboBox *accuracy_combo_box_ = nullptr;
    QComboBox *solver_method_combo_box_ = nullptr;
    RibbonBar *ribbon_bar_ = nullptr;
    QAction *start_simulation_action_ = nullptr;
    QDockWidget *parameter_dock_ = nullptr;
    ParameterListWidget *parameter_list_widget_ = nullptr;
    ParameterStore parameter_store_;
    FieldColorBar *color_bar_ = nullptr;
    QPlainTextEdit *result_text_edit_ = nullptr;
    QLabel *status_label_ = nullptr;
    QLabel *calculation_time_label_ = nullptr;
    QProgressBar *calculation_progress_bar_ = nullptr;
    WaveguideParameters parameters_;
    WaveguideCalculationResult last_result_;
    QString current_model_path_;
    QString waveguide_name_ = QStringLiteral("wr-90");
    QString slot_name_ = QStringLiteral("figure_1");
    QString excitation_name_ = QStringLiteral("signal1");

    QThread worker_thread_;
    CalculationWorker *worker_ = nullptr;
    QTimer progress_timer_;
    QElapsedTimer calculation_elapsed_timer_;
    QString calculation_stage_;
    qint64 estimated_duration_ms_ = 120000;
    qint64 measured_fem_duration_ms_ = 0;
    bool calculation_running_ = false;
    // Геометрия менялась после последнего успешного расчёта: показанное поле
    // относится к прежней модели.
    bool model_changed_since_run_ = true;
    bool active_calculation_is_fem_ = false;
    int next_request_id_ = 1;
    int latest_request_id_ = 0;
    int selected_plate_index_ = -1;
};
