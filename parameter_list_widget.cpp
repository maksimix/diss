#include "parameter_list_widget.h"

#include "model_parameters.h"

#include <QtCore/QLocale>
#include <QtCore/QSet>
#include <QtGui/QBrush>
#include <QtGui/QColor>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QTableWidgetItem>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <functional>

namespace
{
constexpr int name_column = 0;
constexpr int expression_column = 1;
constexpr int value_column = 2;
constexpr int description_column = 3;
}

ParameterListWidget::ParameterListWidget(ParameterStore *store, QWidget *parent)
    : QWidget(parent)
    , store_(store)
{
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    table_ = new QTableWidget(0, 4, this);
    table_->setHorizontalHeaderLabels({QStringLiteral("Имя"),
                                       QStringLiteral("Выражение"),
                                       QStringLiteral("Значение"),
                                       QStringLiteral("Описание")});
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->horizontalHeader()->setSectionResizeMode(name_column, QHeaderView::Interactive);
    table_->setColumnWidth(name_column, 120);
    table_->setColumnWidth(expression_column, 160);
    table_->setColumnWidth(value_column, 110);
    table_->verticalHeader()->setVisible(false);
    table_->setAlternatingRowColors(true);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    layout->addWidget(table_, 1);

    QHBoxLayout *buttons = new QHBoxLayout();
    buttons->setContentsMargins(0, 0, 0, 0);
    add_button_ = new QPushButton(QStringLiteral("Новый параметр"), this);
    delete_button_ = new QPushButton(QStringLiteral("Удалить"), this);
    buttons->addWidget(add_button_);
    buttons->addWidget(delete_button_);
    buttons->addStretch(1);
    layout->addLayout(buttons);

    connect(add_button_, &QPushButton::clicked, this, &ParameterListWidget::addParameterRow);
    connect(delete_button_, &QPushButton::clicked, this, &ParameterListWidget::deleteSelectedRows);
    connect(table_, &QTableWidget::itemChanged, this, &ParameterListWidget::handleItemChanged);

    reload();
}

void ParameterListWidget::reload()
{
    updating_ = true;
    table_->setRowCount(0);
    for (const ModelParameter &parameter : store_->entries()) {
        const int row = table_->rowCount();
        table_->insertRow(row);
        table_->setItem(row, name_column, new QTableWidgetItem(parameter.name));
        table_->setItem(row, expression_column, new QTableWidgetItem(parameter.expression));
        QTableWidgetItem *value_item = new QTableWidgetItem();
        value_item->setFlags(value_item->flags() & ~Qt::ItemIsEditable);
        table_->setItem(row, value_column, value_item);
        table_->setItem(row, description_column, new QTableWidgetItem(parameter.description));
    }
    updating_ = false;
    refreshValueColumn();
}

void ParameterListWidget::addParameterRow()
{
    // Имя по умолчанию не должно совпадать с уже занятым, иначе строка сразу
    // будет отброшена при переносе таблицы в хранилище.
    int index = store_->entries().size() + 1;
    QString name = QStringLiteral("par%1").arg(index);
    while (store_->contains(name)) {
        name = QStringLiteral("par%1").arg(++index);
    }

    store_->set(name, QStringLiteral("1"), QString());
    reload();
    const int row = table_->rowCount() - 1;
    table_->setCurrentCell(row, name_column);
    table_->editItem(table_->item(row, name_column));
    emit parametersEdited();
}

void ParameterListWidget::deleteSelectedRows()
{
    const QList<QTableWidgetItem *> selected = table_->selectedItems();
    QSet<int> rows;
    for (const QTableWidgetItem *item : selected) {
        rows.insert(item->row());
    }
    if (rows.isEmpty()) {
        return;
    }

    QList<int> sorted = rows.values();
    std::sort(sorted.begin(), sorted.end(), std::greater<int>());
    updating_ = true;
    for (const int row : sorted) {
        table_->removeRow(row);
    }
    updating_ = false;
    writeStoreFromTable();
    refreshValueColumn();
    emit parametersEdited();
}

void ParameterListWidget::handleItemChanged(QTableWidgetItem *item)
{
    if (updating_ || item == nullptr || item->column() == value_column) {
        return;
    }

    if (item->column() == name_column) {
        const QString name = item->text().trimmed();
        if (!ParameterStore::isValidName(name)) {
            item->setToolTip(QStringLiteral(
                "Имя должно начинаться с буквы или подчёркивания и состоять из букв, цифр и "
                "подчёркиваний."));
            item->setBackground(QColor(255, 226, 226));
            return;
        }
        item->setToolTip(QString());
        item->setBackground(QBrush());
    }

    writeStoreFromTable();
    refreshValueColumn();
    emit parametersEdited();
}

void ParameterListWidget::writeStoreFromTable()
{
    QVector<ModelParameter> entries;
    QSet<QString> seen;
    for (int row = 0; row < table_->rowCount(); ++row) {
        const QTableWidgetItem *name_item = table_->item(row, name_column);
        if (name_item == nullptr) {
            continue;
        }
        const QString name = name_item->text().trimmed();
        // Строки с непригодным или повторяющимся именем в модель не попадают:
        // выражения всё равно не смогли бы к ним обратиться однозначно.
        if (!ParameterStore::isValidName(name) || seen.contains(name)) {
            continue;
        }
        seen.insert(name);

        ModelParameter parameter;
        parameter.name = name;
        const QTableWidgetItem *expression_item = table_->item(row, expression_column);
        parameter.expression = expression_item != nullptr ? expression_item->text().trimmed()
                                                          : QString();
        const QTableWidgetItem *description_item = table_->item(row, description_column);
        parameter.description = description_item != nullptr ? description_item->text() : QString();
        entries.push_back(parameter);
    }
    store_->setEntries(entries);
}

void ParameterListWidget::refreshValueColumn()
{
    updating_ = true;
    for (int row = 0; row < table_->rowCount(); ++row) {
        QTableWidgetItem *value_item = table_->item(row, value_column);
        const QTableWidgetItem *name_item = table_->item(row, name_column);
        if (value_item == nullptr || name_item == nullptr) {
            continue;
        }

        double value = 0.0;
        QString error;
        if (store_->valueOf(name_item->text().trimmed(), &value, &error)) {
            value_item->setText(QLocale::system().toString(value, 'g', 8));
            value_item->setToolTip(QString());
            value_item->setForeground(QBrush());
        } else {
            value_item->setText(QStringLiteral("ошибка"));
            value_item->setToolTip(error);
            value_item->setForeground(QColor(180, 40, 40));
        }
    }
    updating_ = false;
}
