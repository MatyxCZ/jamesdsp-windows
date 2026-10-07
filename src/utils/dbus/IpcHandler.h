#ifndef IPCHANDLER_H
#define IPCHANDLER_H

#ifdef _WIN32
#include <QObject>
#else
#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusVariant>
#include <QObject>
#endif

class ServiceAdaptor;
class PipeIPCServer;
class IAudioService;

class IpcHandler : public QObject
#ifndef _WIN32
    , protected QDBusContext
#endif
{
    Q_OBJECT

public:
    IpcHandler(IAudioService* service, QObject* parent = nullptr);
    ~IpcHandler();

    bool isServiceReady();

#ifdef _WIN32
    // On Windows, the pipe server handles IPC directly
    PipeIPCServer* pipeServer() const { return _pipeServer; }
#endif

    Q_PROPERTY(QString AppFlavor READ appFlavor)
    QString appFlavor() const;

    Q_PROPERTY(QString AppVersion READ appVersion)
    QString appVersion() const;

    Q_PROPERTY(QString CoreVersion READ coreVersion)
    QString coreVersion() const;

    Q_PROPERTY(bool IsProcessing READ isProcessing)
    bool isProcessing() const;

    Q_PROPERTY(QString SamplingRate READ samplingRate)
    QString samplingRate() const;

    Q_PROPERTY(QString AudioFormat READ audioFormat)
    QString audioFormat() const;

public slots:
    QStringList getKeys() const;
    void commit() const;
#ifdef _WIN32
    void set(const QString &key, const QString &value) const;
    void setAndCommit(const QString &key, const QString &value) const;
#else
    void set(const QString &key, const QDBusVariant &value) const;
    void setAndCommit(const QString &key, const QDBusVariant &value) const;
#endif
    QString get(const QString &key) const;
    QString getAll() const;
    QStringList getPresets() const;
    void loadPreset(const QString &name) const;
    void savePreset(const QString &name) const;
    void deletePreset(const QString &name) const;

private:
    IAudioService* _service;

#ifdef _WIN32
    PipeIPCServer* _pipeServer = nullptr;
    bool _registered = false;

    void setInternal(const QString &key, const QString &value) const;
#else
    QDBusConnection _connection = QDBusConnection::sessionBus();
    ServiceAdaptor* _dbusAdapter;
    bool _registered;

    void setInternal(const QString &key, const QDBusVariant &value) const;
#endif
};

#endif // IPCHANDLER_H