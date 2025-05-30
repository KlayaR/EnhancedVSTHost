// EnhancedVSTHost.h - Main header file
#pragma once

#include <windows.h>
#include <memory>
#include <vector>
#include <string>
#include <unordered_map>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <exception>
#include <filesystem>

// Audio APIs
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>

// VST SDK includes
#include "public.sdk/source/vst/vst3sdk/pluginterfaces/base/ipluginbase.h"
#include "public.sdk/source/vst/vst3sdk/pluginterfaces/vst/ivstcomponent.h"
#include "public.sdk/source/vst/vst3sdk/pluginterfaces/vst/ivstaudioprocessor.h"

// Forward declarations
class PluginScanner;
class AudioEngine;
class PluginHost;
class PluginInstance;
class ASIODriver;
class WASAPIEngine;
class PluginBridge32;
class NotificationManager;
class ErrorLogger;

namespace EVH {
    
    // Constants
    constexpr int MAX_CHANNELS = 32;
    constexpr int DEFAULT_SAMPLE_RATE = 44100;
    constexpr int DEFAULT_BUFFER_SIZE = 512;
    constexpr int MAX_PLUGIN_SCAN_TIME_MS = 5000;
    
    // Plugin types
    enum class PluginType {
        VST3,
        Unknown
    };
    
    // Audio driver types
    enum class AudioDriverType {
        ASIO,
        WASAPI,
        DirectSound,
        Unknown
    };
    
    // Plugin state
    enum class PluginState {
        Unloaded,
        Loading,
        Loaded,
        Active,
        Bypassed,
        Error,
        Crashed
    };
    
    // Plugin info structure
    struct PluginInfo {
        std::wstring path;
        std::wstring name;
        std::wstring vendor;
        PluginType type;
        bool is64Bit;
        bool hasCustomEditor;
        int numInputs;
        int numOutputs;
        std::vector<std::wstring> categories;
        uint32_t uniqueId;
        bool isInstrument;
        bool validated;
        std::wstring errorMsg;
    };
    
    // Audio buffer structure
    template<typename T>
    class AudioBuffer {
    public:
        AudioBuffer(int channels, int samples);
        ~AudioBuffer();
        
        T** getWritePointer() { return writePointers.data(); }
        const T** getReadPointer() const { return const_cast<const T**>(writePointers.data()); }
        
        void clear();
        void applyGain(float gain);
        
    private:
        std::vector<std::vector<T>> channelData;
        std::vector<T*> writePointers;
        int numChannels;
        int numSamples;
    };
    
    // Exception classes
    class PluginException : public std::exception {
    public:
        PluginException(const std::string& msg) : message(msg) {}
        const char* what() const noexcept override { return message.c_str(); }
    private:
        std::string message;
    };
    
    class AudioException : public std::exception {
    public:
        AudioException(const std::string& msg) : message(msg) {}
        const char* what() const noexcept override { return message.c_str(); }
    private:
        std::string message;
    };
}

// Main VST Host class
class EnhancedVSTHost {
public:
    EnhancedVSTHost();
    ~EnhancedVSTHost();
    
    // Initialization
    bool initialize(HWND parentWindow);
    void shutdown();
    
    // Plugin management
    void scanPlugins(const std::vector<std::wstring>& searchPaths);
    bool loadPlugin(const std::wstring& path);
    void unloadPlugin(int pluginId);
    void unloadAllPlugins();
    
    // Audio engine control
    bool startAudio(EVH::AudioDriverType driverType);
    void stopAudio();
    bool isAudioRunning() const { return audioRunning.load(); }
    
    // Plugin chain management
    void addPluginToChain(int pluginId);
    void removePluginFromChain(int pluginId);
    void movePluginInChain(int pluginId, int newPosition);
    void bypassPlugin(int pluginId, bool bypass);
    
    // Settings
    void setSampleRate(double rate);
    void setBufferSize(int size);
    void setAudioDriver(EVH::AudioDriverType type);
    
    // Blacklist/Whitelist
    void addToBlacklist(const std::wstring& pluginPath);
    void removeFromBlacklist(const std::wstring& pluginPath);
    bool isBlacklisted(const std::wstring& pluginPath) const;
    
    // Error handling
    std::vector<std::wstring> getRecentErrors() const;
    void clearErrors();
    
    // Plugin info
    std::vector<EVH::PluginInfo> getAvailablePlugins() const;
    EVH::PluginInfo getPluginInfo(int pluginId) const;
    
