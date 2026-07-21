#pragma once

#include <QtWidgets/QDoubleSpinBox>

class ParameterStore;

// Спинбокс, принимающий не только число, но и выражение с переменными модели —
// как поля размеров в CST, куда можно вписать «a/2 + gap». Введённое выражение
// вычисляется хранилищем параметров; в поле остаётся вычисленное число, потому
// что модель хранит размеры числами, а не формулами.
class ExpressionSpinBox : public QDoubleSpinBox
{
    Q_OBJECT

public:
    explicit ExpressionSpinBox(const ParameterStore *store, QWidget *parent = nullptr);

    QValidator::State validate(QString &input, int &position) const override;
    double valueFromText(const QString &text) const override;

private:
    const ParameterStore *store_ = nullptr;
};
