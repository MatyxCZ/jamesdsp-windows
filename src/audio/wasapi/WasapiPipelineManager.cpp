#include "WasapiPipelineManager.h"
#include "Utils.h"

#include <atomic>
#include <cstring>
#include <algorithm>
#include <cstdio>
#include <system_error>
#include <functiondiscoverykeys_devpkey.h>
#include <xmmintrin.h>
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

namespace {
struct ReleaseCom {
    template <typename T> void operator()(T* object) const { object->Release(); }
};
template <typename T> using ScopedCom = std::unique_ptr<T, ReleaseCom>;

std::string utf8(const wchar_t* text)
{
    if (!text)
        return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 0)
        return {};
    std::string result(static_cast<size_t>(length), '\0');
    if (!WideCharToMultiByte(CP_UTF8, 0, text, -1, &result[0], length, nullptr, nullptr))
        return {};
    result.pop_back();
    return result;
}

std::string hresultDetail(HRESULT hr)
{
    char hex[11] = {};
    std::snprintf(hex, sizeof(hex), "0x%08lX", static_cast<unsigned long>(hr));
    return " (HRESULT " + std::string(hex) + ", " + std::to_string(hr) + ")";
}

std::string endpointContext(const char* role, const wasapi::DeviceInfo& device)
{
    return std::string(role) + " endpoint '" + device.name + "' [ID: " + device.id + "]";
}
}

WasapiPipelineManager::WasapiPipelineManager()
{
}

WasapiPipelineManager::~WasapiPipelineManager()
{
    shutdown();
}

bool WasapiPipelineManager::setError(const std::string& detail)
{
    errorDetail = detail;
    util::error(detail);
    return false;
}

bool WasapiPipelineManager::ensureDeviceEnumerator()
{
    if (deviceEnumerator)
        return true;
    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    comInitialized = SUCCEEDED(comHr);
    if (FAILED(comHr) && comHr != RPC_E_CHANGED_MODE)
        return setError("Cannot initialize COM for WASAPI device enumeration" + hresultDetail(comHr));

    const HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                        __uuidof(IMMDeviceEnumerator),
                                        reinterpret_cast<void**>(&deviceEnumerator));
    if (FAILED(hr) || !deviceEnumerator) {
        if (deviceEnumerator) {
            deviceEnumerator->Release();
            deviceEnumerator = nullptr;
        }
        if (comInitialized) {
            CoUninitialize();
            comInitialized = false;
        }
        return setError("Cannot create the WASAPI device enumerator" +
                        hresultDetail(FAILED(hr) ? hr : E_POINTER));
    }
    return true;
}

bool WasapiPipelineManager::openSelectedDevice(const DeviceInfo& selected, const char* role,
                                              IMMDevice** device)
{
    const auto context = endpointContext(role, selected);
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                           selected.id.c_str(), -1, nullptr, 0);
    if (length <= 0)
        return setError("Invalid saved ID for " + context + hresultDetail(HRESULT_FROM_WIN32(GetLastError())));
    std::wstring id(static_cast<size_t>(length), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, selected.id.c_str(), -1, &id[0], length))
        return setError("Cannot decode saved ID for " + context + hresultDetail(HRESULT_FROM_WIN32(GetLastError())));
    HRESULT hr = deviceEnumerator->GetDevice(id.c_str(), device);
    if (FAILED(hr) || !*device)
        return setError("Cannot open " + context + hresultDetail(FAILED(hr) ? hr : E_POINTER) +
                        ". Reconnect it or select the endpoint again in Audio settings.");
    DWORD state = 0;
    hr = (*device)->GetState(&state);
    if (FAILED(hr))
        return setError("Cannot query the state of " + context + hresultDetail(hr));
    if (!(state & DEVICE_STATE_ACTIVE))
        return setError("The selected " + context + " is not active. Reconnect or enable it, then relink.");
    return true;
}

bool WasapiPipelineManager::initialize()
{
    // Failed starts and previous runs must release every client before reuse.
    shutdown();
    errorDetail.clear();
    if (!initializeEndpoints()) {
        shutdown();
        return false;
    }
    return true;
}

