#include "CliRemoteController.h"

#include "Log.h"
#include "config/DspConfig.h"
#include "data/PresetManager.h"
#include "data/model/PresetListModel.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaMethod>
#include <QMetaProperty>
#include <optional>

CliRemoteController::CliRemoteController(QObject *parent)
    : QObject{parent}
{
    DspConfig::instance().load();
    const QMetaObject* meta = metaObject();
    for (int i = meta->propertyOffset(); i < meta->propertyCount(); ++i)
        _options.append(meta->property(i).read(this).value<QCommandLineOption*>());
}

CliRemoteController::~CliRemoteController()
{
    for (auto* opt : _options)
        delete opt;
}

void CliRemoteController::attachClient()
{
    if (service != nullptr)
        return;
#ifdef _WIN32
    service = new PipeIPCClient(this);
    service->connectToServer();
#else
    service = new me::timschneeberger::jdsp4linux::Service("me.timschneeberger.jdsp4linux", "/jdsp4linux/service",
                                                           QDBusConnection::sessionBus(), this);
#endif
}

void CliRemoteController::registerOptions(QCommandLineParser &parser) const
{
    for (auto* opt : _options)
        parser.addOption(*opt);
}

bool CliRemoteController::isAnyArgumentSet(QCommandLineParser &parser) const
{
    for (auto* opt : _options) {
        if (parser.isSet(*opt))
            return true;
    }
    return false;
}

bool CliRemoteController::execute(QCommandLineParser &parser)
{
    attachClient();
    const QMetaObject* meta = metaObject();
    for (int i = meta->methodOffset(); i < meta->methodCount(); ++i) {
        const QMetaMethod method = meta->method(i);
        const QByteArray propertyName = '_' + method.name();
        const int propertyIndex = meta->indexOfProperty(propertyName.constData());
        if (propertyIndex < 0) {
            Log::error(QString("Bug: Method '%1' has no corresponding property assigned").arg(QString::fromLatin1(method.name())));
            return false;
        }
        const QMetaProperty prop = meta->property(propertyIndex);
        const auto* opt = prop.read(this).value<QCommandLineOption*>();
        if (!parser.isSet(*opt))
            continue;

        bool result = false;
        if (method.parameterCount() == 0)
            return method.invoke(this, Q_RETURN_ARG(bool, result)) && result;
        if (method.parameterCount() == 1 && method.parameterType(0) == QMetaType::QString) {
            const QString value = parser.value(*opt);
            if (value.isEmpty()) {
                Log::error(QString("Option --%1 requires a nonempty value").arg(opt->names().first()));
                return false;
            }
            return method.invoke(this, Q_RETURN_ARG(bool, result), Q_ARG(QString, value)) && result;
        }
        Log::error(QString("Bug: Invokable method '%1' has wrong function signature for property '%2'")
                   .arg(QString::fromLatin1(method.name())).arg(prop.name()));
        return false;
    }
    return true;
}

namespace {
#ifdef _WIN32
std::optional<QString> handleReply(const QString& response)
{
    if (response == "OK")
        return QString();
    if (response.startsWith("OK|"))
        return response.mid(3);
    Log::error(response.startsWith("ERROR|") ? response.mid(6) : response);
    return std::nullopt;
}

std::optional<QJsonArray> handleArrayReply(const QString& response)
{
    const auto value = handleReply(response);
    if (!value)
        return std::nullopt;
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(value->toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !document.isArray()) {
        Log::error("Invalid JSON response from JamesDSP service");
        return std::nullopt;
    }
    return document.array();
}
#else
template<typename T>
std::optional<T> handleReply(QDBusPendingReply<T>& reply)
{
    reply.waitForFinished();
    if (reply.isError()) {
        Log::error(reply.error().message());
        return std::nullopt;
    }
    return reply.value();
}

bool handleVoidReply(QDBusPendingReply<>& reply)
{
    reply.waitForFinished();
    if (reply.isError())
        Log::error(reply.error().message());
    return !reply.isError();
}
#endif

bool validateKey(const QString& keyString, DspConfig::Key& resolvedKey)
{
    const QMetaEnum meta = QMetaEnum::fromType<DspConfig::Key>();
    bool valid = false;
    resolvedKey = static_cast<DspConfig::Key>(meta.keyToValue(keyString.toLocal8Bit().constData(), &valid));
    if (!valid)
        Log::error("Configuration key does not exist");
    return valid;
}

bool parseDeviceAndRoute(const QString& input, QString& deviceId, QString& routeId)
{
    const int separator = input.indexOf(':');
    if (!input.isEmpty() && (separator < 0 || (separator > 0 && separator < input.size() - 1 && input.indexOf(':', separator + 1) < 0))) {
        deviceId = separator < 0 ? input : input.left(separator);
        routeId = separator < 0 ? QString::fromStdString(RouteListModel::makeDefaultRoute().name) : input.mid(separator + 1);
        return true;
    }
    Log::error("Invalid device/route format. Expected: deviceId[:routeId]");
    return false;
}
}

