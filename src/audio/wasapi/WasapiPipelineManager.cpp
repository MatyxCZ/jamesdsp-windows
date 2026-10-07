#include "WasapiPipelineManager.h"
#include "Utils.h"

#include <atomic>
#include <cstring>
#include <algorithm>
#include <functiondiscoverykeys_devpkey.h>
#include <xmmintrin.h>
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

WasapiPipelineManager::WasapiPipelineManager()
{
}

WasapiPipelineManager::~WasapiPipelineManager()
{
    shutdown();
}

bool WasapiPipelineManager::initialize()
{
    HRESULT hr;

    // Initialize COM
    hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    comInitialized = SUCCEEDED(hr);
    if (FAILED(hr))
    {
        if (hr != RPC_E_CHANGED_MODE)
        {
            util::error("CoInitializeEx failed: " + std::to_string(hr));
            return false;
        }
    }

    // Create device enumerator
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator),
                          reinterpret_cast<void**>(&deviceEnumerator));
    if (FAILED(hr))
    {
        util::error("Failed to create device enumerator: " + std::to_string(hr));
        return false;
    }

    // Get default render device (we capture from this via loopback)
    hr = deviceEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &captureDevice);
    if (FAILED(hr) || !captureDevice)
    {
        util::error("Failed to get default render device: " + std::to_string(hr));
        return false;
    }


    // Create capture client (loopback on render device)
    hr = captureDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                 reinterpret_cast<void**>(&captureClient));
    if (FAILED(hr) || !captureClient)
    {
        util::error("Failed to activate capture IAudioClient: " + std::to_string(hr));
        return false;
    }

    // Get the mix format (device's native format)
    WAVEFORMATEX* mixFormat = nullptr;
    hr = captureClient->GetMixFormat(&mixFormat);
    if (FAILED(hr) || !mixFormat)
    {
        util::error("Failed to get mix format: " + std::to_string(hr));
        return false;
    }

    sampleRate = mixFormat->nSamplesPerSec;
    WAVEFORMATEX streamFormat = {};
    streamFormat.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    streamFormat.nChannels = 2;
    streamFormat.nSamplesPerSec = sampleRate;
    streamFormat.wBitsPerSample = 32;
    streamFormat.nBlockAlign = 2 * sizeof(float);
    streamFormat.nAvgBytesPerSec = streamFormat.nSamplesPerSec * streamFormat.nBlockAlign;

    // Initialize capture client in loopback mode (shared)
    REFERENCE_TIME hnsRequestedDuration = 0;
    hr = captureClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                   AUDCLNT_STREAMFLAGS_LOOPBACK |
                                   AUDCLNT_STREAMFLAGS_NOPERSIST |
                                   AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                   AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                   hnsRequestedDuration,
                                   0,
                                   &streamFormat,
                                   nullptr);
    CoTaskMemFree(mixFormat);

    if (FAILED(hr))
    {
        util::error("Failed to initialize capture client: " + std::to_string(hr));
        return false;
    }

    // Get the capture interface
    hr = captureClient->GetService(__uuidof(IAudioCaptureClient),
                                   reinterpret_cast<void**>(&capture));
    if (FAILED(hr) || !capture)
    {
        util::error("Failed to get IAudioCaptureClient: " + std::to_string(hr));
        return false;
    }

    // Render to a separate endpoint. Loopback capture and render on the same
    // endpoint creates an audio feedback loop and leaves the source audible.
    if (currentDeviceId.empty())
    {
        IMMDevice* defaultRender = nullptr;
        hr = deviceEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &defaultRender);
        if (SUCCEEDED(hr) && defaultRender)
        {
            LPWSTR defaultId = nullptr;
            if (SUCCEEDED(defaultRender->GetId(&defaultId)) && defaultId)
            {
                int length = WideCharToMultiByte(CP_UTF8, 0, defaultId, -1, nullptr, 0, nullptr, nullptr);
                if (length > 0)
                {
                    std::string id(static_cast<size_t>(length), '\0');
                    WideCharToMultiByte(CP_UTF8, 0, defaultId, -1, id.data(), length, nullptr, nullptr);
                    currentDeviceId = id.c_str();
                }
                CoTaskMemFree(defaultId);
            }
            defaultRender->Release();
        }
    }

    int wideLength = MultiByteToWideChar(CP_UTF8, 0, currentDeviceId.c_str(), -1, nullptr, 0);
    std::wstring outputDeviceId(static_cast<size_t>(wideLength), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, currentDeviceId.c_str(), -1,
                        outputDeviceId.data(), wideLength);
    hr = deviceEnumerator->GetDevice(outputDeviceId.c_str(), &renderDevice);
    if (FAILED(hr) || !renderDevice)
    {
        util::error("Selected WASAPI render device could not be opened: " + std::to_string(hr));
        return false;
    }

    LPWSTR captureId = nullptr;
    LPWSTR renderId = nullptr;
    const bool sameEndpoint =
        SUCCEEDED(captureDevice->GetId(&captureId)) &&
        SUCCEEDED(renderDevice->GetId(&renderId)) &&
        wcscmp(captureId, renderId) == 0;
    CoTaskMemFree(captureId);
    CoTaskMemFree(renderId);
    if (sameEndpoint)
    {
        util::error("WASAPI loopback capture and render endpoints must be different. "
                    "Route Windows audio to a virtual cable and choose physical output here.");
        return false;
    }

    // The render endpoint has its own reference from IMMDeviceEnumerator::GetDevice.

    hr = renderDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(&renderClient));
    if (FAILED(hr) || !renderClient)
    {
        util::error("Failed to activate render IAudioClient: " + std::to_string(hr));
        return false;
    }

    // Both clients expose stereo float at the capture rate; WASAPI converts
    // the render stream to the physical endpoint's native rate and layout.
    hr = renderClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                  AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                  AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                  0,
                                  0,
                                  &streamFormat,
                                  nullptr);

    if (FAILED(hr))
    {
        util::error("Failed to initialize render client: " + std::to_string(hr));
        return false;
    }

    // Get the render interface
    hr = renderClient->GetService(__uuidof(IAudioRenderClient),
                                  reinterpret_cast<void**>(&render));
    if (FAILED(hr) || !render)
    {
        util::error("Failed to get IAudioRenderClient: " + std::to_string(hr));
        return false;
    }

    // Get buffer sizes
    UINT32 captureBufferSize = 0;
    captureClient->GetBufferSize(&captureBufferSize);
    UINT32 renderBufferSize = 0;
    renderClient->GetBufferSize(&renderBufferSize);

    util::debug("WASAPI initialized: sample rate=" + std::to_string(sampleRate) +
                ", capture buffer=" + std::to_string(captureBufferSize) +
                ", render buffer=" + std::to_string(renderBufferSize));

    // Fill the render buffer with silence initially to prevent pops
    BYTE* silenceBytes = nullptr;
    UINT32 padding = 0;
    hr = render->GetBuffer(renderBufferSize, &silenceBytes);
    if (SUCCEEDED(hr) && silenceBytes)
    {
        memset(silenceBytes, 0, renderBufferSize * sizeof(float) * 2);
        render->ReleaseBuffer(renderBufferSize, 0);
    }

    // Start capture
    hr = captureClient->Start();
    if (FAILED(hr))
    {
        util::error("Failed to start capture client: " + std::to_string(hr));
        return false;
    }

    // Start render
    hr = renderClient->Start();
    if (FAILED(hr))
    {
        util::error("Failed to start render client: " + std::to_string(hr));
        captureClient->Stop();
        return false;
    }

    // Set high timer resolution for lower latency
    timerPeriodActive = timeBeginPeriod(1) == TIMERR_NOERROR;

    // Start processing thread
    running = true;
    audioThread = std::thread(&WasapiPipelineManager::processingLoop, this);

    return true;
}

