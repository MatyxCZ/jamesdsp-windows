#ifndef CLIREMOTECONTROLLER_H
#define CLIREMOTECONTROLLER_H

#include <QObject>

#ifdef _WIN32
#include "utils/PipeIPCHandler.h"
#else
#include "utils/dbus/ClientProxy.h"
#endif

class CliRemoteController : public QObject
{
    Q_OBJECT
public:
    explicit CliRemoteController(QObject *parent = nullptr);

    bool isConnected() const;
    bool set(const QString& key, const QVariant& value) const;
    bool get(const QString& key) const;
    bool getAll() const;
    bool listKeys() const;
    bool listPresets() const;
    bool loadPreset(const QString& name) const;
    bool savePreset(const QString& name) const;
    bool deletePreset(const QString& name) const;
    bool showStatus() const;

private:
#ifdef _WIN32
    PipeIPCClient* service = nullptr;
#else
    me::timschneeberger::jdsp4linux::Service* service;
#endif

    bool checkConnectionAndLog() const;
    bool requireConnectionAndLog() const;
};

#endif // CLIREMOTECONTROLLER_H