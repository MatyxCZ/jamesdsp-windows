#include "IpcHandler.h"
#include "IAudioService.h"
#include "config/DspConfig.h"
#include "data/PresetManager.h"
#include "data/model/PresetListModel.h"
#include "utils/Log.h"
#ifdef _WIN32
#include "../PipeIPCHandler.h"
#else
#include "ServerAdaptor.h"
#include "ClientProxy.h"
#endif
#include "utils/VersionMacros.h"

IpcHandler::IpcHandler(IAudioService* service, QObject* parent) : QObject(parent), _service(service)
{
#ifdef _WIN32
    _pipeServer = new PipeIPCServer(_service, this);
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
#ifdef _WIN32
        // Return empty on Windows (no D-Bus error reply)
#else
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
void IpcHandler::set(const QString &key, const QString &value) const
{
    setInternal(key, value);
}

void IpcHandler::setAndCommit(const QString &key, const QString &value) const
{
    setInternal(key, value);
    commit();
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

void IpcHandler::loadPreset(const QString &name) const
{
    if(!PresetManager::instance().load(name))
#ifdef _WIN32
        ; // No error reply on Windows
#else
        sendErrorReply(QDBusError::InvalidArgs, "Preset does not exist");
#endif
}

void IpcHandler::savePreset(const QString &name) const
{
    PresetManager::instance().save(name);
}

void IpcHandler::deletePreset(const QString &name) const
{
    if(!PresetManager::instance().remove(name))
#ifdef _WIN32
        ; // No error reply on Windows
#else
        sendErrorReply(QDBusError::InvalidArgs, "Preset does not exist");
#endif
}

#ifdef _WIN32
void IpcHandler::setInternal(const QString &key, const QString &value) const
{
    QMetaEnum meta = QMetaEnum::fromType<DspConfig::Key>();
    bool valid = false;
    auto resolvedKey = static_cast<DspConfig::Key>(meta.keyToValue(key.toLocal8Bit().constData(), &valid));

    if(!valid) {
        return;
    }

    DspConfig::instance().set(resolvedKey, QVariant(value));
}
#endif
#ifndef _WIN32
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
    // Type validation
    if(var.type() == QVariant::Type::String ||
            var.type() == QVariant::Type::Bool ||
            var.type() == QVariant::Type::Double ||
            var.canConvert(QVariant::Type::Int)) {
        DspConfig::instance().set(resolvedKey, var);
    }
    else
        sendErrorReply(QDBusError::InvalidArgs, "Invalid variant value type. Variant must contain one of these types: String, Boolean, Integer, or Float");
}
#endif