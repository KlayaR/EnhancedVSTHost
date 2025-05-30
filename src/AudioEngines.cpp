// AudioEngines.cpp - WASAPI implementation
#include "EnhancedVSTHost.h"
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>
#include <algorithm>

#pragma comment(lib, "avrt.lib")

// WASAPI Engine Implementation
WASAPIEngine::WASAPIEngine() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
}

WASAPIEngine::~WASAPIEngine() {
    shutdown();
    CoUninitialize();
}

bool WASAPIEngine::initialize(double sampleRate, int bufferSize) {
    this->sampleRate = sampleRate;
    this->bufferSize = bufferSize;
    
    HRESULT hr;
    
    // Create device enumerator
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                         CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                         (void**)&deviceEnumerator);
    if (FAILED(hr)) {
        return false;
    }
    
    // Get default audio endpoint
    hr = deviceEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &audioDevice);
    if (FAILED(hr)) {
        return false;
    }
    
    // Activate audio client
    hr = audioDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                              nullptr, (void**)&audioClient);
    if (FAILED(hr)) {
        return false;
    }
    
    // Get mix format
    WAVEFORMATEX* pWaveformat = nullptr;
    hr = audioClient->GetMixFormat(&pWaveformat);
    if (FAILED(hr)) {
        return false;
    }
    
    // Set up our desired format
    WAVEFORMATEXTENSIBLE waveFormat = {};
    waveFormat.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    waveFormat.Format.nChannels = 2;
    waveFormat.Format.nSamplesPerSec = static_cast<DWORD>(sampleRate);
    waveFormat.Format.wBitsPerSample = 32;
    waveFormat.Format.nBlockAlign = waveFormat.Format.nChannels * waveFormat.Format.wBitsPerSample / 8;
    waveFormat.Format.nAvgBytesPerSec = waveFormat.Format.nSamplesPerSec * waveFormat.Format.nBlockAlign;
    waveFormat.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    waveFormat.Samples.wValidBitsPerSample = 32;
    waveFormat.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    waveFormat.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    
    // Calculate buffer duration (in 100-nanosecond units)
    REFERENCE_TIME requestedDuration = static_cast<REFERENCE_TIME>(
        (double)bufferSize / sampleRate * 10000000.0
    );
    
    // Initialize audio client
    hr = audioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
        requestedDuration,
        0,
        (WAVEFORMATEX*)&waveFormat,
        nullptr
    );
    
    if (FAILED(hr)) {
        // Try with the mix format if our format failed
        hr = audioClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
            requestedDuration,
            0,
            pWaveformat,
            nullptr
        );
    }
    
    CoTaskMemFree(pWaveformat);
    
    if (FAILED(hr)) {
        return false;
    }
    
    // Create event for buffer notification
    bufferEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!bufferEvent) {
        return false;
    }
    
    hr = audioClient->SetEventHandle(bufferEvent);
    if (FAILED(hr)) {
        CloseHandle(bufferEvent);
        bufferEvent = nullptr;
        return false;
    }
    
    // Get render client
    hr = audioClient->GetService(__uuidof(IAudioRenderClient), (void**)&renderClient);
    if (FAILED(hr)) {
        CloseHandle(bufferEvent);
        bufferEvent = nullptr;
        return false;
    }
    
    // Get actual buffer size
    UINT32 bufferFrameCount;
    hr = audioClient->GetBufferSize(&bufferFrameCount);
    if (FAILED(hr)) {
        CloseHandle(bufferEvent);
        bufferEvent = nullptr;
        return false;
    }
    
    this->bufferSize = bufferFrameCount;
    
    return true;
}

void WASAPIEngine::shutdown() {
    stop();
    
    if (bufferEvent) {
        CloseHandle(bufferEvent);
        bufferEvent = nullptr;
    }
    
    renderClient.Reset();
    audioClient.Reset();
    audioDevice.Reset();
    deviceEnumerator.Reset();
}

bool WASAPIEngine::start() {
    if (!audioClient) {
        return false;
    }
    
    // Start audio thread
    shouldStop = false;
    audioThread = std::thread(&WASAPIEngine::audioThreadFunc, this);
    
    // Start audio client
    HRESULT hr = audioClient->Start();
    if (FAILED(hr)) {
        shouldStop = true;
        if (audioThread.joinable()) {
            audioThread.join();
        }
        return false;
    }
    
    return true;
}

void WASAPIEngine::stop() {
    if (audioClient) {
        audioClient->Stop();
    }
    
    shouldStop = true;
    if (audioThread.joinable()) {
        audioThread.join();
    }
}

std::vector<std::wstring> WASAPIEngine::getDeviceList() const {
    std::vector<std::wstring> devices;
    
    if (!deviceEnumerator) {
        return devices;
    }
    
    IMMDeviceCollection* pCollection = nullptr;
    HRESULT hr = deviceEnumerator->EnumAudioEndpoints(
        eRender, DEVICE_STATE_ACTIVE, &pCollection
    );
    
    if (SUCCEEDED(hr)) {
        UINT count;
        hr = pCollection->GetCount(&count);
        
        if (SUCCEEDED(hr)) {
            for (UINT i = 0; i < count; i++) {
                IMMDevice* pDevice = nullptr;
                hr = pCollection->Item(i, &pDevice);
                
                if (SUCCEEDED(hr)) {
                    IPropertyStore* pProps = nullptr;
                    hr = pDevice->OpenPropertyStore(STGM_READ, &pProps);
                    
                    if (SUCCEEDED(hr)) {
                        PROPVARIANT varName;
                        PropVariantInit(&varName);
                        
                        hr = pProps->GetValue(PKEY_Device_FriendlyName, &varName);
                        if (SUCCEEDED(hr) && varName.vt == VT_LPWSTR) {
                            devices.push_back(varName.pwszVal);
                            PropVariantClear(&varName);
                        }
                        
                        pProps->Release();
                    }
                    
                    pDevice->Release();
                }
            }
        }
        
        pCollection->Release();
    }
    
    return devices;
}