    // Callbacks
    using ScanProgressCallback = std::function<void(int current, int total, const std::wstring& currentPlugin)>;
    using ErrorCallback = std::function<void(const std::wstring& error)>;
    using CrashCallback = std::function<void(int pluginId, const std::wstring& pluginName)>;
    
    void setScanProgressCallback(ScanProgressCallback cb) { scanProgressCb = cb; }
    void setErrorCallback(ErrorCallback cb) { errorCb = cb; }
    void setCrashCallback(CrashCallback cb) { crashCb = cb; }
    
private:
    // Core components
    std::unique_ptr<PluginScanner> scanner;
    std::unique_ptr<AudioEngine> audioEngine;
    std::unique_ptr<PluginHost> pluginHost;
    std::unique_ptr<NotificationManager> notificationMgr;
    std::unique_ptr<ErrorLogger> errorLogger;
    std::unique_ptr<PluginBridge32> bridge32;
    
    // Plugin management
    std::unordered_map<int, std::unique_ptr<PluginInstance>> loadedPlugins;
    std::vector<int> pluginChain;
    std::mutex pluginMutex;
    std::atomic<int> nextPluginId{1};
    
    // Blacklist
    std::unordered_set<std::wstring> blacklistedPlugins;
    mutable std::mutex blacklistMutex;
    
    // Audio state
    std::atomic<bool> audioRunning{false};
    double currentSampleRate{EVH::DEFAULT_SAMPLE_RATE};
    int currentBufferSize{EVH::DEFAULT_BUFFER_SIZE};
    EVH::AudioDriverType currentDriverType{EVH::AudioDriverType::WASAPI};
    
    // Window handling
    HWND parentWindow{nullptr};
    bool highDpiAware{false};
    
    // Callbacks
    ScanProgressCallback scanProgressCb;
    ErrorCallback errorCb;
    CrashCallback crashCb;
    
    // Helper methods
    void setupHighDPI();
    void handlePluginCrash(int pluginId);
    void logError(const std::wstring& error);
    bool validatePlugin(const std::wstring& path);
    std::unique_ptr<PluginInstance> createPluginInstance(const EVH::PluginInfo& info);
};

// Plugin Scanner with crash isolation
class PluginScanner {
public:
    PluginScanner();
    ~PluginScanner();
    
    void scanDirectory(const std::wstring& path, 
                      std::function<void(const EVH::PluginInfo&)> onPluginFound,
                      std::function<void(int, int, const std::wstring&)> onProgress);
    
    bool scanPluginInProcess(const std::wstring& path, EVH::PluginInfo& info);
    
private:
    struct ScanJob {
        std::wstring path;
        HANDLE processHandle;
        HANDLE pipeHandle;
        std::chrono::steady_clock::time_point startTime;
    };
    
    std::vector<ScanJob> activeJobs;
    std::mutex jobMutex;
    
    bool launchScannerProcess(const std::wstring& pluginPath, ScanJob& job);
    bool readScanResult(HANDLE pipe, EVH::PluginInfo& info);
    void terminateHungProcesses();
};

// Audio Engine base class
class AudioEngine {
public:
    virtual ~AudioEngine() = default;
    
    virtual bool initialize(double sampleRate, int bufferSize) = 0;
    virtual void shutdown() = 0;
    virtual bool start() = 0;
    virtual void stop() = 0;
    
    virtual std::vector<std::wstring> getDeviceList() const = 0;
    virtual bool selectDevice(const std::wstring& deviceName) = 0;
    
    using AudioCallback = std::function<void(const float**, float**, int numSamples)>;
    void setAudioCallback(AudioCallback cb) { audioCallback = cb; }
    
protected:
    AudioCallback audioCallback;
    double sampleRate;
    int bufferSize;
};

// ASIO implementation
class ASIOEngine : public AudioEngine {
public:
    ASIOEngine();
    ~ASIOEngine() override;
    
    bool initialize(double sampleRate, int bufferSize) override;
    void shutdown() override;
    bool start() override;
    void stop() override;
    
    std::vector<std::wstring> getDeviceList() const override;
    bool selectDevice(const std::wstring& deviceName) override;
    
private:
    std::unique_ptr<ASIODriver> driver;
    bool isRunning{false};
    
    static long asioMessages(long selector, long value, void* message, double* opt);
    static void bufferSwitch(long index, long processNow);
    static void bufferSwitchTimeInfo(ASIOTime* timeInfo, long index, long processNow);
};

