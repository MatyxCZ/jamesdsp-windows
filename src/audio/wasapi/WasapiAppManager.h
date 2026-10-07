#ifndef WASAPIAPPMANAGER_H
#define WASAPIAPPMANAGER_H

#include "config/AppConfig.h"
#include "AppNode.h"
#include "IAppManager.h"

#include <audiopolicy.h>
#include <mmdeviceapi.h>

#include <QTimer>
#include <QObject>

#include <vector>

class WasapiAudioService;

class WasapiAppManager : public IAppManager
{
    Q_OBJECT
public:
    explicit WasapiAppManager(WasapiAudioService* parentService, QObject* parent = nullptr);
    ~WasapiAppManager();

    QList<AppNode> activeApps() const override;

public slots:
    void handleSettingsUpdate(const AppConfig::Key&, const QVariant&);
    void refresh();

private:
    void enumerateSessions();

    WasapiAudioService* _service = nullptr;
    QList<WaAppInfo> apps;
    IMMDeviceEnumerator* enumerator = nullptr;
    QTimer* pollTimer = nullptr;
};

#endif // WASAPIAPPMANAGER_H