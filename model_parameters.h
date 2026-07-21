#pragma once

#include <QtCore/QSet>
#include <QtCore/QString>
#include <QtCore/QVector>

// Именованная переменная модели, как в списке параметров CST: имя, выражение и
// комментарий. Выражение может ссылаться на другие параметры по имени.
struct ModelParameter
{
    QString name;
    QString expression;
    QString description;
};

// Хранилище переменных модели с вычислителем выражений. Выражение поддерживает
// + - * / ^, скобки, функции (sqrt, sqr, abs, sin, cos, tan, exp, log, log10,
// min, max, pow, floor, ceil, round, rad, deg), константы pi и e, а также
// ссылки на другие параметры. Циклические ссылки распознаются и не приводят к
// зацикливанию.
//
// Запятая вне списка аргументов функции читается как десятичный разделитель
// («22,86» == 22.86), а внутри него всегда разделяет аргументы, поэтому дробные
// аргументы записываются через точку: max(1.5, 2), а не max(1,5, 2).
class ParameterStore
{
public:
    const QVector<ModelParameter> &entries() const { return entries_; }
    void setEntries(const QVector<ModelParameter> &entries) { entries_ = entries; }
    void clear() { entries_.clear(); }

    bool contains(const QString &name) const;
    // Добавляет параметр или обновляет существующий с тем же именем.
    void set(const QString &name, const QString &expression, const QString &description = QString());
    void remove(const QString &name);

    // Вычисляет произвольное выражение. Возвращает false и заполняет error,
    // если выражение синтаксически неверно или ссылается на неизвестное имя.
    bool evaluate(const QString &expression, double *value, QString *error = nullptr) const;
    // Вычисляет значение именованного параметра.
    bool valueOf(const QString &name, double *value, QString *error = nullptr) const;

    // Имя должно начинаться с буквы или подчёркивания и состоять из букв,
    // цифр и подчёркиваний — иначе его нельзя отличить от функции в выражении.
    static bool isValidName(const QString &name);

private:
    bool evaluateInternal(const QString &expression,
                          double *value,
                          QString *error,
                          QSet<QString> &visiting) const;

    QVector<ModelParameter> entries_;
};
