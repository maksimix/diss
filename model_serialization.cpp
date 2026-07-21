#include "model_serialization.h"

#include <QtCore/QDataStream>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

namespace
{
constexpr quint32 results_magic = 0x57475231;   // "WGR1"
constexpr quint32 results_version = 1;
constexpr auto stream_version = QDataStream::Qt_6_0;

QJsonObject plateToJson(const PecPlateParameters &plate)
{
    QJsonObject object;
    object[QStringLiteral("name")] = plate.name;
    object[QStringLiteral("enabled")] = plate.enabled;
    object[QStringLiteral("x_min_mm")] = plate.x_min_mm;
    object[QStringLiteral("x_max_mm")] = plate.x_max_mm;
    object[QStringLiteral("y_min_mm")] = plate.y_min_mm;
    object[QStringLiteral("y_max_mm")] = plate.y_max_mm;
    object[QStringLiteral("z_min_mm")] = plate.z_min_mm;
    object[QStringLiteral("z_max_mm")] = plate.z_max_mm;
    object[QStringLiteral("rotation_x_deg")] = plate.rotation_x_deg;
    object[QStringLiteral("rotation_y_deg")] = plate.rotation_y_deg;
    object[QStringLiteral("rotation_z_deg")] = plate.rotation_z_deg;
    object[QStringLiteral("aperture_enabled")] = plate.aperture_enabled;
    object[QStringLiteral("aperture_shape")] = plate.aperture_shape;
    object[QStringLiteral("aperture_width_mm")] = plate.aperture_width_mm;
    object[QStringLiteral("aperture_height_mm")] = plate.aperture_height_mm;
    object[QStringLiteral("aperture_radius_mm")] = plate.aperture_radius_mm;
    object[QStringLiteral("aperture_offset_x_mm")] = plate.aperture_offset_x_mm;
    object[QStringLiteral("aperture_offset_y_mm")] = plate.aperture_offset_y_mm;
    object[QStringLiteral("post_enabled")] = plate.post_enabled;
    object[QStringLiteral("post_width_mm")] = plate.post_width_mm;
    object[QStringLiteral("post_height_mm")] = plate.post_height_mm;
    return object;
}

PecPlateParameters plateFromJson(const QJsonObject &object)
{
    PecPlateParameters plate;
    plate.name = object.value(QStringLiteral("name")).toString(plate.name);
    plate.enabled = object.value(QStringLiteral("enabled")).toBool(plate.enabled);
    plate.x_min_mm = object.value(QStringLiteral("x_min_mm")).toDouble(plate.x_min_mm);
    plate.x_max_mm = object.value(QStringLiteral("x_max_mm")).toDouble(plate.x_max_mm);
    plate.y_min_mm = object.value(QStringLiteral("y_min_mm")).toDouble(plate.y_min_mm);
    plate.y_max_mm = object.value(QStringLiteral("y_max_mm")).toDouble(plate.y_max_mm);
    plate.z_min_mm = object.value(QStringLiteral("z_min_mm")).toDouble(plate.z_min_mm);
    plate.z_max_mm = object.value(QStringLiteral("z_max_mm")).toDouble(plate.z_max_mm);
    plate.rotation_x_deg =
        object.value(QStringLiteral("rotation_x_deg")).toDouble(plate.rotation_x_deg);
    plate.rotation_y_deg =
        object.value(QStringLiteral("rotation_y_deg")).toDouble(plate.rotation_y_deg);
    plate.rotation_z_deg =
        object.value(QStringLiteral("rotation_z_deg")).toDouble(plate.rotation_z_deg);
    plate.aperture_enabled =
        object.value(QStringLiteral("aperture_enabled")).toBool(plate.aperture_enabled);
    plate.aperture_shape =
        object.value(QStringLiteral("aperture_shape")).toInt(plate.aperture_shape);
    plate.aperture_width_mm =
        object.value(QStringLiteral("aperture_width_mm")).toDouble(plate.aperture_width_mm);
    plate.aperture_height_mm =
        object.value(QStringLiteral("aperture_height_mm")).toDouble(plate.aperture_height_mm);
    plate.aperture_radius_mm =
        object.value(QStringLiteral("aperture_radius_mm")).toDouble(plate.aperture_radius_mm);
    plate.aperture_offset_x_mm =
        object.value(QStringLiteral("aperture_offset_x_mm")).toDouble(plate.aperture_offset_x_mm);
    plate.aperture_offset_y_mm =
        object.value(QStringLiteral("aperture_offset_y_mm")).toDouble(plate.aperture_offset_y_mm);
    plate.post_enabled = object.value(QStringLiteral("post_enabled")).toBool(plate.post_enabled);
    plate.post_width_mm =
        object.value(QStringLiteral("post_width_mm")).toDouble(plate.post_width_mm);
    plate.post_height_mm =
        object.value(QStringLiteral("post_height_mm")).toDouble(plate.post_height_mm);
    return plate;
}

QJsonObject parametersToJson(const WaveguideParameters &parameters)
{
    QJsonObject waveguide;
    waveguide[QStringLiteral("cross_section")] = parameters.cross_section;
    waveguide[QStringLiteral("radius_mm")] = parameters.radius_mm;
    waveguide[QStringLiteral("width_mm")] = parameters.width_mm;
    waveguide[QStringLiteral("depth_mm")] = parameters.depth_mm;
    waveguide[QStringLiteral("length_mm")] = parameters.length_mm;
    waveguide[QStringLiteral("wall_thickness_mm")] = parameters.wall_thickness_mm;
    waveguide[QStringLiteral("wall_conductivity_s_per_m")] = parameters.wall_conductivity_s_per_m;

    QJsonObject excitation;
    excitation[QStringLiteral("frequency_ghz")] = parameters.frequency_ghz;

    QJsonObject slot;
    slot[QStringLiteral("enabled")] = parameters.slot_enabled;
    slot[QStringLiteral("length_mm")] = parameters.slot_length_mm;
    slot[QStringLiteral("width_mm")] = parameters.slot_width_mm;
    slot[QStringLiteral("offset_x_mm")] = parameters.slot_offset_x_mm;
    slot[QStringLiteral("offset_z_mm")] = parameters.slot_offset_z_mm;
    slot[QStringLiteral("rotation_deg")] = parameters.slot_rotation_deg;
    slot[QStringLiteral("surface")] = parameters.slot_surface;

    QJsonArray plates;
    for (const PecPlateParameters &plate : parameters.pec_plates) {
        plates.append(plateToJson(plate));
    }

    QJsonObject root;
    root[QStringLiteral("format")] = QStringLiteral("krutiev-waveguide-model");
    root[QStringLiteral("version")] = 1;
    root[QStringLiteral("waveguide")] = waveguide;
    root[QStringLiteral("excitation")] = excitation;
    root[QStringLiteral("slot")] = slot;
    root[QStringLiteral("plates")] = plates;
    root[QStringLiteral("accuracy_level")] = parameters.accuracy_level;
    root[QStringLiteral("solver_method")] = parameters.solver_method;
    return root;
}

WaveguideParameters parametersFromJson(const QJsonObject &root)
{
    WaveguideParameters parameters;
    const QJsonObject waveguide = root.value(QStringLiteral("waveguide")).toObject();
    parameters.cross_section =
        waveguide.value(QStringLiteral("cross_section")).toInt(parameters.cross_section);
    parameters.radius_mm =
        waveguide.value(QStringLiteral("radius_mm")).toDouble(parameters.radius_mm);
    parameters.width_mm = waveguide.value(QStringLiteral("width_mm")).toDouble(parameters.width_mm);
    parameters.depth_mm = waveguide.value(QStringLiteral("depth_mm")).toDouble(parameters.depth_mm);
    parameters.length_mm =
        waveguide.value(QStringLiteral("length_mm")).toDouble(parameters.length_mm);
    parameters.wall_thickness_mm =
        waveguide.value(QStringLiteral("wall_thickness_mm")).toDouble(parameters.wall_thickness_mm);
    parameters.wall_conductivity_s_per_m =
        waveguide.value(QStringLiteral("wall_conductivity_s_per_m"))
            .toDouble(parameters.wall_conductivity_s_per_m);

    const QJsonObject excitation = root.value(QStringLiteral("excitation")).toObject();
    parameters.frequency_ghz =
        excitation.value(QStringLiteral("frequency_ghz")).toDouble(parameters.frequency_ghz);

    const QJsonObject slot = root.value(QStringLiteral("slot")).toObject();
    parameters.slot_enabled = slot.value(QStringLiteral("enabled")).toBool(parameters.slot_enabled);
    parameters.slot_length_mm =
        slot.value(QStringLiteral("length_mm")).toDouble(parameters.slot_length_mm);
    parameters.slot_width_mm =
        slot.value(QStringLiteral("width_mm")).toDouble(parameters.slot_width_mm);
    parameters.slot_offset_x_mm =
        slot.value(QStringLiteral("offset_x_mm")).toDouble(parameters.slot_offset_x_mm);
    parameters.slot_offset_z_mm =
        slot.value(QStringLiteral("offset_z_mm")).toDouble(parameters.slot_offset_z_mm);
    parameters.slot_rotation_deg =
        slot.value(QStringLiteral("rotation_deg")).toDouble(parameters.slot_rotation_deg);
    parameters.slot_surface = slot.value(QStringLiteral("surface")).toInt(parameters.slot_surface);

    parameters.accuracy_level =
        root.value(QStringLiteral("accuracy_level")).toInt(parameters.accuracy_level);
    parameters.solver_method =
        root.value(QStringLiteral("solver_method")).toInt(parameters.solver_method);

    parameters.pec_plates.clear();
    const QJsonArray plates = root.value(QStringLiteral("plates")).toArray();
    for (const QJsonValue &value : plates) {
        parameters.pec_plates.push_back(plateFromJson(value.toObject()));
    }
    return parameters;
}

QByteArray modelBytes(const WaveguideParameters &parameters)
{
    return QJsonDocument(parametersToJson(parameters)).toJson(QJsonDocument::Compact);
}
}

