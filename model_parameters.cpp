#include "model_parameters.h"

#include <QtCore/QChar>

#include <cmath>
#include <functional>

namespace
{
using Resolver = std::function<bool(const QString &, double *, QString *)>;

// Рекурсивный спусковой разбор арифметического выражения. Разбор и вычисление
// идут одним проходом: дерево не строится, потому что выражения короткие и
// вычисляются заново при каждом изменении параметров.
class ExpressionParser
{
public:
    ExpressionParser(const QString &text, const Resolver &resolver)
        : text_(text)
        , resolver_(resolver)
    {
    }

    bool parse(double *value, QString *error)
    {
        skipSpaces();
        if (atEnd()) {
            setError(error, QStringLiteral("пустое выражение"));
            return false;
        }
        double result = 0.0;
        if (!parseExpression(&result, error)) {
            return false;
        }
        skipSpaces();
        if (!atEnd()) {
            setError(error,
                     QStringLiteral("лишние символы после выражения: «%1»")
                         .arg(text_.mid(position_)));
            return false;
        }
        if (!std::isfinite(result)) {
            setError(error, QStringLiteral("результат не является конечным числом"));
            return false;
        }
        *value = result;
        return true;
    }

private:
    bool atEnd() const { return position_ >= text_.size(); }
    QChar peek() const { return atEnd() ? QChar() : text_.at(position_); }

    void skipSpaces()
    {
        while (!atEnd() && text_.at(position_).isSpace()) {
            ++position_;
        }
    }

    bool consume(QChar character)
    {
        skipSpaces();
        if (!atEnd() && text_.at(position_) == character) {
            ++position_;
            return true;
        }
        return false;
    }

    static void setError(QString *error, const QString &message)
    {
        if (error != nullptr) {
            *error = message;
        }
    }

    bool parseExpression(double *value, QString *error)
    {
        double result = 0.0;
        if (!parseTerm(&result, error)) {
            return false;
        }
        for (;;) {
            skipSpaces();
            const QChar character = peek();
            if (character != QLatin1Char('+') && character != QLatin1Char('-')) {
                break;
            }
            ++position_;
            double operand = 0.0;
            if (!parseTerm(&operand, error)) {
                return false;
            }
            result = character == QLatin1Char('+') ? result + operand : result - operand;
        }
        *value = result;
        return true;
    }

    bool parseTerm(double *value, QString *error)
    {
        double result = 0.0;
        if (!parseUnary(&result, error)) {
            return false;
        }
        for (;;) {
            skipSpaces();
            const QChar character = peek();
            if (character != QLatin1Char('*') && character != QLatin1Char('/')) {
                break;
            }
            ++position_;
            double operand = 0.0;
            if (!parseUnary(&operand, error)) {
                return false;
            }
            if (character == QLatin1Char('/')) {
                if (operand == 0.0) {
                    setError(error, QStringLiteral("деление на ноль"));
                    return false;
                }
                result /= operand;
            } else {
                result *= operand;
            }
        }
        *value = result;
        return true;
    }

    bool parseUnary(double *value, QString *error)
    {
        skipSpaces();
        if (consume(QLatin1Char('-'))) {
            double operand = 0.0;
            if (!parseUnary(&operand, error)) {
                return false;
            }
            *value = -operand;
            return true;
        }
        if (consume(QLatin1Char('+'))) {
            return parseUnary(value, error);
        }
        return parsePower(value, error);
    }

    // Возведение в степень правоассоциативно: 2^3^2 == 2^9.
    bool parsePower(double *value, QString *error)
    {
        double base = 0.0;
        if (!parsePrimary(&base, error)) {
            return false;
        }
        skipSpaces();
        if (peek() == QLatin1Char('^')) {
            ++position_;
            double exponent = 0.0;
            if (!parseUnary(&exponent, error)) {
                return false;
            }
            base = std::pow(base, exponent);
        }
        *value = base;
        return true;
    }

    bool parsePrimary(double *value, QString *error)
    {
        skipSpaces();
        if (atEnd()) {
            setError(error, QStringLiteral("выражение обрывается"));
            return false;
        }

        if (consume(QLatin1Char('('))) {
            if (!parseExpression(value, error)) {
                return false;
            }
            if (!consume(QLatin1Char(')'))) {
                setError(error, QStringLiteral("нет закрывающей скобки"));
                return false;
            }
            return true;
        }

        const QChar character = peek();
        if (character.isDigit() || character == QLatin1Char('.') || character == QLatin1Char(',')) {
            return parseNumber(value, error);
        }
        if (character.isLetter() || character == QLatin1Char('_')) {
            return parseIdentifier(value, error);
        }

        setError(error, QStringLiteral("неожиданный символ «%1»").arg(character));
        return false;
    }