bool CliRemoteController::isConnected() const
{
#ifdef _WIN32
    const bool valid = service && service->isConnected();
#else
    const bool valid = service && service->isValid();
#endif
    Log::console(valid ? "true" : "false", true);
    return valid;
}

bool CliRemoteController::set(const QString &keyValue) const
{
    QPair<QString, QVariant> parsed;
    if (!ConfigIO::readLine(keyValue, parsed)) {
        Log::error("Syntax error. Please use this format: --set key=value or --set \"key=value with spaces\".");
        return false;
    }
    if (!checkConnectionAndLog()) {
        DspConfig::Key key;
        if (!validateKey(parsed.first, key))
            return false;
        DspConfig::instance().set(key, parsed.second);
        DspConfig::instance().save();
        return true;
    }
#ifdef _WIN32
    QJsonArray value;
    value.append(QJsonValue::fromVariant(parsed.second));
    return handleReply(service->sendCommand("SET_AND_COMMIT", {parsed.first,
        QString::fromUtf8(QJsonDocument(value).toJson(QJsonDocument::Compact))})).has_value();
#else
    auto reply = service->setAndCommit(parsed.first, QDBusVariant(parsed.second));
    return handleVoidReply(reply);
#endif
}

bool CliRemoteController::get(const QString &key) const
{
    std::optional<QString> result;
    if (!checkConnectionAndLog()) {
        DspConfig::Key resolvedKey;
        if (!validateKey(key, resolvedKey))
            return false;
        result = DspConfig::instance().get<QString>(resolvedKey, nullptr, false);
    }
    else {
#ifdef _WIN32
        result = handleReply(service->sendCommand("GET", {key}));
#else
        auto reply = service->get(key);
        result = handleReply(reply);
#endif
    }
    if (result)
        Log::console(*result, true);
    return result.has_value();
}

bool CliRemoteController::getAll() const
{
    std::optional<QString> result;
    if (!checkConnectionAndLog())
        result = DspConfig::instance().serialize();
    else {
#ifdef _WIN32
        result = handleReply(service->sendCommand("GET_ALL"));
#else
        auto reply = service->getAll();
        result = handleReply(reply);
#endif
    }
    if (result)
        Log::console(*result, true);
    return result.has_value();
}

bool CliRemoteController::listKeys() const
{
    QStringList keys;
    if (checkConnectionAndLog()) {
#ifdef _WIN32
        const auto array = handleArrayReply(service->sendCommand("GET_KEYS"));
        if (!array)
            return false;
        for (const auto& key : *array)
            keys.append(key.toString());
#else
        auto reply = service->getKeys();
        const auto result = handleReply(reply);
        if (!result)
            return false;
        keys = *result;
#endif
    }
    else
        keys = DspConfig::getKeys();
    Log::console(keys.join('\n'), true);
    return !keys.empty();
}

bool CliRemoteController::listPresets() const
{
    QStringList presets;
    if (checkConnectionAndLog()) {
#ifdef _WIN32
        const auto array = handleArrayReply(service->sendCommand("GET_PRESETS"));
        if (!array)
            return false;
        for (const auto& preset : *array)
            presets.append(preset.toString());
#else
        auto reply = service->getPresets();
        const auto result = handleReply(reply);
        if (!result)
            return false;
        presets = *result;
#endif
    }
    else {
        auto* model = PresetManager::instance().presetModel();
        model->rescan();
        presets = model->getList();
    }
    for (const auto& preset : presets)
        Log::console(preset, true);
    return !presets.empty();
}

