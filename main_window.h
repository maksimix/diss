#pragma once

#include "model_parameters.h"
#include "waveguide_types.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtWidgets/QMainWindow>

#include <memory>

class CalculationWorker;
class CstPanel;
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
class QSlider;
class QStackedWidget;
class QTreeWidget;
class QTreeWidgetItem;
class QVBoxLayout;
class WaveguideOpenGLWidget;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

signals:
    void requestCalculation(int request_id,
                            const WaveguideParameters &parameters,
                            double arrow_density);
    // Перестройка стрелок по готовому решению в рабочем потоке: решатель не
    // запускается, поэтому запрос дешёвый и идёт при каждом сдвиге ползунка.
    void requestGlyphRegeneration(int glyph_request_id,
                                  std::shared_ptr<const em::FieldSolution> field_solution,
                                  double arrow_density);
    // Срез |E| на смещённой плоскости и стопка срезов объёмной заливки — тоже
    // по готовому решению, без пересчёта задачи.
    void requestSliceRebuild(int fill_request_id,
                             std::shared_ptr<const em::FieldSolution> field_solution,
                             int slice_plane,
                             double offset_fraction);
    void requestVolumeFill(int fill_request_id,
                           std::shared_ptr<const em::FieldSolution> field_solution,
                           int slice_plane,
                           int slice_count);

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    bool saveModelFile();
    bool saveModelFileAs();
    // Управление проектами (см. блок методов ниже).
    void showStartPage();
    void newProject();
    void openProject();
    void closeActiveProject();
    // Помечает модель изменённой и обновляет предпросмотр, но расчёт не
    // запускает: в CST решатель стартует только по кнопке Start Simulation.
    void markModelChanged();
    void runCalculation();
    void handleCalculationResult(int request_id, const WaveguideCalculationResult &result);
    void handleCalculationProgress(int request_id, const QString &stage);
    void changeFieldDisplayMode(int index);
    void updateCalculationProgress();
    // Запускает перестройку стрелок с текущей концентрацией (после паузы
    // ползунка) и принимает её результат из рабочего потока.
    void regenerateFieldGlyphs();
    void handleGlyphsRegenerated(int glyph_request_id, const QVector<FieldGlyph> &glyphs);
    // Заливка |E|: перенос плоскости среза и стопка объёма строятся в рабочем
    // потоке по сохранённому решению, эти слоты принимают результат.
    void handleSliceRebuilt(int fill_request_id, int slice_plane, const FieldSlice &slice);
    void handleVolumeFillBuilt(int fill_request_id,
                               int slice_plane,
                               const QVector<FieldSlice> &slices);

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
    void filterObjectTree(const QString &text);
    void handleObjectSelectionChanged();
    void updateModelPreview();
    void handleObjectDoubleClick(QTreeWidgetItem *item, int column);
    void showWaveguideDialog();
    void showSlotDialog();
    void insertProfileTemplate(bool symmetric);
    void showPlateDialog(int plate_index = -1,
                         bool iris_template = false,
                         bool round_post_template = false);
    // Диалог свободного тела: index < 0 — создание нового тела вида new_kind
    // (0 — брусок, 1 — цилиндр, 2 — призма), иначе правка существующего.
    void showShapeDialog(int shape_index = -1, int new_kind = 0);
    // Готовые наборы тел: перегородки в плоскости H, диафрагма с C-окном,
    // диафрагма со встречными Г-выступами, T-образные вставки круглого тракта.
    void insertShapeTemplate(int shape_template);
    void showExcitationDialog();
    void showSolverSetupDialog();
    void showAboutDialog();
    void applyCstStyle();
    void setStatus(const QString &message, bool error);
    void createRibbon();
    void createParameterDock();
    void createStatusBar();
    QString documentName() const;
    // Приводит подписи и доступность кнопок ленты к текущему состоянию:
    // запущен ли расчёт и есть ли несохранённые изменения геометрии.
    void updateSimulationActionState();
    QString solverMethodName(int method) const;
    QString linearSolverMethodName(int method) const;
    // Концентрация стрелок E/H/J по положению ползунка (1.0 — обычная).
    double arrowDensity() const;
    // Текущая плоскость среза по комбобоксу и её желаемое смещение по
    // ползунку (доля поперечного размера, [-0.48, 0.48]).
    FieldSlicePlane activeSlicePlane() const;
    double sliceOffsetFraction(FieldSlicePlane plane) const;
    // Применяет выбор «выключена/срез/объём» к виджетам и дозаказывает
    // недостающие данные (смещённый срез или стопку объёма).
    void applyFieldFillMode();
    // Просит рабочий поток пересобрать срез на текущем смещении.
    void requestActiveSliceRebuild();
    // Просит стопку срезов объёмной заливки для текущей плоскости.
    void requestVolumeFillRebuild();
    // Ожидаемая цена уровня качества: одна и та же строка идёт в подсказку ленты
    // и в диалог настройки решателя, чтобы числа не разъезжались.
    QString accuracyLevelHint(int level) const;
    QString linearSolverHint(int method) const;
    void applyLoadedParameters(const WaveguideParameters &parameters);
    void showResult(const WaveguideCalculationResult &result);
    bool writeModel(const QString &path);
    void updateWindowTitle();

    // ---------------------------------------------------- проекты -----------
    // Один открытый проект: полное состояние документа. Активный проект живёт в
    // «живых» полях окна (parameters_, last_result_ и т.д.); при переключении
    // вкладок оно снимается сюда (harvest) и восстанавливается обратно (apply).
    struct OpenProject
    {
        QString directory;   // папка проекта
        QString file_path;   // <папка>/<имя>.wgproj
        QString name;        // отображаемое имя (база имени файла)
        WaveguideParameters parameters;
        QVector<ModelParameter> variables;   // снимок списка параметров
        WaveguideCalculationResult result;
        QString waveguide_name;
        QString slot_name;
        QString excitation_name;
        int selected_plate_index = -1;
        bool dirty = false;                  // есть несохранённые изменения
        bool model_changed_since_run = true; // показанное поле старше геометрии
    };

    QWidget *buildStartPage();
    void refreshStartPageRecents();
    void newProjectFromPreset(int preset);
    bool createProjectOnDisk(const QString &parent_dir,
                             const QString &name,
                             int preset,
                             QString *created_file,
                             QString *error);
    void openProjectPath(const QString &file_path);
    bool closeProject(int project_index);
    void activateProject(int project_index);
    void harvestActiveProject();
    void applyActiveProject();
    void cancelRunningCalculation();
    // Убирает с видов поле прежнего проекта: setModelPreview обновляет только
    // геометрию, а стрелки, срезы и стопка объёма остались бы от чужого расчёта.
    void clearFieldDisplay();
    bool saveProjectSnapshot(const OpenProject &project, QString *error);
    void updateProjectActionsEnabled();
    void setDocumentDirty(bool dirty);
    QString projectTabTitle(int project_index) const;
    QString projectsBaseDir() const;
    void rememberProjectsBaseDir(const QString &dir);
    QStringList recentProjectPaths() const;
    void pushRecentProject(const QString &file_path);
    void removeRecentProject(const QString &file_path);
    static WaveguideParameters presetParameters(int preset);
    static QString presetName(int preset);
    static QString presetDescription(int preset);

    WaveguideOpenGLWidget *open_gl_widget_ = nullptr;
    WaveguideOpenGLWidget *top_projection_widget_ = nullptr;
    WaveguideOpenGLWidget *side_projection_widget_ = nullptr;
    QCheckBox *slot_enabled_check_box_ = nullptr;
    QTreeWidget *object_tree_widget_ = nullptr;
    QComboBox *field_mode_combo_box_ = nullptr;
    QComboBox *field_fill_combo_box_ = nullptr;
    QCheckBox *animation_check_box_ = nullptr;
    QSlider *arrow_density_slider_ = nullptr;
    QLabel *arrow_density_value_label_ = nullptr;
    QSlider *slice_position_slider_ = nullptr;
    QLabel *slice_position_value_label_ = nullptr;
    QComboBox *slice_plane_combo_box_ = nullptr;
    QComboBox *accuracy_combo_box_ = nullptr;
    QComboBox *solver_method_combo_box_ = nullptr;
    QComboBox *linear_solver_combo_box_ = nullptr;
    RibbonBar *ribbon_bar_ = nullptr;
    CstPanel *navigation_panel_ = nullptr;
    QStackedWidget *workspace_stack_ = nullptr;   // 0 — стартовая страница, 1 — работа
    QWidget *start_page_ = nullptr;
    QVBoxLayout *start_recent_layout_ = nullptr;
    QLabel *start_recent_empty_ = nullptr;
    QVector<OpenProject> projects_;
    int active_project_ = -1;
    bool document_dirty_ = false;
    QAction *start_simulation_action_ = nullptr;
    QAction *close_project_action_ = nullptr;
    QAction *save_action_ = nullptr;
    QAction *save_as_action_ = nullptr;
    QAction *parameters_action_ = nullptr;
    // Команды, которым нужен открытый проект: на стартовой странице они
    // выключены, чтобы кнопка не правила несуществующую модель.
    QVector<QAction *> project_scoped_actions_;
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

    // Поток живёт в куче, а не полем: расчёт может не успеть остановиться к
    // закрытию окна, а ~QThread на работающем потоке — это qFatal. Тогда объект
    // сознательно утекает, см. ~MainWindow.
    QThread *worker_thread_ = nullptr;
    CalculationWorker *worker_ = nullptr;
    QTimer progress_timer_;
    // Пауза после сдвига ползунка концентрации: стрелки перестраиваются один
    // раз по конечному положению, а не на каждый шаг ползунка.
    QTimer arrow_density_timer_;
    // Такая же пауза для ползунка положения среза.
    QTimer slice_position_timer_;
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
    // Счётчики запросов перестройки стрелок — отдельные от счётчиков расчёта,
    // чтобы сдвиг ползунка не отменял идущий расчёт поля.
    int next_glyph_request_id_ = 1;
    int latest_glyph_request_id_ = 0;
    // Счётчики заливки |E| (перенос среза и стопка объёма делят одно
    // пространство номеров: новый запрос отменяет предыдущий).
    int next_fill_request_id_ = 1;
    int latest_fill_request_id_ = 0;
    // Желаемое смещение плоскости среза на каждую ориентацию (доля
    // поперечника) и фактически применённое к показанным срезам: после нового
    // расчёта срезы снова центральные, и несовпадение запускает перестройку.
    double slice_offset_fraction_[2] = {0.0, 0.0};
    double applied_slice_offset_fraction_[2] = {0.0, 0.0};
    // Стопка объёмной заливки: для какой плоскости построен кэш (-1 — пусто).
    int volume_cache_plane_ = -1;
    QVector<FieldSlice> volume_cache_;
    int selected_plate_index_ = -1;
    int selected_shape_index_ = -1;
};