// ------------------------------------------------------------- потоки ------
QDataStream &operator<<(QDataStream &stream, const WaveguideMode &mode)
{
    stream << mode.name << mode.transverse_electric << mode.m << mode.n << mode.cutoff_ghz
           << mode.propagates;
    return stream;
}

QDataStream &operator>>(QDataStream &stream, WaveguideMode &mode)
{
    stream >> mode.name >> mode.transverse_electric >> mode.m >> mode.n >> mode.cutoff_ghz >>
        mode.propagates;
    return stream;
}

QDataStream &operator<<(QDataStream &stream, const FieldGlyph &glyph)
{
    stream << static_cast<qint32>(glyph.type) << glyph.points << glyph.color << glyph.magnitude
           << glyph.animated << glyph.anchor << glyph.phasor_real << glyph.phasor_imag
           << glyph.reference_magnitude << glyph.animation_length_mm;
    return stream;
}

QDataStream &operator>>(QDataStream &stream, FieldGlyph &glyph)
{
    qint32 type = 0;
    stream >> type;
    glyph.type = static_cast<FieldGlyphType>(type);
    stream >> glyph.points >> glyph.color >> glyph.magnitude >> glyph.animated >> glyph.anchor >>
        glyph.phasor_real >> glyph.phasor_imag >> glyph.reference_magnitude >>
        glyph.animation_length_mm;
    return stream;
}