bool WasapiPipelineManager::initializeEndpoints()
{
    if (!ensureDeviceEnumerator())
        return false;

    std::vector<DeviceInfo> devices;
    if (!collectActiveDevices(devices))
        return false;
    const auto input = wasapi::resolveDeviceSelection(devices, savedInput.id, savedInput.name);
    const auto output = wasapi::resolveDeviceSelection(devices, savedOutput.id, savedOutput.name);
    const auto selectionError = [this](const wasapi::DeviceSelection& selection,
                                       const DeviceInfo& saved, const char* role) {
        switch (selection.status) {
        case wasapi::SelectionStatus::Empty:
            return setError("No explicit " + std::string(role) + " endpoint is saved. Select CABLE Input "
                            "(a playback endpoint) as the loopback input and a different physical output "
                            "in Audio settings. Windows default devices are not used.");
        case wasapi::SelectionStatus::Ambiguous:
            return setError("The saved " + endpointContext(role, saved) +
                            " is unavailable and multiple active endpoints have its exact friendly name. "
                            "Select the intended endpoint explicitly in Audio settings.");
        case wasapi::SelectionStatus::Unavailable:
            return setError("The saved " + endpointContext(role, saved) +
                            " is unavailable; no unique active endpoint with its saved friendly name exists. "
                            "Reconnect or enable it, or select it again in Audio settings.");
        case wasapi::SelectionStatus::Resolved:
            return true;
        }
        return false;
    };
    if (!selectionError(input, savedInput, "loopback input") ||
        !selectionError(output, savedOutput, "render output"))
        return false;
    const DeviceInfo& selectedInput = *input.device;
    const DeviceInfo& selectedOutput = *output.device;
    if (selectedInput.id == selectedOutput.id)
        return setError("Feedback prevented: " + endpointContext("loopback input", selectedInput) +
                        " is also the render output. Select CABLE Input as input and a different physical "
                        "output in Audio settings. Windows defaults were not changed.");
    if (!openSelectedDevice(selectedInput, "loopback input", &captureDevice) ||
        !openSelectedDevice(selectedOutput, "render output", &renderDevice))
        return false;

    const std::string inputContext = endpointContext("loopback input", selectedInput);
    const std::string outputContext = endpointContext("render output", selectedOutput);
    HRESULT hr = captureDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                         reinterpret_cast<void**>(&captureClient));
    if (FAILED(hr) || !captureClient)
        return setError("Cannot activate " + inputContext + hresultDetail(FAILED(hr) ? hr : E_POINTER));

    WAVEFORMATEX* mixFormat = nullptr;
    hr = captureClient->GetMixFormat(&mixFormat);
    if (FAILED(hr) || !mixFormat) {
        CoTaskMemFree(mixFormat);
        return setError("Cannot get the mix format for " + inputContext +
                        hresultDetail(FAILED(hr) ? hr : E_POINTER));
    }
    sampleRate = mixFormat->nSamplesPerSec;
    CoTaskMemFree(mixFormat);
    WAVEFORMATEX streamFormat = {};
    streamFormat.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    streamFormat.nChannels = 2;
    streamFormat.nSamplesPerSec = sampleRate;
    streamFormat.wBitsPerSample = 32;
    streamFormat.nBlockAlign = 2 * sizeof(float);
    streamFormat.nAvgBytesPerSec = streamFormat.nSamplesPerSec * streamFormat.nBlockAlign;

    hr = captureClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                   AUDCLNT_STREAMFLAGS_LOOPBACK |
                                   AUDCLNT_STREAMFLAGS_NOPERSIST |
                                   AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                   AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                   0, 0, &streamFormat, nullptr);
    if (FAILED(hr))
        return setError("Cannot initialize " + inputContext + hresultDetail(hr));
    hr = captureClient->GetService(__uuidof(IAudioCaptureClient),
                                   reinterpret_cast<void**>(&capture));
    if (FAILED(hr) || !capture)
        return setError("Cannot get the capture interface for " + inputContext +
                        hresultDetail(FAILED(hr) ? hr : E_POINTER));

    hr = renderDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(&renderClient));
    if (FAILED(hr) || !renderClient)
        return setError("Cannot activate " + outputContext + hresultDetail(FAILED(hr) ? hr : E_POINTER));
    // Both streams remain stereo float at the input rate; WASAPI converts output.
    hr = renderClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                  AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                  AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                  0, 0, &streamFormat, nullptr);
    if (FAILED(hr))
        return setError("Cannot initialize " + outputContext + hresultDetail(hr));
    hr = renderClient->GetService(__uuidof(IAudioRenderClient),
                                  reinterpret_cast<void**>(&render));
    if (FAILED(hr) || !render)
        return setError("Cannot get the render interface for " + outputContext +
                        hresultDetail(FAILED(hr) ? hr : E_POINTER));

    UINT32 captureBufferSize = 0;
    hr = captureClient->GetBufferSize(&captureBufferSize);
    if (FAILED(hr) || captureBufferSize == 0)
        return setError("Cannot get a usable buffer for " + inputContext +
                        hresultDetail(FAILED(hr) ? hr : E_UNEXPECTED));
    UINT32 renderBufferSize = 0;
    hr = renderClient->GetBufferSize(&renderBufferSize);
    if (FAILED(hr) || renderBufferSize == 0)
        return setError("Cannot get a usable buffer for " + outputContext +
                        hresultDetail(FAILED(hr) ? hr : E_UNEXPECTED));

    BYTE* silenceBytes = nullptr;
    hr = render->GetBuffer(renderBufferSize, &silenceBytes);
    if (FAILED(hr))
        return setError("Cannot prime the buffer for " + outputContext + hresultDetail(hr));
    hr = render->ReleaseBuffer(renderBufferSize, AUDCLNT_BUFFERFLAGS_SILENT);
    if (FAILED(hr))
        return setError("Cannot release the initial buffer for " + outputContext + hresultDetail(hr));
    hr = captureClient->Start();
    if (FAILED(hr))
        return setError("Cannot start " + inputContext + hresultDetail(hr));
    hr = renderClient->Start();
    if (FAILED(hr))
        return setError("Cannot start " + outputContext + hresultDetail(hr));

    timerPeriodActive = timeBeginPeriod(1) == TIMERR_NOERROR;
    running = true;
    try {
        audioThread = std::thread(&WasapiPipelineManager::processingLoop, this);
    } catch (const std::system_error& error) {
        return setError("Cannot start the WASAPI processing worker: " + std::string(error.what()));
    }
    activeInput = selectedInput;
    activeOutput = selectedOutput;
    util::debug("WASAPI initialized: " + inputContext + " -> " + outputContext +
                ", sample rate=" + std::to_string(sampleRate) +
                ", capture buffer=" + std::to_string(captureBufferSize) +
                ", render buffer=" + std::to_string(renderBufferSize));
    return true;
}

