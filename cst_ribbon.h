#pragma once

#include <QtGui/QIcon>
#include <QtWidgets/QWidget>

class QAction;
class QCompleter;
class QGridLayout;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QMenu;
class QStringListModel;
class QTabBar;
class QStackedWidget;
class QToolButton;
class QVBoxLayout;

// Рисуемые в коде пиктограммы ленты: внешних ресурсов у проекта нет, поэтому
// значки строятся из простых фигур в стиле CST (плоские, с цветовым акцентом).
enum class RibbonIcon
{
    Open,
    Save,
    SaveAs,
    Quit,
    Start,
    Setup,
    Parameters,
    Waveguide,
    Slot,
    Plate,
    Iris,
    RoundIris,
    Profile,
    ProfileStep,
    Excitation,
    Fields,
    ResetView,
    Report,
    Project,
    Tree,
    Search,
    Help,
    Collapse,
    Expand
};

// Рисунок задан в сетке 32x32 и масштабируется под запрошенный размер, поэтому
// мелкие значки шапки не «мылятся».
QIcon ribbonIcon(RibbonIcon icon, int size = 32);

// Группа ленты — колонка кнопок с подписью снизу, как «Clipboard» или
// «Simulation» в CST.
class RibbonGroup : public QWidget
{
    Q_OBJECT

public:
    explicit RibbonGroup(const QString &title, QWidget *parent = nullptr);

    // Крупная кнопка с картинкой сверху и текстом снизу. Если передано меню,
    // под текстом появляется стрелка — как у split-кнопок CST.
    QToolButton *addLargeButton(QAction *action, RibbonIcon icon, QMenu *menu = nullptr);
    // Мелкие кнопки укладываются по три в колонку, значок слева от текста.
    QToolButton *addSmallButton(QAction *action, RibbonIcon icon);
    // Кнопка без подписи для плотной сетки значков — так устроена группа
    // «Shapes» на вкладке Modeling: два ряда пиктограмм подряд.
    QToolButton *addIconButton(QAction *action, RibbonIcon icon);
    // Произвольный виджет (комбобокс, спинбокс) с подписью слева.
    void addLabeledWidget(const QString &label, QWidget *widget);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QVBoxLayout *smallButtonColumn();
    QGridLayout *iconGrid();
    void registerForSearch(QAction *action);

    QHBoxLayout *content_layout_ = nullptr;
    QVBoxLayout *small_column_ = nullptr;
    QGridLayout *icon_grid_ = nullptr;
    int small_button_count_ = 0;
    int icon_button_count_ = 0;
};

// Одна вкладка ленты: горизонтальный ряд групп.
class RibbonTab : public QWidget
{
    Q_OBJECT

public:
    explicit RibbonTab(QWidget *parent = nullptr);

    RibbonGroup *addGroup(const QString &title);

private:
    QHBoxLayout *layout_ = nullptr;
};

// Лента целиком: синяя полоса вкладок с полем поиска (File, Home, Modeling,
// ...), белое поле групп под ней и «документная» вкладка открытой модели.
class RibbonBar : public QWidget
{
    Q_OBJECT

public:
    explicit RibbonBar(QWidget *parent = nullptr);

    RibbonTab *addRibbonTab(const QString &title);
    void setCurrentTabIndex(int index);
    // Имя открытой модели на документной вкладке под лентой.
    void setDocumentName(const QString &name);
    // Команда попадает в строку поиска (Alt+Q); группы ленты вызывают это сами
    // для каждой добавленной кнопки.
    void registerSearchAction(QAction *action);

private:
    void setRibbonCollapsed(bool collapsed);
    void activateSearchResult(const QString &text);

    QTabBar *tab_bar_ = nullptr;
    QStackedWidget *pages_ = nullptr;
    QTabBar *document_tab_bar_ = nullptr;
    QLineEdit *search_edit_ = nullptr;
    QStringListModel *search_model_ = nullptr;
    QToolButton *collapse_button_ = nullptr;
    QList<QAction *> search_actions_;
};

// Заголовок панели: светлая полоса с названием слева и крестиком справа.
class CstPanelHeader : public QWidget
{
    Q_OBJECT

public:
    explicit CstPanelHeader(const QString &title, QWidget *parent = nullptr);

    void setClosable(bool closable);

signals:
    void closeRequested();

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QString title_;
    QToolButton *close_button_ = nullptr;
};

// Панель рабочей области в стиле CST: узкий заголовок со светлой полосой,
// крестиком справа и содержимым под ним («Navigation Tree», «Messages»).
class CstPanel : public QWidget
{
    Q_OBJECT

public:
    explicit CstPanel(const QString &title, QWidget *parent = nullptr);

    void setContent(QWidget *content);
    void setClosable(bool closable);

signals:
    void closeRequested();

private:
    QVBoxLayout *layout_ = nullptr;
    CstPanelHeader *header_ = nullptr;
};
