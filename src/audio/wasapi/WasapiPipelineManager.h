#ifndef WASAPIPIPELINEMANAGER_H
#define WASAPIPIPELINEMANAGER_H

#include <audioclient.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <QObject>

class WasapiPipelineManager : public QObject {
    Q_OBJECT
public:
    using AudioCallback = std::function<void(float* left_in, float* right_in,
                                             float* left_out, float* right_out,
                                             size_t frames)>;

    WasapiPipelineManager();
    WasapiPipelineManager(const WasapiPipelineManager&) = delete;
    WasapiPipelineManager& operator=(const WasapiPipelineManager&) = delete;
    ~WasapiPipelineManager();

    bool initialize();
    void shutdown();
    void setAudioCallback(AudioCallback cb);
    void setBypass(bool bypass);

    uint32_t getSampleRate() const { return sampleRate; }
    bool isProcessing() const { return !bypass && running; }

    struct DeviceInfo {
        std::string name;
        std::string id;
        bool isDefault = false;
    };

    std::vector<DeviceInfo> enumerateOutputDevices() const;

    void setOutputDeviceId(const std::string& deviceId) { currentDeviceId = deviceId; }
    bool setActiveDevice(const std::string& deviceId);

signals:
    void outputDeviceChanged(const std::string& deviceName, const std::string& deviceId);

private:
    void processingLoop();

    // WASAPI objects
    IMMDeviceEnumerator* deviceEnumerator = nullptr;
    IMMDevice* captureDevice = nullptr;
    IMMDevice* renderDevice = nullptr;
    IAudioClient* captureClient = nullptr;
    IAudioClient* renderClient = nullptr;
    IAudioCaptureClient* capture = nullptr;
    IAudioRenderClient* render = nullptr;
    bool comInitialized = false;
    bool timerPeriodActive = false;

    std::thread audioThread;
    std::atomic<bool> running = false;
    std::atomic<bool> bypass = false;
    std::atomic<uint32_t> sampleRate = 48000;

    std::string currentDeviceId;

    mutable std::mutex callbackMutex;
    AudioCallback _audioCallback;
};

#endif // WASAPIPIPELINEMANAGER_H