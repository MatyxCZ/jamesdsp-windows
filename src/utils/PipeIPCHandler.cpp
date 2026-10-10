#include "PipeIPCHandler.h"
#include "dbus/IpcHandler.h"
#include "utils/Log.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QElapsedTimer>

#ifdef _WIN32
#include <windows.h>
#endif

// ============================================================================
// PipeIPCServer
// ============================================================================

PipeIPCServer::PipeIPCServer(IpcHandler* handler, QObject* parent)
    : QObject(parent), _handler(handler)
{
}

PipeIPCServer::~PipeIPCServer()
{
    stop();
}

bool PipeIPCServer::start()
{
    QString pipeName = QLatin1String("jamesdsp_ipc");
    QString fullPath = QLatin1String("\\\\.\\pipe\\") + pipeName;

    _server = new QLocalServer(this);
    _server->setSocketOptions(QLocalServer::UserAccessOption);

    if (!_server->listen(pipeName))
    {
        Log::error("Failed to start IPC server (named pipe): " + _server->errorString());
        delete _server;
        _server = nullptr;
        return false;
    }

    connect(_server, &QLocalServer::newConnection, this, &PipeIPCServer::onNewConnection);
    Log::information("IPC server started on pipe: " + fullPath);
    return true;
}

void PipeIPCServer::stop()
{
    if (_server)
        _server->close();

    const auto sockets = _buffers.keys();
    _buffers.clear();
    for (auto* socket : sockets)
    {
        socket->disconnect(this);
        socket->abort();
        socket->deleteLater();
    }
    delete _server;
    _server = nullptr;
}

void PipeIPCServer::onNewConnection()
{
    while (QLocalSocket* socket = _server->nextPendingConnection())
    {
        _buffers[socket] = QByteArray();
        connect(socket, &QLocalSocket::readyRead, this, &PipeIPCServer::onReadyRead);
        connect(socket, &QLocalSocket::disconnected, this, &PipeIPCServer::onDisconnected);
        Log::debug("IPC client connected");
    }
}

void PipeIPCServer::onReadyRead()
{
    QLocalSocket* socket = qobject_cast<QLocalSocket*>(sender());
    if (!socket)
        return;

    QByteArray& buffer = _buffers[socket];
    buffer.append(socket->readAll());

    // Process complete lines (newline-delimited protocol)
    while (true)
    {
        int newlinePos = buffer.indexOf('\n');
        if (newlinePos == -1)
            break;

        QByteArray line = buffer.left(newlinePos);
        buffer.remove(0, newlinePos + 1);

        const QList<QByteArray> fields = line.split('|');
        const QString command = QString::fromUtf8(fields.first());
        QStringList args;
        for (int i = 1; i < fields.size(); ++i)
            args.append(QString::fromUtf8(QByteArray::fromPercentEncoding(fields.at(i))));
        const QString response = executeCommand(command, args);

        // Send response back
        QByteArray responseData = response.toUtf8().toPercentEncoding() + '\n';
        socket->write(responseData);
        socket->flush();
    }
}

void PipeIPCServer::onDisconnected()
{
    QLocalSocket* socket = qobject_cast<QLocalSocket*>(sender());
    if (!socket)
        return;

    Log::debug("IPC client disconnected");
    _buffers.remove(socket);
    socket->deleteLater();
}

