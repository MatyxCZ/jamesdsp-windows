#ifndef WASAPIPIPELINEMANAGER_H
#define WASAPIPIPELINEMANAGER_H

#include "WasapiDeviceSelection.h"

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

    using DeviceInfo = wasapi::DeviceInfo;

    // Enumeration works before initialize(), including after a failed start.
    std::vector<DeviceInfo> enumerateOutputDevices();
    void setDeviceSelection(const std::string& inputId, const std::string& inputName,
                            const std::string& outputId, const std::string& outputName);
    const DeviceInfo& resolvedInputDevice() const { return activeInput; }
    const DeviceInfo& resolvedOutputDevice() const { return activeOutput; }
    const std::string& lastError() const { return errorDetail; }

private:
    void processingLoop();
    bool initializeEndpoints();
    bool ensureDeviceEnumerator();
    bool collectActiveDevices(std::vector<DeviceInfo>& devices);
    bool openSelectedDevice(const DeviceInfo& selected, const char* role, IMMDevice** device);
    bool setError(const std::string& detail);

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

    DeviceInfo savedInput;
    DeviceInfo savedOutput;
    DeviceInfo activeInput;
    DeviceInfo activeOutput;
    std::string errorDetail;

    mutable std::mutex callbackMutex;
    AudioCallback _audioCallback;
};

#endif // WASAPIPIPELINEMANAGER_H