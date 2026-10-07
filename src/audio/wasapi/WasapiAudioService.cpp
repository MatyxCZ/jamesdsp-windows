#include "WasapiAudioService.h"
#include "WasapiDevice.h"
#include "DspHost.h"
#include "utils/Log.h"

#include <QDebug>
#include <QTimer>
#include <algorithm>

extern "C" {
#ifdef DEBUG_FPE
#include <fenv.h>
#endif
#include <JdspImpResToolbox.h>
#include <PrintfStdOutExtension.h>
#include <jdsp_header.h>
}

void receiveWasapiPrintfStdout(const char* msg, void* userdata) {
    auto* service = static_cast<WasapiAudioService*>(userdata);
    if (service != nullptr) {
        service->handleMessage(DspHost::PrintfWriteOutputBuffer, QString(msg));
    }
}

WasapiAudioService::WasapiAudioService()
{
    Log::information("============ Initializing WASAPI audio service ============");

    // Allocate and initialize the DSP
    dsp = static_cast<JamesDSPLib*>(malloc(sizeof(JamesDSPLib)));
    memset(dsp, 0, sizeof(JamesDSPLib));

    JamesDSPGlobalMemoryAllocation();
#ifdef _MSC_VER
    initialize();
#endif
    JamesDSPInit(dsp, 128, sampleRate);


    // Create and configure the pipeline before initializing the audio endpoints.
    pipeline = std::make_unique<WasapiPipelineManager>();
    pipeline->setOutputDeviceId(
        AppConfig::instance().get<QString>(AppConfig::AudioOutputDevice).toStdString());
    // Initialize WASAPI (must be called before creating DspHost, since DspHost needs correct sample rate)
    if (!pipeline->initialize())
    {
        Log::error("Failed to initialize WASAPI audio pipeline. "
                   "The DSP will not process audio but the GUI will still work.");
        QTimer::singleShot(0, this, [this] { emit pipelineFailed(); });
    }
    // Start session enumeration after COM has been initialized by the pipeline.
    appMgr = std::make_unique<WasapiAppManager>(this);

    // Update sample rate from pipeline
    sampleRate = pipeline->getSampleRate();
    setupDsp();

    // Set up the DSP host with message handler
    _host = new DspHost(dsp, [this](DspHost::Message msg, std::any value) {
        switch(msg)
        {
            case DspHost::SwitchPassthrough:
                bypass = !std::any_cast<bool>(value);
                pipeline->setBypass(bypass);
                break;
            default:
                handleMessage(msg, std::move(value));
                break;
        }
    });

    // Set up printf handler for EEL output
    setPrintfStdOutHandler(receiveWasapiPrintfStdout, this);
    // Set up the audio callback
    pipeline->setAudioCallback([this](float* left_in, float* right_in,
                                      float* left_out, float* right_out, size_t frames) {
        processAudio(left_in, right_in, left_out, right_out, frames);
    });

    // Listen for config changes
    connect(&AppConfig::instance(), &AppConfig::updated, this, &WasapiAudioService::onAppConfigUpdated);

    Log::information("WASAPI audio service initialized. Sample rate: " + QString::number(sampleRate));
}

WasapiAudioService::~WasapiAudioService()
{
    pipeline->shutdown();

    setPrintfStdOutHandler(nullptr, nullptr);

    delete _host;

#ifdef DEBUG_FPE
    feenableexcept(FE_ALL_EXCEPT & ~FE_INEXACT & ~FE_INVALID);
#endif
    JamesDSPFree(dsp);
#ifdef _MSC_VER
    destruction();
#endif
    JamesDSPGlobalMemoryDeallocation();
#ifdef DEBUG_FPE
    fedisableexcept(FE_ALL_EXCEPT & ~FE_INEXCEPT & ~FE_INVALID);
#endif

    free(dsp);
    dsp = nullptr;
}

void WasapiAudioService::setupDsp()
{
    if (!dsp)
        return;

    JamesDSPSetSampleRate(dsp, pipeline->getSampleRate(), 0);
    dspFrameOffset = 0;
    dspOutputLeft.fill(0.0f);
    dspOutputRight.fill(0.0f);
}

void WasapiAudioService::processAudio(float* left_in, float* right_in,
                                      float* left_out, float* right_out, size_t frames)
{
    std::lock_guard<std::mutex> lock(dspMutex);
    const auto rate = pipeline->getSampleRate();
    if (sampleRate != rate) {
        sampleRate = rate;
        setupDsp();
    }
#ifdef DEBUG_FPE
    feenableexcept(FE_DIVBYZERO);
#endif
    // A one-block delay preserves every input frame across packet boundaries.
    // Stable DSP quanta avoid reallocating/rebuilding convolvers on packet jitter.
    for (size_t offset = 0; offset < frames;)
    {
        const size_t count = (std::min)(dspBlockFrames - dspFrameOffset, frames - offset);
        std::copy_n(left_in + offset, count, dspInputLeft.data() + dspFrameOffset);
        std::copy_n(right_in + offset, count, dspInputRight.data() + dspFrameOffset);
        std::copy_n(dspOutputLeft.data() + dspFrameOffset, count, left_out + offset);
        std::copy_n(dspOutputRight.data() + dspFrameOffset, count, right_out + offset);
        dspFrameOffset += count;
        offset += count;
        if (dspFrameOffset == dspBlockFrames)
        {
            dsp->processFloatDeinterleaved(dsp, dspInputLeft.data(), dspInputRight.data(),
                                          dspOutputLeft.data(), dspOutputRight.data(),
                                          dspBlockFrames);
            dspFrameOffset = 0;
        }
    }
#ifdef DEBUG_FPE
    fedisableexcept(FE_DIVBYZERO);
#endif
}

void WasapiAudioService::onAppConfigUpdated(const AppConfig::Key& key, const QVariant& value)
{
    Q_UNUSED(value)

    if (key == AppConfig::AudioOutputDevice)
    {
        QString deviceId = value.toString();
        if (!deviceId.isEmpty())
        {
            if (pipeline->setActiveDevice(deviceId.toStdString()))
                emit outputDeviceChanged(deviceId, deviceId);
            else
                emit pipelineFailed();
        }
    }
    else if (key == AppConfig::AudioAppBlocklist ||
             key == AppConfig::AudioAppBlocklistInvert)
    {
        appMgr->handleSettingsUpdate(key, value);
    }
}

void WasapiAudioService::update(DspConfig* config)
{
    std::lock_guard<std::mutex> lock(dspMutex);
    _host->update(config);
}

void WasapiAudioService::reloadService()
{
    pipeline->shutdown();
    if (!pipeline->initialize())
        emit pipelineFailed();
}

DspHost* WasapiAudioService::host()
{
    return _host;
}

IAppManager* WasapiAudioService::appManager()
{
    return appMgr.get();
}

std::vector<IOutputDevice> WasapiAudioService::sinkDevices()
{
    std::vector<IOutputDevice> devices;

    auto wasapiDevices = pipeline->enumerateOutputDevices();
    for (const auto& dev : wasapiDevices)
    {
        devices.emplace_back(dev.id, dev.name);
    }

    return devices;
}

DspStatus WasapiAudioService::status()
{
    DspStatus status;
    status.AudioFormat = "32-bit floating point samples, little endian";
    status.SamplingRate = std::to_string(pipeline->getSampleRate());
    status.IsProcessing = pipeline->isProcessing();
    return status;
}