bool CliRemoteController::loadPreset(const QString &name) const
{
    if (checkConnectionAndLog()) {
#ifdef _WIN32
        return handleReply(service->sendCommand("LOAD_PRESET", {name})).has_value();
#else
        auto reply = service->loadPreset(name);
        return handleVoidReply(reply);
#endif
    }
    const bool success = PresetManager::instance().load(name);
    if (!success)
        Log::error("Preset does not exist");
    return success;
}

bool CliRemoteController::savePreset(const QString &name) const
{
    if (checkConnectionAndLog()) {
#ifdef _WIN32
        return handleReply(service->sendCommand("SAVE_PRESET", {name})).has_value();
#else
        auto reply = service->savePreset(name);
        return handleVoidReply(reply);
#endif
    }
    PresetManager::instance().save(name);
    return PresetManager::instance().exists(name);
}

bool CliRemoteController::deletePreset(const QString &name) const
{
    if (checkConnectionAndLog()) {
#ifdef _WIN32
        return handleReply(service->sendCommand("DELETE_PRESET", {name})).has_value();
#else
        auto reply = service->deletePreset(name);
        return handleVoidReply(reply);
#endif
    }
    const bool success = PresetManager::instance().remove(name);
    if (!success)
        Log::error("Preset does not exist");
    return success;
}

bool CliRemoteController::listPresetRules() const
{
#ifdef USE_PULSEAUDIO
    Log::error("This feature is only supported in the PipeWire version of JamesDSP");
    return false;
#endif
    QList<PresetRule> rules;
    if (checkConnectionAndLog()) {
#ifdef _WIN32
        const auto array = handleArrayReply(service->sendCommand("GET_PRESET_RULES"));
        if (!array)
            return false;
        for (const auto& rule : *array)
            rules.append(PresetRule(rule.toObject()));
#else
        auto reply = service->getPresetRules();
        const auto result = handleReply(reply);
        if (!result)
            return false;
        rules = *result;
#endif
    }
    else
        rules = PresetManager::instance().rules().toList();
    for (const auto& rule : rules)
        Log::console(rule.deviceId + ':' + rule.routeId + '=' + rule.preset, true);
    return !rules.empty();
}

bool CliRemoteController::addPresetRule(const QString &keyValue) const
{
#ifdef USE_PULSEAUDIO
    Log::error("This feature is only supported in the PipeWire version of JamesDSP");
    return false;
#endif
    const int separator = keyValue.indexOf('=');
    if (separator <= 0 || separator == keyValue.size() - 1) {
        Log::error("Invalid format. Expected input format: deviceId=presetName or deviceId:routeId=presetName");
        return false;
    }
    QString deviceId;
    QString routeId;
    if (!parseDeviceAndRoute(keyValue.left(separator), deviceId, routeId))
        return false;
    const QString preset = keyValue.mid(separator + 1);
    if (checkConnectionAndLog()) {
#ifdef _WIN32
        return handleReply(service->sendCommand("SET_PRESET_RULE", {deviceId, deviceId, routeId, routeId, preset})).has_value();
#else
        auto reply = service->setPresetRule(deviceId, deviceId, routeId, routeId, preset);
        return handleVoidReply(reply);
#endif
    }
    return PresetManager::instance().addRule(PresetRule(deviceId, routeId, preset));
}

bool CliRemoteController::deletePresetRule(const QString &deviceIdWithRoute) const
{
#ifdef USE_PULSEAUDIO
    Log::error("This feature is only supported in the PipeWire version of JamesDSP");
    return false;
#endif
    QString deviceId;
    QString routeId;
    if (!parseDeviceAndRoute(deviceIdWithRoute, deviceId, routeId))
        return false;
    if (checkConnectionAndLog()) {
#ifdef _WIN32
        return handleReply(service->sendCommand("DELETE_PRESET_RULE", {deviceId, routeId})).has_value();
#else
        auto reply = service->deletePresetRule(deviceId, routeId);
        return handleVoidReply(reply);
#endif
    }
    PresetManager::instance().removeRule(deviceId, routeId);
    return true;
}