bool WASAPIEngine::selectDevice(const std::wstring& deviceName) {
    if (!deviceEnumerator) {
        return false;
    }
    
    // Stop current audio if running
    bool wasRunning = (audioClient != nullptr);
    if (wasRunning) {
        stop();
    }
    
    // Find and select the device
    IMMDeviceCollection* pCollection = nullptr;
    HRESULT hr = deviceEnumerator->EnumAudioEndpoints(
        eRender, DEVICE_STATE_ACTIVE, &pCollection
    );
    
    if (SUCCEEDED(hr)) {
        UINT count;
        hr = pCollection->GetCount(&count);
        
        if (SUCCEEDED(hr)) {
            for (UINT i = 0; i < count; i++) {
                IMMDevice* pDevice = nullptr;
                hr = pCollection->Item(i, &pDevice);
                
                if (SUCCEEDED(hr)) {
                    IPropertyStore* pProps = nullptr;
                    hr = pDevice->OpenPropertyStore(STGM_READ, &pProps);
                    
                    if (SUCCEEDED(hr)) {
                        PROPVARIANT varName;
                        PropVariantInit(&varName);
                        
                        hr = pProps->GetValue(PKEY_Device_FriendlyName, &varName);
                        if (SUCCEEDED(hr) && varName.vt == VT_LPWSTR) {
                            if (deviceName == varName.pwszVal) {
                                // Found the device
                                audioDevice.Reset();
                                audioDevice = pDevice;
                                pDevice = nullptr;  // Don't release since we're keeping it
                                
                                PropVariantClear(&varName);
                                pProps->Release();
                                
                                // Re-initialize with new device
                                if (wasRunning) {
                                    initialize(sampleRate, bufferSize);
                                    start();
                                }
                                
                                pCollection->Release();
                                return true;
                            }
                            PropVariantClear(&varName);
                        }
                        
                        pProps->Release();
                    }
                    
                    if (pDevice) {
                        pDevice->Release();
                    }
                }
            }
        }
        
        pCollection->Release();
    }
    
    return false;
}

void WASAPIEngine::audioThreadFunc() {
    // Set thread priority
    DWORD taskIndex = 0;
    HANDLE hTask = AvSetMmThreadCharacteristics(L"Pro Audio", &taskIndex);
    
    if (hTask) {
        AvSetMmThreadPriority(hTask, AVRT_PRIORITY_HIGH);
    }
    
    // Prepare buffers
    const int numChannels = 2;
    std::vector<float> interleavedBuffer(bufferSize * numChannels, 0.0f);
    std::vector<std::vector<float>> inputBuffers(numChannels, std::vector<float>(bufferSize, 0.0f));
    std::vector<std::vector<float>> outputBuffers(numChannels, std::vector<float>(bufferSize, 0.0f));
    
    std::vector<const float*> inputPtrs(numChannels);
    std::vector<float*> outputPtrs(numChannels);
    
    for (int ch = 0; ch < numChannels; ++ch) {
        inputPtrs[ch] = inputBuffers[ch].data();
        outputPtrs[ch] = outputBuffers[ch].data();
    }
    
    while (!shouldStop) {
        // Wait for buffer event
        DWORD waitResult = WaitForSingleObject(bufferEvent, 2000);
        if (waitResult != WAIT_OBJECT_0) {
            if (waitResult == WAIT_TIMEOUT) {
                continue;
            }
            break;  // Error occurred
        }
        
        // Get buffer
        UINT32 numFramesAvailable;
        HRESULT hr = audioClient->GetCurrentPadding(&numFramesAvailable);
        if (FAILED(hr)) {
            continue;
        }
        
        UINT32 numFramesToWrite = bufferSize - numFramesAvailable;
        if (numFramesToWrite == 0) {
            continue;
        }
        
        BYTE* pData;
        hr = renderClient->GetBuffer(numFramesToWrite, &pData);
        if (FAILED(hr)) {
            continue;
        }
        
        // Call audio callback
        if (audioCallback) {
            // Clear output buffers
            for (auto& buffer : outputBuffers) {
                std::fill(buffer.begin(), buffer.begin() + numFramesToWrite, 0.0f);
            }
            
            // Process audio
            audioCallback(inputPtrs.data(), outputPtrs.data(), numFramesToWrite);
            
            // Interleave output
            float* pOut = reinterpret_cast<float*>(pData);
            for (UINT32 frame = 0; frame < numFramesToWrite; ++frame) {
                for (int ch = 0; ch < numChannels; ++ch) {
                    *pOut++ = outputBuffers[ch][frame];
                }
            }
        } else {
            // Fill with silence
            memset(pData, 0, numFramesToWrite * numChannels * sizeof(float));
        }
        
        // Release buffer
        hr = renderClient->ReleaseBuffer(numFramesToWrite, 0);
    }
    
    if (hTask) {
        AvRevertMmThreadCharacteristics(hTask);
    }
}