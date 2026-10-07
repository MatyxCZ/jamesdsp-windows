#ifndef PIPEIPCHANDLER_H
#define PIPEIPCHANDLER_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QThread>
#include <QLocalServer>
#include <QLocalSocket>

class IAudioService;
class DspConfig;

/*
 * On Windows, we use QLocalServer/QLocalSocket (named pipes) instead of D-Bus
 * for inter-process communication. This provides the same functionality:
 * - Single instance detection
 * - Remote control (CLI commands)
 * - Window raise requests
 */

class PipeIPCServer : public QObject
{
    Q_OBJECT
public:
    explicit PipeIPCServer(IAudioService* service, QObject* parent = nullptr);
    ~PipeIPCServer();

    bool start();
    void stop();

    IAudioService* service() const { return _service; }

signals:
    void raiseWindow();

private slots:
    void onNewConnection();
    void onReadyRead();
    void onDisconnected();

private:
    void handleCommand(QLocalSocket* socket, const QString& command);
    QString executeCommand(const QString& cmd, const QStringList& args);

    QLocalServer* _server = nullptr;
    IAudioService* _service = nullptr;
    QMap<QLocalSocket*, QByteArray> _buffers;
};

class PipeIPCClient : public QObject
{
    Q_OBJECT
public:
    explicit PipeIPCClient(QObject* parent = nullptr);
    ~PipeIPCClient();

    bool connectToServer();
    void disconnectFromServer();
    bool isConnected() const { return _socket && _socket->state() == QLocalSocket::ConnectedState; }

    QString sendCommand(const QString& command);

signals:
    void connected();
    void disconnected();

private:
    QLocalSocket* _socket = nullptr;
};

#endif // PIPEIPCHANDLER_H