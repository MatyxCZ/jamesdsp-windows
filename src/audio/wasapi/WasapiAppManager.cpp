#include "WasapiAppManager.h"
#include "WasapiAudioService.h"
#include "Utils.h"

#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <QTimer>
#include <QDebug>

#pragma comment(lib, "Ole32.lib")

WasapiAppManager::WasapiAppManager(WasapiAudioService* parentService, QObject* parent)
    : _service(parentService)
{
    Q_UNUSED(parent)
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                     __uuidof(IMMDeviceEnumerator),
                     reinterpret_cast<void**>(&enumerator));

    // Poll for app changes every 500ms
    pollTimer = new QTimer(this);
    connect(pollTimer, &QTimer::timeout, this, &WasapiAppManager::refresh);
    pollTimer->start(500);

    enumerateSessions();
}

WasapiAppManager::~WasapiAppManager()
{
    if (enumerator)
        enumerator->Release();
}

void WasapiAppManager::refresh()
{
    enumerateSessions();
}

void WasapiAppManager::enumerateSessions()
{
    if (!enumerator)
        return;

    IMMDevice* device = nullptr;
    HRESULT hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr) || !device)
        return;

    IAudioSessionManager2* sessionManager = nullptr;
    hr = device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**>(&sessionManager));
    if (FAILED(hr) || !sessionManager)
    {
        device->Release();
        return;
    }

    IAudioSessionEnumerator* sessionEnum = nullptr;
    hr = sessionManager->GetSessionEnumerator(&sessionEnum);
    if (FAILED(hr) || !sessionEnum)
    {
        sessionManager->Release();
        device->Release();
        return;
    }

    // Build list of current session names
    QStringList currentNames;

    int count = 0;
    sessionEnum->GetCount(&count);

    for (int i = 0; i < count; i++)
    {
        IAudioSessionControl* session = nullptr;
        if (FAILED(sessionEnum->GetSession(i, &session)) || !session)
            continue;

        IAudioSessionControl2* session2 = nullptr;
        if (FAILED(session->QueryInterface(__uuidof(IAudioSessionControl2),
                                           reinterpret_cast<void**>(&session2))) || !session2)
        {
            session->Release();
            continue;
        }

        WaAppInfo info;
        info.id = i;

        // Get display name
        WCHAR* displayName = nullptr;
        if (SUCCEEDED(session2->GetDisplayName(&displayName)) && displayName)
        {
            int len = WideCharToMultiByte(CP_UTF8, 0, displayName, -1, nullptr, 0, nullptr, nullptr);
            if (len > 0)
            {
                char buffer[512];
                WideCharToMultiByte(CP_UTF8, 0, displayName, -1, buffer, sizeof(buffer), nullptr, nullptr);
                info.name = buffer;
            }
            CoTaskMemFree(displayName);
        }

        // Get process ID and name
        DWORD processId = 0;
        session2->GetProcessId(&processId);

        if (processId > 0)
        {
            HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, processId);
            if (hProcess)
            {
                char processName[MAX_PATH] = {0};
                DWORD size = MAX_PATH;
                if (QueryFullProcessImageNameA(hProcess, 0, processName, &size))
                {
                    char* base = strrchr(processName, '\\');
                    info.description = (base) ? base + 1 : processName;
                }
                CloseHandle(hProcess);
            }
        }

        // Get session state
        AudioSessionState state;
        session2->GetState(&state);
        info.connected = true;
        info.wants_to_play = (state == AudioSessionStateActive);
        info.media_class = "Stream/Output/Audio";
        info.format = "32 bit float";

        // Get volume and mute
        ISimpleAudioVolume* simpleVol = nullptr;
        if (SUCCEEDED(session2->QueryInterface(__uuidof(ISimpleAudioVolume),
                                               reinterpret_cast<void**>(&simpleVol))))
        {
            BOOL mute = FALSE;
            simpleVol->GetMute(&mute);
            info.mute = (mute != FALSE);
            simpleVol->GetMasterVolume(&info.volume);
            simpleVol->Release();
        }

        session2->Release();
        session->Release();

        QString qname = QString::fromStdString(info.name);
        currentNames.append(qname);

        // Check if this app is new or changed
        bool found = false;
        for (int n = 0; n < apps.length(); n++)
        {
            if (apps[n].name == info.name)
            {
                found = true;
                bool changed = (apps[n].mute != info.mute ||
                               apps[n].volume != info.volume ||
                               apps[n].wants_to_play != info.wants_to_play);
                if (changed)
                {
                    apps[n] = info;
                    emit appChanged(AppNode(info));
                }
                break;
            }
        }

        if (!found)
        {
            apps.append(info);
            emit appAdded(AppNode(info));
        }
    }

    // Remove apps that are no longer present
    for (int n = apps.length() - 1; n >= 0; n--)
    {
        if (!currentNames.contains(QString::fromStdString(apps[n].name)))
        {
            emit appRemoved(apps[n].id);
            apps.removeAt(n);
        }
    }

    sessionEnum->Release();
    sessionManager->Release();
    device->Release();
}

QList<AppNode> WasapiAppManager::activeApps() const
{
    QList<AppNode> appNodes;
    for (const auto& app : apps)
    {
        appNodes.append(AppNode(app));
    }
    return appNodes;
}

void WasapiAppManager::handleSettingsUpdate(const AppConfig::Key& key, const QVariant& value)
{
    Q_UNUSED(key)
    Q_UNUSED(value)
}