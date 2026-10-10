#include "IpcHandler.h"
#include "IAudioService.h"
#include "config/DspConfig.h"
#include "data/PresetManager.h"
#include "data/model/PresetListModel.h"
#include "utils/Log.h"
#include "utils/QtCompat.h"
#ifdef _WIN32
#include "../PipeIPCHandler.h"
#else
#include "ServerAdaptor.h"
#endif
#include "utils/VersionMacros.h"

#include <QVector>

IpcHandler::IpcHandler(IAudioService* service, QObject* parent) : QObject(parent), _service(service)
{
#ifdef _WIN32
    _pipeServer = new PipeIPCServer(this, this);
    _registered = _pipeServer->start();
#else
    _dbusAdapter = new ServiceAdaptor(this);
    _registered  = _connection.registerObject("/jdsp4linux/service", this) &&
            _connection.registerService("me.timschneeberger.jdsp4linux");
#endif
}

IpcHandler::~IpcHandler()
{
#ifdef _WIN32
    delete _pipeServer;
#else
    _dbusAdapter->deleteLater();
#endif
}

bool IpcHandler::isServiceReady()
{
    if (_registered)
    {
        Log::information("Main service registration successful");
        return true;
    }
    else
    {
        Log::warning("Main service registration failed. Another instance may be running.");
        return false;
    }
}

QString IpcHandler::appFlavor() const
{
    return APP_FLAVOR_SHORT;
}

QString IpcHandler::appVersion() const
{
    return APP_VERSION_FULL;
}

QString IpcHandler::coreVersion() const
{
    return STR(JDSP_VERSION);
}

bool IpcHandler::isProcessing() const
{
    return _service->status().IsProcessing;
}

QString IpcHandler::samplingRate() const
{
    return QString::fromStdString(_service->status().SamplingRate);
}

QString IpcHandler::audioFormat() const
{
    return QString::fromStdString(_service->status().AudioFormat);
}

QStringList IpcHandler::getKeys() const
{
    return DspConfig::getKeys();
}

void IpcHandler::commit() const
{
    DspConfig::instance().commit();
    DspConfig::instance().save();
    emit DspConfig::instance().configBuffered();
}

QString IpcHandler::get(const QString &key) const
{
    bool exists = false;

    QVariant value = DspConfig::instance().get<QVariant>(key, &exists, false);
    if(!exists) {
#ifndef _WIN32
        sendErrorReply(QDBusError::InvalidArgs, "Configuration key does not exist");
#endif
        return "";
    }
    return value.toString();
}

QString IpcHandler::getAll() const
{
    return DspConfig::instance().serialize();
}

#ifdef _WIN32
bool IpcHandler::set(const QString &key, const QVariant &value) const
{
    return setInternal(key, value);
}

bool IpcHandler::setAndCommit(const QString &key, const QVariant &value) const
{
    if (!setInternal(key, value))
        return false;
    commit();
    return true;
}
#else
void IpcHandler::set(const QString &key, const QDBusVariant &value) const
{
    setInternal(key, value);
}

void IpcHandler::setAndCommit(const QString &key, const QDBusVariant &value) const
{
    setInternal(key, value);
    commit();
}
#endif

QStringList IpcHandler::getPresets() const
{
    auto *model = PresetManager::instance().presetModel();
    model->rescan();
    return model->getList();
}

#ifdef _WIN32
bool IpcHandler::loadPreset(const QString &name) const
{
    return PresetManager::instance().load(name);
}

bool IpcHandler::savePreset(const QString &name) const
{
    PresetManager::instance().save(name);
    return PresetManager::instance().exists(name);
}

bool IpcHandler::deletePreset(const QString &name) const
{
    return PresetManager::instance().remove(name);
}
#else
void IpcHandler::loadPreset(const QString &name) const
{
    if(!PresetManager::instance().load(name))
        sendErrorReply(QDBusError::InvalidArgs, "Preset does not exist");
}

void IpcHandler::savePreset(const QString &name) const
{
    PresetManager::instance().save(name);
}

void IpcHandler::deletePreset(const QString &name) const
{
    if(!PresetManager::instance().remove(name))
        sendErrorReply(QDBusError::InvalidArgs, "Preset does not exist");
}
#endif

void IpcHandler::setPresetRule(const QString &deviceName, const QString &deviceId, const QString &routeName, const QString &routeId, const QString &preset) const
{
#ifdef USE_PULSEAUDIO
    sendErrorReply(QDBusError::ErrorType::NotSupported, "This feature is only supported in the PipeWire version of JamesDSP");
    return;
#endif
    PresetManager::instance().addRule(PresetRule(deviceName, deviceId, routeName, routeId, preset));
}

void IpcHandler::deletePresetRule(const QString &deviceId, const QString &routeId) const
{
#ifdef USE_PULSEAUDIO
    sendErrorReply(QDBusError::ErrorType::NotSupported, "This feature is only supported in the PipeWire version of JamesDSP");
    return;
#endif
    PresetManager::instance().removeRule(deviceId, routeId);
}

QList<PresetRule> IpcHandler::getPresetRules() const
{
#ifdef USE_PULSEAUDIO
    sendErrorReply(QDBusError::ErrorType::NotSupported, "This feature is only supported in the PipeWire version of JamesDSP");
    return QList<PresetRule>();
#endif
    return PresetManager::instance().rules().toList();
}

QList<IOutputDevice> IpcHandler::getOutputDevices() const
{
    auto devices = _service->outputDevices();
    return QVector<IOutputDevice>(devices.begin(), devices.end()).toList();
}

void IpcHandler::relinkAudioPipeline() const
{
    _service->reloadService();
    DspConfig::instance().commit();
}

#ifdef _WIN32
bool IpcHandler::setInternal(const QString &key, const QVariant &value) const
{
    QMetaEnum meta = QMetaEnum::fromType<DspConfig::Key>();
    bool valid = false;
    const auto resolvedKey = static_cast<DspConfig::Key>(meta.keyToValue(key.toLocal8Bit().constData(), &valid));
    if (!valid)
        return false;

    const int type = QtCompat::variantTypeId(value);
    if (type != QMetaType::QString && type != QMetaType::Bool &&
        type != QMetaType::Double && !value.canConvert(QMetaType::Int))
        return false;

    DspConfig::instance().set(resolvedKey, value);
    return true;
}
#else
void IpcHandler::setInternal(const QString &key, const QDBusVariant &value) const
{
    QMetaEnum meta = QMetaEnum::fromType<DspConfig::Key>();
    bool valid = false;
    auto resolvedKey = static_cast<DspConfig::Key>(meta.keyToValue(key.toLocal8Bit().constData(), &valid));

    if(!valid) {
        sendErrorReply(QDBusError::InvalidArgs, "Configuration key does not exist");
        return;
    }

    QVariant var = value.variant();
    int type = QtCompat::variantTypeId(var);
    // Type validation
    if(type == QMetaType::QString ||
       type == QMetaType::Bool ||
       type == QMetaType::Double ||
       var.canConvert(QMetaType::Int)) {
        DspConfig::instance().set(resolvedKey, var);
    }
    else
        sendErrorReply(QDBusError::InvalidArgs, "Invalid variant value type. Variant must contain one of these types: String, Boolean, Integer, or Float");
}
#endif