bool CliRemoteController::listOutputDevices() const
{
    if (!requireConnectionAndLog())
        return false;
    QList<IOutputDevice> devices;
#ifdef _WIN32
    const auto array = handleArrayReply(service->sendCommand("GET_OUTPUT_DEVICES"));
    if (!array)
        return false;
    for (const auto& value : *array) {
        const auto object = value.toObject();
        IOutputDevice device;
        device.id = static_cast<uint32_t>(object.value("id").toDouble());
        device.name = object.value("name").toString().toStdString();
        device.description = object.value("description").toString().toStdString();
        device.output_route_name = object.value("output_route_name").toString().toStdString();
        device.output_route_description = object.value("output_route_description").toString().toStdString();
        for (const auto& route : object.value("output_routes").toArray()) {
            const auto record = route.toObject();
            device.output_routes.append(Route(record.value("name").toString().toStdString(), record.value("description").toString().toStdString()));
        }
        devices.append(device);
    }
#else
    auto reply = service->getOutputDevices();
    const auto result = handleReply(reply);
    if (!result)
        return false;
    devices = *result;
#endif
    for (const auto& device : devices) {
        Log::console(QString("Name: %1").arg(QString::fromStdString(device.description)), true);
        Log::console(QString("ID: %1").arg(QString::fromStdString(device.name)), true);
        Log::console(QString("Current output route ID: %2 (%1)").arg(QString::fromStdString(device.output_route_description)).arg(QString::fromStdString(device.output_route_name)), true);
        Log::console("Available output route IDs:", true);
        for (const auto& route : device.output_routes)
            Log::console(QString("\t%2 (%1)").arg(QString::fromStdString(route.description)).arg(QString::fromStdString(route.name)), true);
        Log::console("", true);
    }
    return true;
}

bool CliRemoteController::showStatus() const
{
    if (!requireConnectionAndLog())
        return false;
#ifdef _WIN32
    const auto appVersion = handleReply(service->sendCommand("APP_VERSION"));
    const auto coreVersion = handleReply(service->sendCommand("CORE_VERSION"));
    const auto samplingRate = handleReply(service->sendCommand("SAMPLING_RATE"));
    const auto audioFormat = handleReply(service->sendCommand("AUDIO_FORMAT"));
    const auto processing = handleReply(service->sendCommand("IS_PROCESSING"));
    if (!appVersion || !coreVersion || !samplingRate || !audioFormat || !processing)
        return false;
    Log::console("App version:\t" + *appVersion, true);
    Log::console("Core version:\t" + *coreVersion, true);
    Log::console("Sampling rate:\t" + *samplingRate + "Hz", true);
    Log::console("Audio format:\t" + *audioFormat, true);
    Log::console("Is processing:\t" + QString(*processing == "true" ? "enabled" : "disabled"), true);
#else
    Log::console("App version:\t" + service->appVersion(), true);
    Log::console("Core version:\t" + service->coreVersion(), true);
    Log::console("Sampling rate:\t" + service->samplingRate() + "Hz", true);
    Log::console("Audio format:\t" + service->audioFormat(), true);
    Log::console("Is processing:\t" + QString(service->isProcessing() ? "enabled" : "disabled"), true);
#endif
    return true;
}

bool CliRemoteController::relinkAudioPipeline() const
{
    if (!requireConnectionAndLog())
        return false;
#ifdef _WIN32
    return handleReply(service->sendCommand("RELINK")).has_value();
#else
    auto reply = service->relinkAudioPipeline();
    return handleVoidReply(reply);
#endif
}

bool CliRemoteController::checkConnectionAndLog() const
{
#ifdef _WIN32
    const bool valid = service && service->isConnected();
#else
    const bool valid = service && service->isValid();
#endif
    if (!valid)
        Log::warning("JamesDSP service is offline; falling back to direct file access");
    return valid;
}

bool CliRemoteController::requireConnectionAndLog() const
{
#ifdef _WIN32
    const bool valid = service && service->isConnected();
#else
    const bool valid = service && service->isValid();
#endif
    if (!valid)
        Log::error("JamesDSP service is offline. Please start it first and try again.");
    return valid;
}