void WasapiPipelineManager::shutdown()
{
    running = false;
    if (audioThread.joinable())
        audioThread.join();

    if (timerPeriodActive) {
        timeEndPeriod(1);
        timerPeriodActive = false;
    }

    if (captureClient)
    {
        captureClient->Stop();
        captureClient->Release();
        captureClient = nullptr;
    }

    if (renderClient)
    {
        renderClient->Stop();
        renderClient->Release();
        renderClient = nullptr;
    }

    if (capture)
    {
        capture->Release();
        capture = nullptr;
    }

    if (render)
    {
        render->Release();
        render = nullptr;
    }

    if (captureDevice)
    {
        captureDevice->Release();
        captureDevice = nullptr;
    }

    if (renderDevice)
    {
        renderDevice->Release();
        renderDevice = nullptr;
    }

    if (deviceEnumerator)
    {
        deviceEnumerator->Release();
        deviceEnumerator = nullptr;
    }

    if (comInitialized) {
        CoUninitialize();
        comInitialized = false;
    }
}

void WasapiPipelineManager::setAudioCallback(AudioCallback cb)
{
    std::lock_guard<std::mutex> lock(callbackMutex);
    _audioCallback = std::move(cb);
}

void WasapiPipelineManager::setBypass(bool bypass)
{
    this->bypass = bypass;
}

