#include "PipeIPCHandler.h"
#include "IAudioService.h"
#include "config/DspConfig.h"
#include "data/PresetManager.h"
#include "data/model/PresetListModel.h"
#include "utils/Log.h"
#include "utils/VersionMacros.h"

#include <QDataStream>
#include <QDateTime>
#include <QElapsedTimer>

// ============================================================================
// PipeIPCServer
// ============================================================================

PipeIPCServer::PipeIPCServer(IAudioService* service, QObject* parent)
    : QObject(parent), _service(service)
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
    // Remove any stale pipe from a previous crashed instance
    QLocalServer::removeServer(pipeName);

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
    {
        _server->close();
        delete _server;
        _server = nullptr;
    }

    // Close all client sockets
    for (auto it = _buffers.keyBegin(); it != _buffers.keyEnd(); ++it)
    {
        (*it)->disconnectFromServer();
    }
    _buffers.clear();
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

        QString command = QString::fromUtf8(line).trimmed();
        QString response = executeCommand(command, QStringList());

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

QString PipeIPCServer::executeCommand(const QString& cmd, const QStringList& /*args*/)
{
    QStringList parts = cmd.split('|', Qt::SkipEmptyParts);
    if (parts.isEmpty() || !parts[0].compare("STATUS", Qt::CaseInsensitive))
    {
        if (parts.size() > 1)
        {
            QString command = parts[0].toUpper();
            if (command == "GET")
            {
                QString key = parts[1];
                if (parts.size() > 2)
                    key = parts[2]; // Support both "GET|key" and "GET|get|key"

                // Parse key
                bool ok = false;
                int keyVal = key.toInt(&ok);
                if (!ok)
                {
                    QMetaEnum meta = QMetaEnum::fromType<DspConfig::Key>();
                    keyVal = meta.keyToValue(key.toLocal8Bit().constData(), &ok);
                }

                if (!ok)
                    return "ERROR|Configuration key does not exist";

                QString value = DspConfig::instance().get<QString>(
                    static_cast<DspConfig::Key>(keyVal), nullptr, false);
                return "OK|" + value;
            }
            else if (command == "SET")
            {
                QString key = parts[1];
                QString value = parts.size() > 2 ? parts[2] : QString();

                QMetaEnum meta = QMetaEnum::fromType<DspConfig::Key>();
                bool ok = false;
                int keyVal = meta.keyToValue(key.toLocal8Bit().constData(), &ok);
                if (!ok)
                    return "ERROR|Configuration key does not exist";

                DspConfig::instance().set(static_cast<DspConfig::Key>(keyVal), value);
                return "OK";
            }
            else if (command == "SET_AND_COMMIT")
            {
                QString key = parts[1];
                QString value = parts.size() > 2 ? parts[2] : QString();

                QMetaEnum meta = QMetaEnum::fromType<DspConfig::Key>();
                bool ok = false;
                int keyVal = meta.keyToValue(key.toLocal8Bit().constData(), &ok);
                if (!ok)
                    return "ERROR|Configuration key does not exist";

                DspConfig::instance().set(static_cast<DspConfig::Key>(keyVal), value);
                DspConfig::instance().commit();
                DspConfig::instance().save();
                return "OK";
            }
            else if (command == "GET_ALL")
            {
                return "OK|" + DspConfig::instance().serialize();
            }
            else if (command == "GET_KEYS")
            {
                QStringList keys = DspConfig::getKeys();
                return "OK|" + keys.join(";");
            }
            else if (command == "COMMIT")
            {
                DspConfig::instance().commit();
                DspConfig::instance().save();
                return "OK";
            }
            else if (command == "GET_PRESETS")
            {
                auto* model = PresetManager::instance().presetModel();
                model->rescan();
                return "OK|" + model->getList().join(";");
            }
            else if (command == "LOAD_PRESET")
            {
                QString name = parts.size() > 1 ? parts[1] : QString();
                if (PresetManager::instance().load(name))
                    return "OK";
                else
                    return "ERROR|Preset does not exist";
            }
            else if (command == "SAVE_PRESET")
            {
                QString name = parts.size() > 1 ? parts[1] : QString();
                PresetManager::instance().save(name);
                return "OK";
            }
            else if (command == "DELETE_PRESET")
            {
                QString name = parts.size() > 1 ? parts[1] : QString();
                if (PresetManager::instance().remove(name))
                    return "OK";
                else
                    return "ERROR|Preset does not exist";
            }
            else if (command == "IS_CONNECTED")
            {
                return "OK|true";
            }
            else if (command == "APP_FLAVOR")
            {
                return "OK|" + QString(APP_FLAVOR_SHORT);
            }
            else if (command == "APP_VERSION")
            {
                return "OK|" + QString(APP_VERSION_FULL);
            }
            else if (command == "CORE_VERSION")
            {
                return "OK|" + QString(STR(JDSP_VERSION));
            }
            else if (command == "IS_PROCESSING")
            {
                return "OK|" + QString(_service->status().IsProcessing ? "true" : "false");
            }
            else if (command == "SAMPLING_RATE")
            {
                return "OK|" + QString::fromStdString(_service->status().SamplingRate);
            }
            else if (command == "AUDIO_FORMAT")
            {
                return "OK|" + QString::fromStdString(_service->status().AudioFormat);
            }
            else if (command == "RAISE_WINDOW")
            {
                emit raiseWindow();
                return "OK";
            }
            else
            {
                return "ERROR|Unknown command: " + command;
            }
        }
        else
        {
            // Just STATUS command with no subcommand
            return "OK";
        }
    }
    else
    {
        // First part is not STATUS - it's the command
        QString command = parts[0].toUpper();
        if (command == "GET")
        {
            QString key = parts.size() > 1 ? parts[1] : QString();
            QMetaEnum meta = QMetaEnum::fromType<DspConfig::Key>();
            bool ok = false;
            int keyVal = meta.keyToValue(key.toLocal8Bit().constData(), &ok);
            if (!ok)
                return "ERROR|Configuration key does not exist";
            QString value = DspConfig::instance().get<QString>(
                static_cast<DspConfig::Key>(keyVal), nullptr, false);
            return "OK|" + value;
        }
        else if (command == "SET")
        {
            QString key = parts.size() > 1 ? parts[1] : QString();
            QString value = parts.size() > 2 ? parts[2] : QString();
            QMetaEnum meta = QMetaEnum::fromType<DspConfig::Key>();
            bool ok = false;
            int keyVal = meta.keyToValue(key.toLocal8Bit().constData(), &ok);
            if (!ok)
                return "ERROR|Configuration key does not exist";
            DspConfig::instance().set(static_cast<DspConfig::Key>(keyVal), value);
            return "OK";
        }
        else if (command == "SET_AND_COMMIT")
        {
            QString key = parts.size() > 1 ? parts[1] : QString();
            QString value = parts.size() > 2 ? parts[2] : QString();
            QMetaEnum meta = QMetaEnum::fromType<DspConfig::Key>();
            bool ok = false;
            int keyVal = meta.keyToValue(key.toLocal8Bit().constData(), &ok);
            if (!ok)
                return "ERROR|Configuration key does not exist";
            DspConfig::instance().set(static_cast<DspConfig::Key>(keyVal), value);
            DspConfig::instance().commit();
            DspConfig::instance().save();
            return "OK";
        }
        else if (command == "GET_ALL")
        {
            return "OK|" + DspConfig::instance().serialize();
        }
        else if (command == "GET_KEYS")
        {
            QStringList keys = DspConfig::getKeys();
            return "OK|" + keys.join(";");
        }
        else if (command == "COMMIT")
        {
            DspConfig::instance().commit();
            DspConfig::instance().save();
            return "OK";
        }
        else if (command == "GET_PRESETS")
        {
            auto* model = PresetManager::instance().presetModel();
            model->rescan();
            return "OK|" + model->getList().join(";");
        }
        else if (command == "LOAD_PRESET")
        {
            QString name = parts.size() > 1 ? parts[1] : QString();
            if (PresetManager::instance().load(name))
                return "OK";
            else
                return "ERROR|Preset does not exist";
        }
        else if (command == "SAVE_PRESET")
        {
            QString name = parts.size() > 1 ? parts[1] : QString();
            PresetManager::instance().save(name);
            return "OK";
        }
        else if (command == "DELETE_PRESET")
        {
            QString name = parts.size() > 1 ? parts[1] : QString();
            if (PresetManager::instance().remove(name))
                return "OK";
            else
                return "ERROR|Preset does not exist";
        }
        else if (command == "IS_CONNECTED")
        {
            return "OK|true";
        }
        else if (command == "APP_FLAVOR")
        {
            return "OK|" + QString(APP_FLAVOR_SHORT);
        }
        else if (command == "APP_VERSION")
        {
            return "OK|" + QString(APP_VERSION_FULL);
        }
        else if (command == "CORE_VERSION")
        {
            return "OK|" + QString(STR(JDSP_VERSION));
        }
        else if (command == "IS_PROCESSING")
        {
            return "OK|" + QString(_service->status().IsProcessing ? "true" : "false");
        }
        else if (command == "SAMPLING_RATE")
        {
            return "OK|" + QString::fromStdString(_service->status().SamplingRate);
        }
        else if (command == "AUDIO_FORMAT")
        {
            return "OK|" + QString::fromStdString(_service->status().AudioFormat);
        }
        else if (command == "RAISE_WINDOW")
        {
            emit raiseWindow();
            return "OK";
        }
        else
        {
            return "ERROR|Unknown command: " + command;
        }
    }
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
    if (!_socket->waitForConnected(2000))
    {
        Log::error("Failed to connect to IPC server: " + _socket->errorString());
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
        _socket->waitForDisconnected(1000);
    }
}

QString PipeIPCClient::sendCommand(const QString& command)
{
    if (_socket->state() != QLocalSocket::ConnectedState)
        return "ERROR|Not connected to server";

    QByteArray data = (command + "\n").toUtf8();
    _socket->write(data);
    _socket->flush();

    QElapsedTimer timer;
    timer.start();
    while (!_socket->canReadLine()) {
        const int remaining = 5000 - static_cast<int>(timer.elapsed());
        if (remaining <= 0 || !_socket->waitForReadyRead(remaining))
            return "ERROR|Timeout waiting for response";
    }
    return QString::fromUtf8(QByteArray::fromPercentEncoding(_socket->readLine().trimmed()));
}