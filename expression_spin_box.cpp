#include "expression_spin_box.h"

#include "model_parameters.h"

ExpressionSpinBox::ExpressionSpinBox(const ParameterStore *store, QWidget *parent)
    : QDoubleSpinBox(parent)
    , store_(store)
{
    setToolTip(QStringLiteral("Можно ввести выражение с переменными модели, например «a/2 - 1»."));
}

QValidator::State ExpressionSpinBox::validate(QString &input, int &position) const
{
    const QValidator::State numeric = QDoubleSpinBox::validate(input, position);
    if (numeric == QValidator::Acceptable || store_ == nullptr) {
        return numeric;
    }

    QString body = input;
    body.remove(prefix());
    body.remove(suffix());
    body = body.trimmed();
    if (body.isEmpty()) {
        return QValidator::Intermediate;
    }

    double value = 0.0;
    if (store_->evaluate(body, &value) && value >= minimum() && value <= maximum()) {
        return QValidator::Acceptable;
    }
    // Незаконченная формула («a/» или «max(») ещё может стать верной, поэтому
    // ввод не отвергается — иначе поле не даст дописать выражение.
    return QValidator::Intermediate;
}

double ExpressionSpinBox::valueFromText(const QString &text) const
{
    QString body = text;
    body.remove(prefix());
    body.remove(suffix());
    body = body.trimmed();

    if (store_ != nullptr) {
        double value = 0.0;
        if (store_->evaluate(body, &value)) {
            return value;
        }
    }
    return QDoubleSpinBox::valueFromText(text);
}
