#pragma once

#include <QtWidgets/QWidget>

class ParameterStore;
class QPushButton;
class QTableWidget;
class QTableWidgetItem;

// Список переменных модели — аналог панели Parameter List внизу окна CST.
// Строки редактируются прямо в таблице, колонка «Значение» показывает результат
// вычисления выражения или текст ошибки.
class ParameterListWidget : public QWidget
{
    Q_OBJECT

public:
    explicit ParameterListWidget(ParameterStore *store, QWidget *parent = nullptr);

    // Перечитывает таблицу из хранилища (после загрузки модели с диска).
    void reload();

signals:
    // Пользователь изменил набор переменных: геометрию нужно пересобрать.
    void parametersEdited();

private:
    void addParameterRow();
    void deleteSelectedRows();
    void handleItemChanged(QTableWidgetItem *item);
    void writeStoreFromTable();
    void refreshValueColumn();

    ParameterStore *store_ = nullptr;
    QTableWidget *table_ = nullptr;
    QPushButton *add_button_ = nullptr;
    QPushButton *delete_button_ = nullptr;
    bool updating_ = false;
};