    bool parseNumber(double *value, QString *error)
    {
        const int start = position_;
        bool separator_seen = false;
        while (!atEnd()) {
            const QChar character = text_.at(position_);
            const bool exponent_sign =
                (character == QLatin1Char('+') || character == QLatin1Char('-')) &&
                position_ > start &&
                (text_.at(position_ - 1) == QLatin1Char('e') ||
                 text_.at(position_ - 1) == QLatin1Char('E'));
            if (character == QLatin1Char('.') || character == QLatin1Char(',')) {
                // Запятая двусмысленна: это и десятичный разделитель русской
                // локали, и разделитель аргументов функции. Внутри списка
                // аргументов она всегда разделяет аргументы, снаружи считается
                // десятичной точкой — но только если за ней идёт цифра и точки
                // в числе ещё не было.
                const bool comma = character == QLatin1Char(',');
                const bool next_is_digit = position_ + 1 < text_.size() &&
                                           text_.at(position_ + 1).isDigit();
                if (separator_seen || (comma && (function_depth_ > 0 || !next_is_digit))) {
                    break;
                }
                separator_seen = true;
                ++position_;
                continue;
            }
            if (character.isDigit() || character == QLatin1Char('e') ||
                character == QLatin1Char('E') || exponent_sign) {
                ++position_;
                continue;
            }
            break;
        }
        // Запятая принимается как десятичный разделитель: локаль ввода в полях
        // русская, и пользователь набирает «22,86» так же, как в спинбоксе.
        QString token = text_.mid(start, position_ - start);
        token.replace(QLatin1Char(','), QLatin1Char('.'));
        bool ok = false;
        const double parsed = token.toDouble(&ok);
        if (!ok) {
            setError(error, QStringLiteral("не число: «%1»").arg(token));
            return false;
        }
        *value = parsed;
        return true;
    }

    bool parseIdentifier(double *value, QString *error)
    {
        const int start = position_;
        while (!atEnd()) {
            const QChar character = text_.at(position_);
            if (character.isLetterOrNumber() || character == QLatin1Char('_')) {
                ++position_;
                continue;
            }
            break;
        }
        const QString name = text_.mid(start, position_ - start);

        skipSpaces();
        if (peek() == QLatin1Char('(')) {
            ++position_;
            QVector<double> arguments;
            ++function_depth_;
            if (!consume(QLatin1Char(')'))) {
                for (;;) {
                    double argument = 0.0;
                    if (!parseExpression(&argument, error)) {
                        --function_depth_;
                        return false;
                    }
                    arguments.push_back(argument);
                    if (consume(QLatin1Char(','))) {
                        continue;
                    }
                    if (consume(QLatin1Char(')'))) {
                        break;
                    }
                    --function_depth_;
                    setError(error,
                             QStringLiteral("нет закрывающей скобки у функции «%1»").arg(name));
                    return false;
                }
            }
            --function_depth_;
            return applyFunction(name, arguments, value, error);
        }

        const QString lowered = name.toLower();
        if (lowered == QLatin1String("pi")) {
            *value = 3.14159265358979323846;
            return true;
        }
        if (lowered == QLatin1String("e")) {
            *value = 2.71828182845904523536;
            return true;
        }
        if (resolver_ && resolver_(name, value, error)) {
            return true;
        }
        if (error == nullptr || error->isEmpty()) {
            setError(error, QStringLiteral("неизвестный параметр «%1»").arg(name));
        }
        return false;
    }