QDataStream &operator<<(QDataStream &stream, const FieldSliceCell &cell)
{
    stream << cell.center << cell.u_half << cell.v_half << cell.envelope << cell.phasor_real
           << cell.phasor_imag;
    return stream;
}

QDataStream &operator>>(QDataStream &stream, FieldSliceCell &cell)
{
    stream >> cell.center >> cell.u_half >> cell.v_half >> cell.envelope >> cell.phasor_real >>
        cell.phasor_imag;
    return stream;
}

QDataStream &operator<<(QDataStream &stream, const FieldSlice &slice)
{
    stream << slice.valid << static_cast<qint32>(slice.plane) << slice.cells
           << slice.maximum_value << slice.value_label;
    return stream;
}

QDataStream &operator>>(QDataStream &stream, FieldSlice &slice)
{
    qint32 plane = 0;
    stream >> slice.valid >> plane;
    slice.plane = static_cast<FieldSlicePlane>(plane);
    stream >> slice.cells >> slice.maximum_value >> slice.value_label;
    return stream;
}

namespace model_io
{
bool saveModel(const QString &path,
               const WaveguideParameters &parameters,
               const ParameterStore *store,
               QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) {
            *error = QStringLiteral("Не удалось открыть файл для записи: %1").arg(file.errorString());
        }
        return false;
    }
    QJsonObject root = parametersToJson(parameters);
    if (store != nullptr && !store->entries().isEmpty()) {
        QJsonArray variables;
        for (const ModelParameter &parameter : store->entries()) {
            QJsonObject variable;
            variable[QStringLiteral("name")] = parameter.name;
            variable[QStringLiteral("expression")] = parameter.expression;
            variable[QStringLiteral("description")] = parameter.description;
            variables.append(variable);
        }
        root[QStringLiteral("variables")] = variables;
    }
    const QByteArray payload = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (file.write(payload) != payload.size()) {
        if (error) {
            *error = QStringLiteral("Ошибка записи модели: %1").arg(file.errorString());
        }
        return false;
    }
    return true;
}

