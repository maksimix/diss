#pragma once

#include <QtGui/QIcon>
#include <QtWidgets/QWidget>

class QAction;
class QHBoxLayout;
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
    Excitation,
    Fields,
    ResetView,
    Report
};

QIcon ribbonIcon(RibbonIcon icon);

// Группа ленты — колонка кнопок с подписью снизу, как «Clipboard» или
// «Simulation» в CST.
class RibbonGroup : public QWidget
{
    Q_OBJECT

public:
    explicit RibbonGroup(const QString &title, QWidget *parent = nullptr);

    // Крупная кнопка с картинкой сверху и текстом снизу.
    QToolButton *addLargeButton(QAction *action, RibbonIcon icon);
    // Мелкие кнопки укладываются по три в колонку, значок слева от текста.
    QToolButton *addSmallButton(QAction *action, RibbonIcon icon);
    // Произвольный виджет (комбобокс, спинбокс) с подписью слева.
    void addLabeledWidget(const QString &label, QWidget *widget);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QVBoxLayout *smallButtonColumn();

    QHBoxLayout *content_layout_ = nullptr;
    QVBoxLayout *small_column_ = nullptr;
    int small_button_count_ = 0;
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

// Лента целиком: строка вкладок (File, Home, Modeling, ...) и панель под ней.
class RibbonBar : public QWidget
{
    Q_OBJECT

public:
    explicit RibbonBar(QWidget *parent = nullptr);

    RibbonTab *addRibbonTab(const QString &title);
    void setCurrentTabIndex(int index);

private:
    QTabBar *tab_bar_ = nullptr;
    QStackedWidget *pages_ = nullptr;
};