    static bool applyFunction(const QString &raw_name,
                              const QVector<double> &arguments,
                              double *value,
                              QString *error)
    {
        const QString name = raw_name.toLower();
        const auto unary = [&](double (*function)(double)) {
            if (arguments.size() != 1) {
                setError(error, QStringLiteral("функция «%1» ждёт один аргумент").arg(raw_name));
                return false;
            }
            *value = function(arguments.at(0));
            return true;
        };
        const auto binary = [&](double (*function)(double, double)) {
            if (arguments.size() != 2) {
                setError(error, QStringLiteral("функция «%1» ждёт два аргумента").arg(raw_name));
                return false;
            }
            *value = function(arguments.at(0), arguments.at(1));
            return true;
        };

        if (name == QLatin1String("sqrt")) {
            return unary([](double x) { return std::sqrt(x); });
        }
        if (name == QLatin1String("sqr")) {
            return unary([](double x) { return x * x; });
        }
        if (name == QLatin1String("abs")) {
            return unary([](double x) { return std::fabs(x); });
        }
        if (name == QLatin1String("sin")) {
            return unary([](double x) { return std::sin(x); });
        }
        if (name == QLatin1String("cos")) {
            return unary([](double x) { return std::cos(x); });
        }
        if (name == QLatin1String("tan")) {
            return unary([](double x) { return std::tan(x); });
        }
        if (name == QLatin1String("asin")) {
            return unary([](double x) { return std::asin(x); });
        }
        if (name == QLatin1String("acos")) {
            return unary([](double x) { return std::acos(x); });
        }
        if (name == QLatin1String("atan")) {
            return unary([](double x) { return std::atan(x); });
        }
        if (name == QLatin1String("exp")) {
            return unary([](double x) { return std::exp(x); });
        }
        if (name == QLatin1String("log")) {
            return unary([](double x) { return std::log(x); });
        }
        if (name == QLatin1String("log10")) {
            return unary([](double x) { return std::log10(x); });
        }
        if (name == QLatin1String("floor")) {
            return unary([](double x) { return std::floor(x); });
        }
        if (name == QLatin1String("ceil")) {
            return unary([](double x) { return std::ceil(x); });
        }
        if (name == QLatin1String("round")) {
            return unary([](double x) { return std::round(x); });
        }
        // Тригонометрия работает в радианах, поэтому rad()/deg() нужны для
        // выражений, записанных в градусах.
        if (name == QLatin1String("rad")) {
            return unary([](double x) { return x * 3.14159265358979323846 / 180.0; });
        }
        if (name == QLatin1String("deg")) {
            return unary([](double x) { return x * 180.0 / 3.14159265358979323846; });
        }
        if (name == QLatin1String("min")) {
            return binary([](double a, double b) { return a < b ? a : b; });
        }
        if (name == QLatin1String("max")) {
            return binary([](double a, double b) { return a > b ? a : b; });
        }
        if (name == QLatin1String("pow")) {
            return binary([](double a, double b) { return std::pow(a, b); });
        }
        if (name == QLatin1String("atan2")) {
            return binary([](double a, double b) { return std::atan2(a, b); });
        }

        setError(error, QStringLiteral("неизвестная функция «%1»").arg(raw_name));
        return false;
    }

    QString text_;
    Resolver resolver_;
    int position_ = 0;
    // Глубина вложенности в списки аргументов функций: внутри них запятая
    // всегда разделяет аргументы, а не дробную часть числа.
    int function_depth_ = 0;
};
}

bool ParameterStore::isValidName(const QString &name)
{
    if (name.isEmpty()) {
        return false;
    }
    const QChar first = name.at(0);
    if (!first.isLetter() && first != QLatin1Char('_')) {
        return false;
    }
    for (const QChar character : name) {
        if (!character.isLetterOrNumber() && character != QLatin1Char('_')) {
            return false;
        }
    }
    return true;
}

bool ParameterStore::contains(const QString &name) const
{
    for (const ModelParameter &entry : entries_) {
        if (entry.name == name) {
            return true;
        }
    }
    return false;
}

void ParameterStore::set(const QString &name, const QString &expression, const QString &description)
{
    for (ModelParameter &entry : entries_) {
        if (entry.name == name) {
            entry.expression = expression;
            entry.description = description;
            return;
        }
    }
    entries_.push_back(ModelParameter{name, expression, description});
}

void ParameterStore::remove(const QString &name)
{
    for (int index = 0; index < entries_.size(); ++index) {
        if (entries_.at(index).name == name) {
            entries_.remove(index);
            return;
        }
    }
}

bool ParameterStore::evaluate(const QString &expression, double *value, QString *error) const
{
    QSet<QString> visiting;
    return evaluateInternal(expression, value, error, visiting);
}

bool ParameterStore::valueOf(const QString &name, double *value, QString *error) const
{
    for (const ModelParameter &entry : entries_) {
        if (entry.name == name) {
            QSet<QString> visiting;
            visiting.insert(name);
            return evaluateInternal(entry.expression, value, error, visiting);
        }
    }
    if (error != nullptr) {
        *error = QStringLiteral("неизвестный параметр «%1»").arg(name);
    }
    return false;
}

bool ParameterStore::evaluateInternal(const QString &expression,
                                      double *value,
                                      QString *error,
                                      QSet<QString> &visiting) const
{
    const auto resolve = [this, &visiting](const QString &name, double *resolved, QString *inner) {
        for (const ModelParameter &entry : entries_) {
            if (entry.name != name) {
                continue;
            }
            if (visiting.contains(name)) {
                if (inner != nullptr) {
                    *inner = QStringLiteral("циклическая ссылка на «%1»").arg(name);
                }
                return false;
            }
            visiting.insert(name);
            const bool ok = evaluateInternal(entry.expression, resolved, inner, visiting);
            visiting.remove(name);
            return ok;
        }
        return false;
    };

    if (error != nullptr) {
        error->clear();
    }
    ExpressionParser parser(expression, resolve);
    return parser.parse(value, error);
}