bool loadModel(const QString &path,
               WaveguideParameters *parameters,
               ParameterStore *store,
               QString *error)
{
    if (parameters == nullptr) {
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = QStringLiteral("Не удалось открыть файл: %1").arg(file.errorString());
        }
        return false;
    }
    QJsonParseError parse_error{};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) {
            *error = QStringLiteral("Файл не является моделью: %1").arg(parse_error.errorString());
        }
        return false;
    }
    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("format")).toString() !=
        QStringLiteral("krutiev-waveguide-model")) {
        if (error) {
            *error = QStringLiteral("Неизвестный формат файла модели.");
        }
        return false;
    }
    *parameters = parametersFromJson(root);
    if (store != nullptr) {
        QVector<ModelParameter> variables;
        for (const QJsonValue &value : root.value(QStringLiteral("variables")).toArray()) {
            const QJsonObject variable = value.toObject();
            ModelParameter parameter;
            parameter.name = variable.value(QStringLiteral("name")).toString();
            parameter.expression = variable.value(QStringLiteral("expression")).toString();
            parameter.description = variable.value(QStringLiteral("description")).toString();
            if (ParameterStore::isValidName(parameter.name)) {
                variables.push_back(parameter);
            }
        }
        store->setEntries(variables);
    }
    return true;
}

QString resultsPathFor(const QString &model_path)
{
    const QFileInfo info(model_path);
    return info.dir().filePath(info.completeBaseName() + QStringLiteral(".wgr"));
}