void WasapiPipelineManager::shutdown()
{
    running = false;
    if (audioThread.joinable())
        audioThread.join();
    activeInput = {};
    activeOutput = {};

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

bool WasapiPipelineManager::collectActiveDevices(std::vector<DeviceInfo>& devices)
{
    IMMDeviceCollection* rawCollection = nullptr;
    HRESULT hr = deviceEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &rawCollection);
    ScopedCom<IMMDeviceCollection> collection(rawCollection);
    if (FAILED(hr) || !collection)
        return setError("Cannot enumerate active WASAPI playback endpoints" +
                        hresultDetail(FAILED(hr) ? hr : E_POINTER));
    UINT count = 0;
    hr = collection->GetCount(&count);
    if (FAILED(hr))
        return setError("Cannot count active WASAPI playback endpoints" + hresultDetail(hr));
    devices.reserve(count);

    std::string defaultId;
    IMMDevice* rawDefault = nullptr;
    hr = deviceEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &rawDefault);
    ScopedCom<IMMDevice> defaultDevice(rawDefault);
    if (SUCCEEDED(hr) && defaultDevice) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(defaultDevice->GetId(&id)))
            defaultId = utf8(id);
        CoTaskMemFree(id);
    }

    for (UINT i = 0; i < count; ++i) {
        IMMDevice* rawDevice = nullptr;
        hr = collection->Item(i, &rawDevice);
        ScopedCom<IMMDevice> device(rawDevice);
        if (FAILED(hr) || !device)
            return setError("Cannot read active WASAPI playback endpoint " + std::to_string(i) +
                            hresultDetail(FAILED(hr) ? hr : E_POINTER));
        DeviceInfo info;
        LPWSTR id = nullptr;
        hr = device->GetId(&id);
        if (SUCCEEDED(hr))
            info.id = utf8(id);
        CoTaskMemFree(id);
        if (FAILED(hr) || info.id.empty())
            return setError("Cannot read the ID of active WASAPI playback endpoint " + std::to_string(i) +
                            hresultDetail(FAILED(hr) ? hr : E_UNEXPECTED));
        IPropertyStore* rawProperties = nullptr;
        hr = device->OpenPropertyStore(STGM_READ, &rawProperties);
        ScopedCom<IPropertyStore> properties(rawProperties);
        if (FAILED(hr) || !properties)
            return setError("Cannot read properties for playback endpoint [ID: " + info.id + "]" +
                            hresultDetail(FAILED(hr) ? hr : E_POINTER));
        PROPVARIANT name;
        PropVariantInit(&name);
        hr = properties->GetValue(PKEY_Device_FriendlyName, &name);
        if (SUCCEEDED(hr) && name.vt == VT_LPWSTR)
            info.name = utf8(name.pwszVal);
        PropVariantClear(&name);
        if (FAILED(hr) || info.name.empty())
            return setError("Cannot read the friendly name of playback endpoint [ID: " + info.id + "]" +
                            hresultDetail(FAILED(hr) ? hr : E_UNEXPECTED));
        info.isDefault = info.id == defaultId;
        devices.push_back(std::move(info));
    }
    return true;
}

std::vector<WasapiPipelineManager::DeviceInfo> WasapiPipelineManager::enumerateOutputDevices()
{
    std::vector<DeviceInfo> devices;
    if (!ensureDeviceEnumerator() || !collectActiveDevices(devices))
        devices.clear();
    return devices;
}

void WasapiPipelineManager::setDeviceSelection(const std::string& inputId, const std::string& inputName,
                                              const std::string& outputId, const std::string& outputName)
{
    savedInput.id = inputId;
    savedInput.name = inputName;
    savedOutput.id = outputId;
    savedOutput.name = outputName;
}