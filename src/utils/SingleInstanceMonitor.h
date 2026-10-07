#ifndef SINGLEINSTANCEMONITOR_H
#define SINGLEINSTANCEMONITOR_H

#include <QObject>

#ifdef _WIN32
#include <windows.h>
#else
class GuiAdaptor;
#endif

class SingleInstanceMonitor : public QObject
{
    Q_OBJECT
public:
    SingleInstanceMonitor(QObject* parent = nullptr);
    ~SingleInstanceMonitor();

    bool handover();
    bool isServiceReady();

signals:
    void raiseWindow();

private:
#ifdef _WIN32
    HANDLE _mutex = nullptr;
    bool _registered = false;
#else
    GuiAdaptor* _dbusAdapter;
    bool _registered;
#endif
};

#endif // SINGLEINSTANCEMONITOR_H