QString PipeIPCServer::executeCommand(const QString& command, const QStringList& args)
{
    const QString cmd = command.toUpper();
    const auto ok = [](const QString& value) { return QStringLiteral("OK|") + value; };
    const auto json = [](const QJsonArray& value) {
        return QString::fromUtf8(QJsonDocument(value).toJson(QJsonDocument::Compact));
    };
    const auto strings = [&json](const QStringList& values) {
        QJsonArray array;
        for (const auto& value : values)
            array.append(value);
        return json(array);
    };

    if (cmd == "GET" && args.size() == 1) {
        if (!_handler->getKeys().contains(args.at(0)))
            return "ERROR|Configuration key does not exist";
        return ok(_handler->get(args.at(0)));
    }
    if ((cmd == "SET" || cmd == "SET_AND_COMMIT") && args.size() == 2) {
        // JSON preserves the configuration value type across the pipe.
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(args.at(1).toUtf8(), &error);
        if (error.error != QJsonParseError::NoError || !document.isArray() || document.array().size() != 1)
            return "ERROR|Invalid configuration value";
        const QVariant value = document.array().at(0).toVariant();
        const bool success = cmd == "SET" ? _handler->set(args.at(0), value)
                                          : _handler->setAndCommit(args.at(0), value);
        return success ? QStringLiteral("OK") : QStringLiteral("ERROR|Invalid configuration key or value");
    }
    if (cmd == "LOAD_PRESET" && args.size() == 1)
        return _handler->loadPreset(args.at(0)) ? "OK" : "ERROR|Preset does not exist";
    if (cmd == "SAVE_PRESET" && args.size() == 1)
        return _handler->savePreset(args.at(0)) ? "OK" : "ERROR|Could not save preset";
    if (cmd == "DELETE_PRESET" && args.size() == 1)
        return _handler->deletePreset(args.at(0)) ? "OK" : "ERROR|Preset does not exist";
    if (cmd == "SET_PRESET_RULE" && args.size() == 5) {
        _handler->setPresetRule(args.at(0), args.at(1), args.at(2), args.at(3), args.at(4));
        return "OK";
    }
    if (cmd == "DELETE_PRESET_RULE" && args.size() == 2) {
        _handler->deletePresetRule(args.at(0), args.at(1));
        return "OK";
    }
    if (!args.isEmpty())
        return "ERROR|Unknown command or invalid arguments: " + cmd;

    if (cmd == "GET_ALL")
        return ok(_handler->getAll());
    if (cmd == "GET_KEYS")
        return ok(strings(_handler->getKeys()));
    if (cmd == "GET_PRESETS")
        return ok(strings(_handler->getPresets()));
    if (cmd == "GET_PRESET_RULES") {
        QJsonArray rules;
        for (const auto& rule : _handler->getPresetRules())
            rules.append(rule.toJson());
        return ok(json(rules));
    }
    if (cmd == "GET_OUTPUT_DEVICES") {
        QJsonArray devices;
        for (const auto& device : _handler->getOutputDevices()) {
            QJsonObject entry;
            entry["id"] = static_cast<double>(device.id);
            entry["name"] = QString::fromStdString(device.name);
            entry["description"] = QString::fromStdString(device.description);
            entry["output_route_name"] = QString::fromStdString(device.output_route_name);
            entry["output_route_description"] = QString::fromStdString(device.output_route_description);
            QJsonArray routes;
            for (const auto& route : device.output_routes) {
                QJsonObject item;
                item["name"] = QString::fromStdString(route.name);
                item["description"] = QString::fromStdString(route.description);
                routes.append(item);
            }
            entry["output_routes"] = routes;
            devices.append(entry);
        }
        return ok(json(devices));
    }
    if (cmd == "COMMIT") {
        _handler->commit();
        return "OK";
    }
    if (cmd == "RELINK") {
        _handler->relinkAudioPipeline();
        return "OK";
    }
    if (cmd == "STATUS" || cmd == "IS_CONNECTED")
        return ok("true");
    if (cmd == "APP_FLAVOR")
        return ok(_handler->appFlavor());
    if (cmd == "APP_VERSION")
        return ok(_handler->appVersion());
    if (cmd == "CORE_VERSION")
        return ok(_handler->coreVersion());
    if (cmd == "IS_PROCESSING")
        return ok(_handler->isProcessing() ? "true" : "false");
    if (cmd == "SAMPLING_RATE")
        return ok(_handler->samplingRate());
    if (cmd == "AUDIO_FORMAT")
        return ok(_handler->audioFormat());
    if (cmd == "RAISE_WINDOW") {
        emit raiseWindow();
        return "OK";
    }
    return "ERROR|Unknown command: " + cmd;
}

// ============================================================================
// PipeIPCClient
// ============================================================================

PipeIPCClient::PipeIPCClient(QObject* parent)
    : QObject(parent)
{
    _socket = new QLocalSocket(this);
}

PipeIPCClient::~PipeIPCClient()
{
    disconnectFromServer();
}

bool PipeIPCClient::connectToServer()
{
    if (_socket->state() == QLocalSocket::ConnectedState)
        return true;

    _socket->connectToServer("jamesdsp_ipc");
    // A failed previous attempt must not leave a pending connection alive.
    if (!_socket->waitForConnected(2000))
    {
        _socket->abort();
        Log::debug("Failed to connect to IPC server: " + _socket->errorString());
        return false;
    }

    emit connected();
    return true;
}

void PipeIPCClient::disconnectFromServer()
{
    if (_socket && _socket->state() == QLocalSocket::ConnectedState)
    {
        _socket->disconnectFromServer();
        if (_socket->state() != QLocalSocket::UnconnectedState)
            _socket->waitForDisconnected(1000);
    }
}

QString PipeIPCClient::sendCommand(const QString& command, const QStringList& args)
{
    if (_socket->state() != QLocalSocket::ConnectedState)
        return "ERROR|Not connected to server";

#ifdef _WIN32
    if (command == "RAISE_WINDOW") {
        ULONG serverProcessId = 0;
        if (GetNamedPipeServerProcessId(reinterpret_cast<HANDLE>(_socket->socketDescriptor()), &serverProcessId))
            AllowSetForegroundWindow(serverProcessId);
    }
#endif

    QByteArray data = command.toUtf8();
    for (const auto& arg : args)
        data += '|' + arg.toUtf8().toPercentEncoding();
    data += '\n';
    if (_socket->write(data) != data.size()) {
        _socket->abort();
        return "ERROR|Failed to send command";
    }
    _socket->flush();

    QElapsedTimer timer;
    timer.start();
    while (!_socket->canReadLine()) {
        const int remaining = 5000 - static_cast<int>(timer.elapsed());
        if (remaining <= 0 || !_socket->waitForReadyRead(remaining)) {
            _socket->abort();
            return "ERROR|Timeout waiting for response";
        }
    }
    return QString::fromUtf8(QByteArray::fromPercentEncoding(_socket->readLine().trimmed()));
}