// WASAPI implementation
class WASAPIEngine : public AudioEngine {
public:
    WASAPIEngine();
    ~WASAPIEngine() override;
    
    bool initialize(double sampleRate, int bufferSize) override;
    void shutdown() override;
    bool start() override;
    void stop() override;
    
    std::vector<std::wstring> getDeviceList() const override;
    bool selectDevice(const std::wstring& deviceName) override;
    
private:
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> deviceEnumerator;
    Microsoft::WRL::ComPtr<IMMDevice> audioDevice;
    Microsoft::WRL::ComPtr<IAudioClient> audioClient;
    Microsoft::WRL::ComPtr<IAudioRenderClient> renderClient;
    
    std::thread audioThread;
    std::atomic<bool> shouldStop{false};
    HANDLE bufferEvent{nullptr};
    
    void audioThreadFunc();
};

// Plugin Instance wrapper
class PluginInstance {
public:
    PluginInstance(const EVH::PluginInfo& info);
    ~PluginInstance();
    
    bool load();
    void unload();
    
    void process(const float** inputs, float** outputs, int numSamples);
    void processReplacing(float** inputs, float** outputs, int numSamples);
    
    void suspend();
    void resume();
    
    void openEditor(HWND parentWindow);
    void closeEditor();
    bool hasEditor() const { return info.hasCustomEditor; }
    
    void setBypass(bool bypass) { bypassed = bypass; }
    bool isBypassed() const { return bypassed; }
    
    EVH::PluginState getState() const { return state.load(); }
    const EVH::PluginInfo& getInfo() const { return info; }
    
    // Parameter management
    int getParameterCount() const;
    float getParameter(int index) const;
    void setParameter(int index, float value);
    std::wstring getParameterName(int index) const;
    std::wstring getParameterLabel(int index) const;
    std::wstring getParameterDisplay(int index) const;
    
private:
    EVH::PluginInfo info;
    std::atomic<EVH::PluginState> state{EVH::PluginState::Unloaded};
    bool bypassed{false};

    
    // VST3 specific
    Steinberg::IPtr<Steinberg::Vst::IComponent> component;
    Steinberg::IPtr<Steinberg::Vst::IAudioProcessor> processor;
    
    // Editor
    HWND editorWindow{nullptr};
    
    // Thread safety
    mutable std::mutex processMutex;
    
    // Helper methods
    bool loadVST3();
    static intptr_t VSTCALLBACK hostCallback(AEffect* effect, int32_t opcode, 
                                             int32_t index, intptr_t value, 
                                             void* ptr, float opt);
};

// 32-bit plugin bridge
class PluginBridge32 {
public:
    PluginBridge32();
    ~PluginBridge32();
    
    bool initialize();
    void shutdown();
    
    bool loadPlugin32(const std::wstring& path, EVH::PluginInfo& info);
    void unloadPlugin32(const std::wstring& path);
    
    void process32(const std::wstring& pluginPath, 
                   const float** inputs, float** outputs, 
                   int numSamples);
    
private:
    HANDLE bridgeProcess{nullptr};
    HANDLE commandPipe{nullptr};
    HANDLE dataPipe{nullptr};
    std::mutex bridgeMutex;
    
    bool sendCommand(const std::string& cmd);
    bool receiveResponse(std::string& response);
};

// Windows notification manager
class NotificationManager {
public:
    NotificationManager(HWND parentWindow);
    ~NotificationManager();
    
    void showNotification(const std::wstring& title, const std::wstring& message);
    void showErrorNotification(const std::wstring& error);
    void showPluginCrashNotification(const std::wstring& pluginName);
    
private:
    HWND parentWindow;
    bool useToastNotifications;
    
    void initializeToastNotifications();
    void showLegacyNotification(const std::wstring& title, const std::wstring& message);
};

// Error logger
class ErrorLogger {
public:
    ErrorLogger(const std::wstring& logPath);
    ~ErrorLogger();
    
    void logError(const std::wstring& error);
    void logPluginCrash(const std::wstring& pluginName, const std::wstring& details);
    void logAudioError(const std::wstring& error);
    
    std::vector<std::wstring> getRecentErrors(int count = 100) const;
    void clearLog();
    
private:
    std::wstring logFilePath;
    mutable std::mutex logMutex;
    std::queue<std::wstring> recentErrors;
    std::ofstream logFile;
    
    std::wstring getCurrentTimestamp() const;
};