#ifndef PIPEIPCHANDLER_H
#define PIPEIPCHANDLER_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QMap>
#include <QLocalServer>
#include <QLocalSocket>

class IpcHandler;

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
    explicit PipeIPCServer(IpcHandler* handler, QObject* parent = nullptr);
    ~PipeIPCServer();

    bool start();
    void stop();

signals:
    void raiseWindow();

private slots:
    void onNewConnection();
    void onReadyRead();
    void onDisconnected();

private:
    QString executeCommand(const QString& command, const QStringList& args);

    QLocalServer* _server = nullptr;
    IpcHandler* _handler = nullptr;
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

    QString sendCommand(const QString& command, const QStringList& args = {});

signals:
    void connected();
    void disconnected();

private:
    QLocalSocket* _socket = nullptr;
};

#endif // PIPEIPCHANDLER_H