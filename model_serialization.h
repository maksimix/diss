#pragma once

#include "model_parameters.h"
#include "waveguide_types.h"

#include <QtCore/QString>

// Сохранение и загрузка модели волновода и кэша её расчёта.
//
// Модель (.wgm) — читаемый JSON: геометрия, возбуждение, объекты.
// Результаты (.wgr) — сжатый бинарный файл рядом с моделью: скалярные итоги,
// таблица мод, глифы поля и заливка срезов. В нём хранится копия модели, по
// которой он посчитан, поэтому устаревший кэш распознаётся и не подставляется.
namespace model_io
{
// Переменные модели (список параметров) хранятся в том же .wgm, но намеренно
// не входят в отпечаток геометрии: изменение комментария к переменной не должно
// обесценивать кэш расчёта. Передайте store == nullptr, если они не нужны.
bool saveModel(const QString &path,
               const WaveguideParameters &parameters,
               const ParameterStore *store,
               QString *error);
bool loadModel(const QString &path,
               WaveguideParameters *parameters,
               ParameterStore *store,
               QString *error);

// Путь к файлу расчёта рядом с моделью: <модель>.wgr
QString resultsPathFor(const QString &model_path);

// Путь к кэшу расчёта внутри проекта: <каталог модели>/Result/<база>.wgr.
// Проект — самодостаточная папка (модель + Result рядом), поэтому переносится
// на другой ПК целиком и открывается без пересчёта.
QString projectResultsPathFor(const QString &model_path);

// Создаёт каталог проекта и подпапку Result, если их ещё нет.
bool ensureProjectLayout(const QString &model_path, QString *error);

bool saveResults(const QString &path,
                 const WaveguideParameters &parameters,
                 const WaveguideCalculationResult &result,
                 QString *error);

// Загружает результаты, только если они посчитаны ровно для этой модели.
// Иначе возвращает false и сообщает причину в error.
bool loadResults(const QString &path,
                 const WaveguideParameters &parameters,
                 WaveguideCalculationResult *result,
                 QString *error);
}
