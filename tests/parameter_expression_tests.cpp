// Проверки вычислителя выражений списка параметров: приоритет операций,
// функции, ссылки между переменными и распознавание циклов.
#include "model_parameters.h"

#include <QtCore/QString>

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace
{
int failures = 0;

void expectValue(const ParameterStore &store, const QString &expression, double expected)
{
    double value = 0.0;
    QString error;
    if (!store.evaluate(expression, &value, &error)) {
        std::printf("FAIL: «%s» не вычислилось: %s\n",
                    expression.toUtf8().constData(),
                    error.toUtf8().constData());
        ++failures;
        return;
    }
    if (std::fabs(value - expected) > 1.0e-9 * std::max(1.0, std::fabs(expected))) {
        std::printf("FAIL: «%s» = %.12g, ожидалось %.12g\n",
                    expression.toUtf8().constData(),
                    value,
                    expected);
        ++failures;
    }
}

void expectFailure(const ParameterStore &store, const QString &expression)
{
    double value = 0.0;
    QString error;
    if (store.evaluate(expression, &value, &error)) {
        std::printf("FAIL: «%s» должно было быть отвергнуто, получено %.12g\n",
                    expression.toUtf8().constData(),
                    value);
        ++failures;
    }
}

void expectTrue(bool condition, const char *what)
{
    if (!condition) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}
}

int main()
{
    ParameterStore store;

    // Арифметика и приоритет операций.
    expectValue(store, QStringLiteral("2 + 3 * 4"), 14.0);
    expectValue(store, QStringLiteral("(2 + 3) * 4"), 20.0);
    expectValue(store, QStringLiteral("-3 + 1"), -2.0);
    expectValue(store, QStringLiteral("10 / 4"), 2.5);
    // Возведение в степень правоассоциативно.
    expectValue(store, QStringLiteral("2^3^2"), 512.0);
    // Запятая как десятичный разделитель — так набирают в русской локали.
    expectValue(store, QStringLiteral("22,86 / 2"), 11.43);
    expectValue(store, QStringLiteral("1.5e2"), 150.0);

    // Функции и константы.
    expectValue(store, QStringLiteral("max(3, 22.86/2) + sqrt(4)"), 13.43);
    expectValue(store, QStringLiteral("sqr(3)"), 9.0);
    expectValue(store, QStringLiteral("abs(0 - 7)"), 7.0);
    expectValue(store, QStringLiteral("cos(0)"), 1.0);
    expectValue(store, QStringLiteral("sin(rad(90))"), 1.0);
    expectValue(store, QStringLiteral("deg(pi)"), 180.0);
    expectValue(store, QStringLiteral("min(2, 5)"), 2.0);
    // Запятая двусмысленна. Внутри списка аргументов она всегда разделяет
    // аргументы — «min(2,5)» это два аргумента, а не число 2,5. Снаружи она
    // остаётся десятичным разделителем русской локали.
    expectValue(store, QStringLiteral("min(2,5)"), 2.0);
    expectValue(store, QStringLiteral("2,5 * 2"), 5.0);
    expectValue(store, QStringLiteral("pow(2, 10) / min(2,5)"), 512.0);
    // Следствие правила: дробь внутри аргумента пишется только через точку.
    expectValue(store, QStringLiteral("max(1.5, 2)"), 2.0);
    expectFailure(store, QStringLiteral("max(1,5, 2)"));

    // Ссылки между переменными.
    store.set(QStringLiteral("a"), QStringLiteral("22.86"));
    store.set(QStringLiteral("b"), QStringLiteral("a / 2"));
    store.set(QStringLiteral("gap"), QStringLiteral("b - 1"));
    expectValue(store, QStringLiteral("gap"), 10.43);
    expectValue(store, QStringLiteral("a/2 + 1"), 12.43);

    double value = 0.0;
    expectTrue(store.valueOf(QStringLiteral("b"), &value) && std::fabs(value - 11.43) < 1.0e-9,
               "valueOf(b) должно дать 11.43");

    // Ошибки: незакрытая скобка, неизвестное имя, деление на ноль, мусор.
    expectFailure(store, QStringLiteral("2 * (3 + 1"));
    expectFailure(store, QStringLiteral("unknown_name + 1"));
    expectFailure(store, QStringLiteral("1 / 0"));
    expectFailure(store, QStringLiteral("2 3"));
    expectFailure(store, QStringLiteral(""));
    expectFailure(store, QStringLiteral("nosuchfunc(2)"));

    // Цикл должен распознаваться, а не уводить разбор в бесконечность.
    store.set(QStringLiteral("loop_a"), QStringLiteral("loop_b + 1"));
    store.set(QStringLiteral("loop_b"), QStringLiteral("loop_a + 1"));
    expectFailure(store, QStringLiteral("loop_a"));

    // Проверка имён.
    expectTrue(ParameterStore::isValidName(QStringLiteral("width_mm")), "width_mm — верное имя");
    expectTrue(ParameterStore::isValidName(QStringLiteral("_a1")), "_a1 — верное имя");
    expectTrue(!ParameterStore::isValidName(QStringLiteral("1a")), "1a — неверное имя");
    expectTrue(!ParameterStore::isValidName(QStringLiteral("a-b")), "a-b — неверное имя");
    expectTrue(!ParameterStore::isValidName(QString()), "пустое имя неверно");

    // Удаление и повторная установка.
    store.remove(QStringLiteral("gap"));
    expectTrue(!store.contains(QStringLiteral("gap")), "gap удалён");
    expectFailure(store, QStringLiteral("gap"));

    if (failures != 0) {
        std::printf("Провалено проверок: %d\n", failures);
        return EXIT_FAILURE;
    }
    std::printf("All parameter expression tests passed.\n");
    return EXIT_SUCCESS;
}
