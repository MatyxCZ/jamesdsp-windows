#include "CliRemoteController.h"

#include "Log.h"
#include "config/DspConfig.h"
#include "data/PresetManager.h"
#include "data/model/PresetListModel.h"

CliRemoteController::CliRemoteController(QObject *parent)
    : QObject{parent}
{
#ifdef _WIN32
    service = new PipeIPCClient(this);
#else
    service = new me::timschneeberger::jdsp4linux::Service("me.timschneeberger.jdsp4linux", "/jdsp4linux/service",
                                                           QDBusConnection::sessionBus(), this);
#endif
}

#ifndef _WIN32
template<typename T>
std::optional<T> handleReply(QDBusPendingReply<T>& reply) {
    reply.waitForFinished();

    if(reply.isError()) {
        Log::error(reply.error().message());
        return {};
    }
    else
        return { reply.value() };
}

bool handleVoidReply(QDBusPendingReply<>& reply) {
    reply.waitForFinished();

    if(reply.isError()) {
        Log::error(reply.error().message());;
    }
    return reply.isError();
}
#endif

bool validateKey(const QString& keyString, DspConfig::Key& resolvedKey) {
    QMetaEnum meta = QMetaEnum::fromType<DspConfig::Key>();
    bool valid = false;
    resolvedKey = static_cast<DspConfig::Key>(meta.keyToValue(keyString.toLocal8Bit().constData(), &valid));
    if(!valid) {
        Log::error("Configuration key does not exist");
        return false;
    }
    return true;
}

bool CliRemoteController::isConnected() const
{
#ifdef _WIN32
    return service->isConnected();
#endif
#ifndef _WIN32
    return service->isValid();
#endif
}

bool CliRemoteController::set(const QString &key, const QVariant &value) const
{
#ifdef _WIN32
    if(!checkConnectionAndLog()) {
        DspConfig::Key resolvedKey;
        bool valid = validateKey(key, resolvedKey);
        if(!valid)
            return false;

        DspConfig::instance().set(resolvedKey, value);
        return true;
    }

    QString response = service->sendCommand("SET_AND_COMMIT|" + key + "|" + value.toString());
    return response.startsWith("OK");
#else
    if(!checkConnectionAndLog()) {
        DspConfig::Key resolvedKey;
        bool valid = validateKey(key, resolvedKey);
        if(!valid)
            return false;

        DspConfig::instance().set(resolvedKey, value);
        return true;
    }

    auto reply = service->setAndCommit(key, QDBusVariant(value));
    return handleVoidReply(reply);
#endif
}

bool CliRemoteController::get(const QString &key) const
{
    std::optional<QString> result;
#ifdef _WIN32
    if(!checkConnectionAndLog()) {
        DspConfig::Key resolvedKey;
        bool valid = validateKey(key, resolvedKey);
        if(!valid)
            result = std::nullopt;

        result = DspConfig::instance().get<QString>(resolvedKey, nullptr, false);
    }
    else {
        QString response = service->sendCommand("GET|" + key);
        if (response.startsWith("OK|"))
            result = response.mid(3);
        else
            result = std::nullopt;
    }
#else
    if(!checkConnectionAndLog()) {
        DspConfig::Key resolvedKey;
        bool valid = validateKey(key, resolvedKey);
        if(!valid)
            result = std::nullopt;

        result = DspConfig::instance().get<QString>(resolvedKey, nullptr, false);
    }
    else {
        auto reply = service->get(key);
        auto received = handleReply<QString>(reply);
        result = received;
    }
#endif

    if(result.has_value()) {
        Log::console(result.value(), true);
    }

    return result.has_value();
}

bool CliRemoteController::getAll() const
{
    std::optional<QString> result;
#ifdef _WIN32
    if(!checkConnectionAndLog()) {
        result = DspConfig::instance().serialize();
    }
    else {
        QString response = service->sendCommand("GET_ALL");
        if (response.startsWith("OK|"))
            result = response.mid(3);
        else
            result = std::nullopt;
    }
#else
    if(!checkConnectionAndLog()) {
        result = DspConfig::instance().serialize();
    }
    else {
        auto reply = service->getAll();
        auto received = handleReply<QString>(reply);
        result = received;
    }
#endif

    if(result.has_value()) {
        Log::console(result.value(), true);
    }

    return result.has_value();
}

bool CliRemoteController::listKeys() const
{
    QStringList keys;
#ifdef _WIN32
    if(checkConnectionAndLog()) {
        QString response = service->sendCommand("GET_KEYS");
        if (response.startsWith("OK|"))
            keys = response.mid(3).split(";");
    }
#else
    if(checkConnectionAndLog()) {
        auto reply = service->getKeys();
        keys = handleReply(reply).value_or(keys);
    }
#endif
    else {
        keys = DspConfig::getKeys();
    }

    for (const auto& key : keys)
    {
        Log::console(key, true);
    }

    return true;
}

