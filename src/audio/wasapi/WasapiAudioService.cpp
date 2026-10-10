#include "WasapiAudioService.h"
#include "DspHost.h"
#include "utils/Log.h"

#include <QDebug>
#include <QTimer>
#include <algorithm>
#include <new>

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
    if (!dsp)
        throw std::bad_alloc();

    JamesDSPGlobalMemoryAllocation();
#ifdef _MSC_VER
    initialize();
#endif


    // Keep the GUI-thread COM apartment alive while the app manager owns its
    // enumerator, including across pipeline shutdown/reinitialization.
    comInitialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    // Create and configure the pipeline before initializing the audio endpoints.
    pipeline = std::make_unique<WasapiPipelineManager>();
    const QString deviceId = AppConfig::instance().get<QString>(AppConfig::AudioOutputDevice);
    pipeline->setOutputDeviceId(deviceId.toStdString());
    // Initialize endpoints before constructing the host and attaching its callback.
    const bool initialized = pipeline->initialize();
    if (!initialized)
    {
        Log::error("Failed to initialize WASAPI audio pipeline. "
                   "The DSP will not process audio but the GUI will still work.");
        QTimer::singleShot(0, this, [this] { emit pipelineFailed(); });
    }
    // Start session enumeration after COM has been initialized by the pipeline.
    appMgr = std::make_unique<WasapiAppManager>(this);

    // Update sample rate from pipeline
    sampleRate = pipeline->getSampleRate();
    JamesDSPInit(dsp, static_cast<int>(dspBlockFrames), sampleRate);
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
    if (initialized)
    {
        activeOutputDeviceId = deviceId;
        QTimer::singleShot(0, this, [this, deviceId] { reportOutputDevice(deviceId); });
    }

    // Listen for config changes
    connect(&AppConfig::instance(), &AppConfig::updated, this, &WasapiAudioService::onAppConfigUpdated);

    Log::information("WASAPI audio service initialized. Sample rate: " + QString::number(sampleRate));
}

WasapiAudioService::~WasapiAudioService()
{
    appMgr.reset();
    pipeline->shutdown();
    if (comInitialized)
        CoUninitialize();

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

    // The Windows pthread adapter uses recursive critical sections, permitting
    // the library's nested effect locks while refreshing rate-dependent state.
    JamesDSPSetSampleRate(dsp, sampleRate, 1);
    dspFrameOffset = 0;
    dspOutputLeft.fill(0.0f);
    dspOutputRight.fill(0.0f);
}

void WasapiAudioService::processAudio(float* left_in, float* right_in,
                                      float* left_out, float* right_out, size_t frames)
{
    std::lock_guard<std::mutex> lock(dspMutex);
    // Endpoint restarts configure the sample rate before attaching this callback.
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
        restartPipeline(value.toString());
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
    restartPipeline(AppConfig::instance().get<QString>(AppConfig::AudioOutputDevice));
}

void WasapiAudioService::restartPipeline(const QString& deviceId)
{
    activeOutputDeviceId.clear();
    // Join the audio worker before acquiring the DSP lock; it may be waiting for it.
    pipeline->shutdown();
    pipeline->setAudioCallback({});
    pipeline->setOutputDeviceId(deviceId.toStdString());
    if (!pipeline->initialize())
    {
        emit pipelineFailed();
        return;
    }
    {
        std::lock_guard<std::mutex> lock(dspMutex);
        sampleRate = pipeline->getSampleRate();
        setupDsp();
        // Recreate sample-rate-dependent effects with all official cached keys.
        _host->updateFromCache();
    }
    pipeline->setAudioCallback([this](float* left_in, float* right_in,
                                     float* left_out, float* right_out, size_t frames) {
        processAudio(left_in, right_in, left_out, right_out, frames);
    });
    activeOutputDeviceId = deviceId;
    reportOutputDevice(deviceId);
}

void WasapiAudioService::reportOutputDevice(const QString& deviceId)
{
    if (deviceId.isEmpty() || deviceId != activeOutputDeviceId)
        return;
    for (const auto& device : pipeline->enumerateOutputDevices())
    {
        if (QString::fromStdString(device.id) == deviceId)
        {
            // A WASAPI render endpoint is itself the selected hardware output route.
            emit outputDeviceChanged(QString::fromStdString(device.name),
                                     QString::fromStdString(device.id),
                                     QString::fromStdString(device.id));
            return;
        }
    }
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
        IOutputDevice device(dev.id, dev.name, dev.id, dev.name);
        device.output_routes.append(Route(dev.id, dev.name));
        devices.push_back(std::move(device));
    }

    return devices;
}

std::vector<IOutputDevice> WasapiAudioService::outputDevices()
{
    return sinkDevices();
}

DspStatus WasapiAudioService::status()
{
    DspStatus status;
    status.AudioFormat = "32-bit floating point samples, little endian";
    status.SamplingRate = std::to_string(pipeline->getSampleRate());
    status.IsProcessing = pipeline->isProcessing();
    return status;
}