bool saveResults(const QString &path,
                 const WaveguideParameters &parameters,
                 const WaveguideCalculationResult &result,
                 QString *error)
{
    QByteArray payload;
    {
        QDataStream stream(&payload, QIODevice::WriteOnly);
        stream.setVersion(stream_version);
        stream << result.valid << result.inner_width_mm << result.inner_depth_mm
               << result.area_mm2 << result.cavity_volume_mm3 << result.metal_volume_mm3
               << result.modes << result.selected_mode << result.has_propagating_mode
               << result.wavelength0_mm << result.guide_wavelength_mm << result.beta_rad_per_m
               << result.attenuation_np_per_m << result.conductor_attenuation_np_per_m
               << result.stored_electric_energy_j << result.stored_magnetic_energy_j
               << result.quality_factor << result.slot_normalized_coupling
               << result.incident_power_w << result.reflected_power_w
               << result.transmitted_power_w << result.dissipated_power_w << result.input_power_w
               << result.output_power_w << result.s11_magnitude << result.s21_magnitude
               << result.power_balance_relative_error << result.solver_backend
               << result.solver_warnings << result.field_glyphs << result.horizontal_slice
               << result.vertical_slice;
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) {
            *error = QStringLiteral("Не удалось сохранить расчёт: %1").arg(file.errorString());
        }
        return false;
    }
    QDataStream stream(&file);
    stream.setVersion(stream_version);
    stream << results_magic << results_version << modelBytes(parameters)
           << qCompress(payload, 6);
    if (stream.status() != QDataStream::Ok) {
        if (error) {
            *error = QStringLiteral("Ошибка записи расчёта.");
        }
        return false;
    }
    return true;
}

bool loadResults(const QString &path,
                 const WaveguideParameters &parameters,
                 WaveguideCalculationResult *result,
                 QString *error)
{
    if (result == nullptr) {
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = QStringLiteral("Файл расчёта не открывается: %1").arg(file.errorString());
        }
        return false;
    }
    QDataStream stream(&file);
    stream.setVersion(stream_version);
    quint32 magic = 0;
    quint32 version = 0;
    QByteArray stored_model;
    QByteArray compressed;
    stream >> magic >> version >> stored_model >> compressed;
    if (stream.status() != QDataStream::Ok || magic != results_magic) {
        if (error) {
            *error = QStringLiteral("Файл расчёта повреждён или имеет чужой формат.");
        }
        return false;
    }
    if (version != results_version) {
        if (error) {
            *error = QStringLiteral("Файл расчёта другой версии — нужен пересчёт.");
        }
        return false;
    }
    if (stored_model != modelBytes(parameters)) {
        if (error) {
            *error = QStringLiteral("Расчёт посчитан для другой геометрии — нужен пересчёт.");
        }
        return false;
    }

    const QByteArray payload = qUncompress(compressed);
    if (payload.isEmpty()) {
        if (error) {
            *error = QStringLiteral("Не удалось распаковать данные расчёта.");
        }
        return false;
    }

    WaveguideCalculationResult loaded;
    loaded.parameters = parameters;
    QDataStream payload_stream(payload);
    payload_stream.setVersion(stream_version);
    payload_stream >> loaded.valid >> loaded.inner_width_mm >> loaded.inner_depth_mm >>
        loaded.area_mm2 >> loaded.cavity_volume_mm3 >> loaded.metal_volume_mm3 >> loaded.modes >>
        loaded.selected_mode >> loaded.has_propagating_mode >> loaded.wavelength0_mm >>
        loaded.guide_wavelength_mm >> loaded.beta_rad_per_m >> loaded.attenuation_np_per_m >>
        loaded.conductor_attenuation_np_per_m >> loaded.stored_electric_energy_j >>
        loaded.stored_magnetic_energy_j >> loaded.quality_factor >>
        loaded.slot_normalized_coupling >> loaded.incident_power_w >> loaded.reflected_power_w >>
        loaded.transmitted_power_w >> loaded.dissipated_power_w >> loaded.input_power_w >>
        loaded.output_power_w >> loaded.s11_magnitude >> loaded.s21_magnitude >>
        loaded.power_balance_relative_error >> loaded.solver_backend >> loaded.solver_warnings >>
        loaded.field_glyphs >> loaded.horizontal_slice >> loaded.vertical_slice;
    if (payload_stream.status() != QDataStream::Ok) {
        if (error) {
            *error = QStringLiteral("Данные расчёта неполные.");
        }
        return false;
    }
    *result = loaded;
    return true;
}
}