bool CliRemoteController::listPresets() const
{
    QStringList presets;
#ifdef _WIN32
    if(checkConnectionAndLog()) {
        QString response = service->sendCommand("GET_PRESETS");
        if (response.startsWith("OK|"))
            presets = response.mid(3).split(";");
    }
#endif
#ifndef _WIN32
    if(checkConnectionAndLog()) {
        auto reply = service->getPresets();
        presets = handleReply(reply).value_or(presets);
    }
#endif
    else {
        auto *model = PresetManager::instance().presetModel();
        model->rescan();
        presets = model->getList();
    }

    for (const auto& preset : presets)
    {
        Log::console(preset, true);
    }

    return true;
}

bool CliRemoteController::loadPreset(const QString &name) const
{
#ifdef _WIN32
    if(!checkConnectionAndLog()) {
        return PresetManager::instance().load(name);
    }
    QString response = service->sendCommand("LOAD_PRESET|" + name);
    return response.startsWith("OK");
#else
    if(!checkConnectionAndLog()) {
        return PresetManager::instance().load(name);
    }
    auto reply = service->loadPreset(name);
    return handleVoidReply(reply);
#endif
}

bool CliRemoteController::savePreset(const QString &name) const
{
#ifdef _WIN32
    if(!checkConnectionAndLog()) {
        PresetManager::instance().save(name);
        return true;
    }
    QString response = service->sendCommand("SAVE_PRESET|" + name);
    return response.startsWith("OK");
#else
    if(!checkConnectionAndLog()) {
        PresetManager::instance().save(name);
        return true;
    }
    auto reply = service->savePreset(name);
    return handleVoidReply(reply);
#endif
}

bool CliRemoteController::deletePreset(const QString &name) const
{
#ifdef _WIN32
    if(!checkConnectionAndLog()) {
        return PresetManager::instance().remove(name);
    }
    QString response = service->sendCommand("DELETE_PRESET|" + name);
    return response.startsWith("OK");
#else
    if(!checkConnectionAndLog()) {
        return PresetManager::instance().remove(name);
    }
    auto reply = service->deletePreset(name);
    return handleVoidReply(reply);
#endif
}

bool CliRemoteController::showStatus() const
{
#ifdef _WIN32
    if(!checkConnectionAndLog()) {
        // Show local status
        DspConfig::Key resolvedKey;
        Q_UNUSED(resolvedKey)
    }
    else {
        QString flavor = service->sendCommand("APP_FLAVOR");
        QString version = service->sendCommand("APP_VERSION");
        QString coreVersion = service->sendCommand("CORE_VERSION");
        QString isProcessing = service->sendCommand("IS_PROCESSING");
        QString samplingRate = service->sendCommand("SAMPLING_RATE");
        QString audioFormat = service->sendCommand("AUDIO_FORMAT");

        // Strip "OK|" prefix
        if (flavor.startsWith("OK|")) flavor = flavor.mid(3);
        if (version.startsWith("OK|")) version = version.mid(3);
        if (coreVersion.startsWith("OK|")) coreVersion = coreVersion.mid(3);
        if (isProcessing.startsWith("OK|")) isProcessing = isProcessing.mid(3);
        if (samplingRate.startsWith("OK|")) samplingRate = samplingRate.mid(3);
        if (audioFormat.startsWith("OK|")) audioFormat = audioFormat.mid(3);

        Log::console("App flavor: " + flavor, true);
        Log::console("App version: " + version, true);
        Log::console("Core version: " + coreVersion, true);
        Log::console("Processing: " + isProcessing, true);
        Log::console("Sampling rate: " + samplingRate, true);
        Log::console("Audio format: " + audioFormat, true);
    }
    return true;
#else
    if(!checkConnectionAndLog()) {
        // Local status
        return true;
    }

    Log::console("App flavor: " + service->appFlavor(), true);
    Log::console("App version: " + service->appVersion(), true);
    Log::console("Core version: " + service->coreVersion(), true);
    Log::console("Processing: " + QString(service->isProcessing() ? "true" : "false"), true);
    Log::console("Sampling rate: " + service->samplingRate(), true);
    Log::console("Audio format: " + service->audioFormat(), true);
    return true;
#endif
}

bool CliRemoteController::checkConnectionAndLog() const
{
#ifdef _WIN32
    if (!service->isConnected())
    {
        if (service->connectToServer())
        {
            return true;
        }

        Log::warning("Cannot connect to JamesDSP service. Running in local mode.");
        return false;
    }
    return true;
#else
    if (!service->isValid())
    {
        if (QDBusConnection::sessionBus().isConnected() &&
            QDBusConnection::sessionBus().interface()->isServiceRegistered("me.timschneeberger.jdsp4linux"))
        {
            Log::warning("Cannot connect to JamesDSP D-Bus service. Running in local mode.");
            return false;
        }
    }
    return service->isValid();
#endif
}

bool CliRemoteController::requireConnectionAndLog() const
{
    if (!checkConnectionAndLog())
    {
        Log::error("No connection to JamesDSP service. Use --help for usage.");
        return false;
    }
    return true;
}