void WasapiPipelineManager::processingLoop()
{
    // Set real-time priority for this thread
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    // IIR tails can enter the subnormal range during silence and exhaust the
    // real-time budget. Flush only on this worker; restore its policy on exit.
    const unsigned int previousMxcsr = _mm_getcsr();
    constexpr unsigned int flushToZero = 1u << 15;
    constexpr unsigned int denormalsAreZero = 1u << 6;
    _mm_setcsr(previousMxcsr | flushToZero | denormalsAreZero);
    HRESULT hr;
    UINT32 captureBufferSize = 0;
    UINT32 renderBufferSize = 0;
    captureClient->GetBufferSize(&captureBufferSize);
    renderClient->GetBufferSize(&renderBufferSize);
    std::vector<float> leftIn(captureBufferSize);
    std::vector<float> rightIn(captureBufferSize);
    std::vector<float> leftOut(captureBufferSize);
    std::vector<float> rightOut(captureBufferSize);

    UINT32 pendingFrames = 0;
    UINT32 pendingOffset = 0;

    while (running)
    {
        // Retain one processed packet until every frame has been rendered.
        // Capture packets and render periods need not have the same size.
        if (pendingFrames == 0)
        {
            UINT32 packetFrames = 0;
            hr = capture->GetNextPacketSize(&packetFrames);
            if (FAILED(hr) || packetFrames == 0)
            {
                Sleep(1);
                continue;
            }

            BYTE* captureBytes = nullptr;
            UINT32 capturedFrames = 0;
            DWORD captureFlags = 0;
            hr = capture->GetBuffer(&captureBytes, &capturedFrames, &captureFlags,
                                    nullptr, nullptr);
            if (hr == AUDCLNT_S_BUFFER_EMPTY || FAILED(hr))
            {
                Sleep(1);
                continue;
            }
            if (capturedFrames == 0)
            {
                capture->ReleaseBuffer(0);
                continue;
            }
            if (capturedFrames > captureBufferSize)
            {
                capture->ReleaseBuffer(capturedFrames);
                util::error("WASAPI capture packet exceeds the capture buffer");
                break;
            }

            const bool silent = (captureFlags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
            const float* source = reinterpret_cast<const float*>(captureBytes);
            {
                std::lock_guard<std::mutex> lock(callbackMutex);
                if (!bypass && _audioCallback)
                {
                    for (UINT32 i = 0; i < capturedFrames; ++i)
                    {
                        leftIn[i] = silent ? 0.0f : source[i * 2];
                        rightIn[i] = silent ? 0.0f : source[i * 2 + 1];
                    }
                    _audioCallback(leftIn.data(), rightIn.data(),
                                   leftOut.data(), rightOut.data(), capturedFrames);
                }
                else
                {
                    for (UINT32 i = 0; i < capturedFrames; ++i)
                    {
                        leftOut[i] = silent ? 0.0f : source[i * 2];
                        rightOut[i] = silent ? 0.0f : source[i * 2 + 1];
                    }
                }
            }
            capture->ReleaseBuffer(capturedFrames);
            pendingFrames = capturedFrames;
            pendingOffset = 0;
        }

        UINT32 renderPadding = 0;
        hr = renderClient->GetCurrentPadding(&renderPadding);
        if (FAILED(hr) || renderPadding >= renderBufferSize)
        {
            Sleep(1);
            continue;
        }
        const UINT32 framesToWrite =
            std::min(pendingFrames, renderBufferSize - renderPadding);
        BYTE* renderBytes = nullptr;
        hr = render->GetBuffer(framesToWrite, &renderBytes);
        if (FAILED(hr))
        {
            // No packet is discarded or processed twice on a transient failure.
            Sleep(1);
            continue;
        }
        float* renderData = reinterpret_cast<float*>(renderBytes);
        for (UINT32 i = 0; i < framesToWrite; ++i)
        {
            renderData[i * 2] = leftOut[pendingOffset + i];
            renderData[i * 2 + 1] = rightOut[pendingOffset + i];
        }
        hr = render->ReleaseBuffer(framesToWrite, 0);
        if (FAILED(hr))
        {
            util::debug("Failed to release render buffer: " + std::to_string(hr));
            break;
        }
        else
        {
            pendingOffset += framesToWrite;
            pendingFrames -= framesToWrite;
        }
    }
    running = false;
    _mm_setcsr(previousMxcsr);

    util::debug("WASAPI processing thread exited");
}

std::vector<WasapiPipelineManager::DeviceInfo> WasapiPipelineManager::enumerateOutputDevices() const
{
    std::vector<DeviceInfo> devices;

    if (!deviceEnumerator)
        return devices;

    IMMDeviceCollection* deviceCollection = nullptr;
    HRESULT hr = deviceEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &deviceCollection);
    if (FAILED(hr) || !deviceCollection)
        return devices;

    UINT count = 0;
    deviceCollection->GetCount(&count);

    for (UINT i = 0; i < count; i++)
    {
        IMMDevice* device = nullptr;
        hr = deviceCollection->Item(i, &device);
        if (FAILED(hr) || !device)
            continue;

        DeviceInfo info;

        // Get device ID
        LPWSTR id = nullptr;
        if (SUCCEEDED(device->GetId(&id)))
        {
            int len = WideCharToMultiByte(CP_UTF8, 0, id, -1, nullptr, 0, nullptr, nullptr);
            if (len > 0)
            {
                char buffer[512];
                WideCharToMultiByte(CP_UTF8, 0, id, -1, buffer, sizeof(buffer), nullptr, nullptr);
                info.id = buffer;
            }
            CoTaskMemFree(id);
        }

        // Get friendly name
        IPropertyStore* props = nullptr;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props)))
        {
            PROPVARIANT nameVar;
            PropVariantInit(&nameVar);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &nameVar)))
            {
                if (nameVar.vt == VT_LPWSTR)
                {
                    int len = WideCharToMultiByte(CP_UTF8, 0, nameVar.pwszVal, -1, nullptr, 0, nullptr, nullptr);
                    if (len > 0)
                    {
                        char buffer[512];
                        WideCharToMultiByte(CP_UTF8, 0, nameVar.pwszVal, -1, buffer, sizeof(buffer), nullptr, nullptr);
                        info.name = buffer;
                    }
                }
            }
            PropVariantClear(&nameVar);
            props->Release();
        }

        // Check if this is the default device
        IMMDevice* defaultDev = nullptr;
        if (SUCCEEDED(deviceEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &defaultDev)))
        {
            LPWSTR defaultId = nullptr;
            if (SUCCEEDED(defaultDev->GetId(&defaultId)))
            {
                // Convert defaultId to string for comparison (id was already freed)
                int dlen = WideCharToMultiByte(CP_UTF8, 0, defaultId, -1, nullptr, 0, nullptr, nullptr);
                if (dlen > 0)
                {
                    char dbuffer[512];
                    WideCharToMultiByte(CP_UTF8, 0, defaultId, -1, dbuffer, sizeof(dbuffer), nullptr, nullptr);
                    info.isDefault = (info.id == dbuffer);
                }
                CoTaskMemFree(defaultId);
            }
            defaultDev->Release();
        }

        devices.push_back(info);
        device->Release();
    }

    deviceCollection->Release();
    return devices;
}

bool WasapiPipelineManager::setActiveDevice(const std::string& deviceId)
{
    shutdown();
    currentDeviceId = deviceId;
    return initialize();
}