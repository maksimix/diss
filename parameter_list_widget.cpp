#include "parameter_list_widget.h"

#include "model_parameters.h"

#include <QtCore/QLocale>
#include <QtCore/QSet>
#include <QtGui/QBrush>
#include <QtGui/QColor>
#include <QtGui/QFontDatabase>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
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
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // Панель инструментов над таблицей: команды рядом с данными, а не под ними,
    // как в списке параметров CST. Справа — подсказка о синтаксисе выражений.
    QWidget *toolbar = new QWidget(this);
    toolbar->setObjectName(QStringLiteral("cstParameterToolbar"));
    QHBoxLayout *buttons = new QHBoxLayout(toolbar);
    buttons->setContentsMargins(6, 4, 8, 4);
    buttons->setSpacing(6);
    add_button_ = new QPushButton(QStringLiteral("Добавить"), toolbar);
    add_button_->setObjectName(QStringLiteral("cstParameterButton"));
    add_button_->setToolTip(QStringLiteral("Новая переменная модели"));
    delete_button_ = new QPushButton(QStringLiteral("Удалить"), toolbar);
    delete_button_->setObjectName(QStringLiteral("cstParameterButton"));
    delete_button_->setToolTip(QStringLiteral("Удалить выделенные строки"));
    delete_button_->setEnabled(false);
    buttons->addWidget(add_button_);
    buttons->addWidget(delete_button_);
    buttons->addStretch(1);
    QLabel *hint = new QLabel(
        QStringLiteral("Выражения: + − × ÷ ^, скобки, sqrt, sin, cos, min, max, pi; "
                       "имена других параметров подставляются по значению"),
        toolbar);
    hint->setObjectName(QStringLiteral("cstParameterHint"));
    buttons->addWidget(hint, 0);
    layout->addWidget(toolbar);

    table_ = new QTableWidget(0, 4, this);
    table_->setObjectName(QStringLiteral("cstParameterTable"));
    table_->setHorizontalHeaderLabels({QStringLiteral("Имя"),
                                       QStringLiteral("Выражение"),
                                       QStringLiteral("Значение"),
                                       QStringLiteral("Описание")});
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->horizontalHeader()->setSectionResizeMode(name_column, QHeaderView::Interactive);
    table_->horizontalHeader()->setHighlightSections(false);
    table_->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    table_->setColumnWidth(name_column, 150);
    table_->setColumnWidth(expression_column, 220);
    table_->setColumnWidth(value_column, 130);
    table_->verticalHeader()->setVisible(false);
    table_->verticalHeader()->setDefaultSectionSize(22);
    table_->setAlternatingRowColors(true);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setShowGrid(false);
    table_->setFrameShape(QFrame::NoFrame);
    // Правка начинается с первого щелчка по выделенной ячейке: список параметров
    // существует ради быстрого перебора значений.
    table_->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::SelectedClicked |
                            QAbstractItemView::EditKeyPressed | QAbstractItemView::AnyKeyPressed);
    layout->addWidget(table_, 1);

    // Пока переменных нет, таблица пустая и непонятная: подсказка объясняет,
    // зачем она нужна, и исчезает с первой строкой.
    empty_hint_ = new QLabel(
        QStringLiteral("Переменных пока нет. «Добавить» заводит именованное значение, которое\n"
                       "можно вписать в любое поле размера вместо числа — например width/2."),
        this);
    empty_hint_->setObjectName(QStringLiteral("cstParameterEmpty"));
    empty_hint_->setAlignment(Qt::AlignCenter);
    layout->addWidget(empty_hint_, 1);

    connect(add_button_, &QPushButton::clicked, this, &ParameterListWidget::addParameterRow);
    connect(delete_button_, &QPushButton::clicked, this, &ParameterListWidget::deleteSelectedRows);
    connect(table_, &QTableWidget::itemChanged, this, &ParameterListWidget::handleItemChanged);
    connect(table_, &QTableWidget::itemSelectionChanged, this, [this]() {
        delete_button_->setEnabled(!table_->selectedItems().isEmpty());
    });

    reload();
}

// Подсказка вместо пустой таблицы: показывается ровно одна из двух.
void ParameterListWidget::updateEmptyState()
{
    const bool empty = table_->rowCount() == 0;
    table_->setVisible(!empty);
    empty_hint_->setVisible(empty);
}

void ParameterListWidget::reload()
{
    updating_ = true;
    table_->setRowCount(0);
    for (const ModelParameter &parameter : store_->entries()) {
        const int row = table_->rowCount();
        table_->insertRow(row);
        QTableWidgetItem *name_item = new QTableWidgetItem(parameter.name);
        // Имя — ключ, по которому на переменную ссылаются выражения, поэтому
        // выделено начертанием.
        QFont name_font = name_item->font();
        name_font.setBold(true);
        name_item->setFont(name_font);
        table_->setItem(row, name_column, name_item);

        QTableWidgetItem *expression_item = new QTableWidgetItem(parameter.expression);
        expression_item->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        table_->setItem(row, expression_column, expression_item);

        QTableWidgetItem *value_item = new QTableWidgetItem();
        value_item->setFlags(value_item->flags() & ~Qt::ItemIsEditable);
        value_item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        value_item->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        table_->setItem(row, value_column, value_item);

        QTableWidgetItem *description_item = new QTableWidgetItem(parameter.description);
        description_item->setForeground(QColor(0x5a, 0x66, 0x70));
        table_->setItem(row, description_column, description_item);
    }
    updating_ = false;
    refreshValueColumn();
    updateEmptyState();
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
    updateEmptyState();
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
        QTableWidgetItem *expression_item = table_->item(row, expression_column);
        if (store_->valueOf(name_item->text().trimmed(), &value, &error)) {
            value_item->setText(QLocale::system().toString(value, 'g', 8));
            value_item->setToolTip(QStringLiteral("Значение подставляется всюду, где имя "
                                                  "вписано в поле размера"));
            value_item->setForeground(QColor(0x1c, 0x5f, 0x3a));
            if (expression_item != nullptr) {
                expression_item->setBackground(QBrush());
                expression_item->setToolTip(QString());
            }
        } else {
            // Ошибку показываем на самом выражении: чинить нужно именно его.
            value_item->setText(QStringLiteral("— ошибка —"));
            value_item->setToolTip(error);
            value_item->setForeground(QColor(0xb0, 0x20, 0x20));
            if (expression_item != nullptr) {
                expression_item->setBackground(QColor(0xff, 0xe6, 0xe6));
                expression_item->setToolTip(error);
            }
        }
    }
    updating_ = false;
}
