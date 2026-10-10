#include "SingleInstanceMonitor.h"

#include <QCoreApplication>
#include <QTimer>
#include "Log.h"
#ifdef _WIN32
#include "PipeIPCHandler.h"
#else
#include "dbus/ServerAdaptor.h"
#include "dbus/ClientProxy.h"
#endif

SingleInstanceMonitor::SingleInstanceMonitor(QObject* parent) : QObject(parent)
{
#ifdef _WIN32
    // Try to create a named mutex for single-instance enforcement
    _mutex = CreateMutexA(nullptr, FALSE, "Global\\jamesdsp_singleton");
    if (_mutex == nullptr)
    {
        Log::error("Failed to create single-instance mutex");
        _registered = false;
        return;
    }

    // Check if the mutex was already owned (another instance is running)
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        _registered = false;
        Log::warning("Mutex already owned. Another instance is running");
        return;
    }

    _registered = true;
    Log::information("Service registration successful (single-instance mutex acquired)");
#else
    QDBusConnection connection = QDBusConnection::sessionBus();

    _dbusAdapter = new GuiAdaptor(this);
    _registered  = connection.registerObject("/jdsp4linux/gui", this) &&
            connection.registerService("me.timschneeberger.jdsp4linux");
#endif
}

SingleInstanceMonitor::~SingleInstanceMonitor()
{
#ifndef _WIN32
    _dbusAdapter->deleteLater();
#endif
#ifdef _WIN32
    if (_mutex)
    {
        CloseHandle(_mutex);
    }
#endif
}

bool SingleInstanceMonitor::isServiceReady()
{
    if (_registered)
    {
        Log::information("Service registration successful");
        return true;
    }
    else
    {
        Log::warning("Service registration failed. Another instance is already running");
        return false;
    }
}

bool SingleInstanceMonitor::handover()
{
    Log::information("Attempting to switch to existing instance...");

#ifdef _WIN32
    // Connect to the named pipe to ask the running instance to raise its window
    PipeIPCClient client;
    if (!client.connectToServer())
    {
        Log::error("Unable to connect to other instance via named pipe. Continuing anyway...");
        return false;
    }

    QString response = client.sendCommand("RAISE_WINDOW");
    if (response != "OK")
    {
        Log::error("Other instance returned error. Continuing anyway...");
        return false;
    }

    Log::information("Success! Waiting for event loop to exit...");
    Log::console("\n" + QObject::tr("Another instance of JamesDSP is already active and has been put in the foreground"), true);

    QTimer::singleShot(0, qApp, &QCoreApplication::quit);
    return true;
#else
    auto m_dbInterface = new me::timschneeberger::jdsp4linux::Gui("me.timschneeberger.jdsp4linux", "/jdsp4linux/gui",
                                                          QDBusConnection::sessionBus(), this);
    if (!m_dbInterface->isValid())
    {
        Log::error("Unable to connect to other DBus instance. Continuing anyway...");
    }
    else
    {
        QDBusPendingReply<> msg = m_dbInterface->raiseWindow();
        msg.waitForFinished();

        if (msg.isError())
        {
            Log::error("Other instance returned (invalid) error message. Continuing anyway...");
        }
        else
        {
            Log::information("Success! Waiting for event loop to exit...");
            Log::console("\n" + QObject::tr("Another instance of JamesDSP is already active and has been put in the foreground."), true);

            QTimer::singleShot(0, qApp, &QCoreApplication::quit);
            return true;
        }
    }

    return false;
#endif
}