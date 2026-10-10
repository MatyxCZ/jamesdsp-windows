#ifndef WASAPIAUDIOSERVICE_H
#define WASAPIAUDIOSERVICE_H

#include "IAudioService.h"
#include "config/AppConfig.h"

#include "WasapiAppManager.h"
#include "WasapiPipelineManager.h"

#include <memory>
#include <array>
#include <mutex>
#include <QObject>

typedef struct dspsys JamesDSPLib;

class WasapiAudioService : public IAudioService
{
    Q_OBJECT
    Q_INTERFACES(IAudioService)

public:
    WasapiAudioService();
    ~WasapiAudioService();

public slots:
    void onAppConfigUpdated(const AppConfig::Key& key, const QVariant& value);

    void update(DspConfig* config) override;
    void reloadService() override;

    DspHost* host() override;
    IAppManager* appManager() override;
    std::vector<IOutputDevice> sinkDevices() override;
    std::vector<IOutputDevice> outputDevices() override;
    DspStatus status() override;

    // Called by WasapiAppManager to get the pipeline
    WasapiPipelineManager* pipelineManager() { return pipeline.get(); }

signals:
    void pipelineFailed();

private:
    void setupDsp();
    void restartPipeline(const QString& deviceId);
    void reportOutputDevice(const QString& deviceId);
    void processAudio(float* left_in, float* right_in,
                      float* left_out, float* right_out, size_t frames);

    std::unique_ptr<WasapiPipelineManager> pipeline;
    std::unique_ptr<WasapiAppManager> appMgr;
    QString activeOutputDeviceId;

    JamesDSPLib* dsp = nullptr;
    DspHost* _host = nullptr;
    std::mutex dspMutex;
    static constexpr size_t dspBlockFrames = 128;
    std::array<float, dspBlockFrames> dspInputLeft{};
    std::array<float, dspBlockFrames> dspInputRight{};
    std::array<float, dspBlockFrames> dspOutputLeft{};
    std::array<float, dspBlockFrames> dspOutputRight{};
    size_t dspFrameOffset = 0;

    uint32_t sampleRate = 48000;
    bool comInitialized = false;
    bool bypass = false;
};

#endif // WASAPIAUDIOSERVICE_H