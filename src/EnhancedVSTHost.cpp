// EnhancedVSTHost.cpp - Main implementation
#include "EnhancedVSTHost.h"
#include <shlwapi.h>
#include <shellapi.h>
#include <comdef.h>
#include <wrl/client.h>
#include <sstream>
#include <fstream>
#include <chrono>
#include <algorithm>
#include <VersionHelpers.h>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")

using namespace EVH;
using namespace Microsoft::WRL;

// Helper functions
namespace {
    std::wstring GetErrorMessage(DWORD errorCode) {
        LPWSTR messageBuffer = nullptr;
        size_t size = FormatMessageW(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            NULL, errorCode, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            (LPWSTR)&messageBuffer, 0, NULL);
        
        std::wstring message(messageBuffer, size);
        LocalFree(messageBuffer);
        
        // Remove trailing newlines
        message.erase(message.find_last_not_of(L"\r\n") + 1);
        return message;
    }
    
    bool IsWaves32BitPlugin(const std::wstring& path) {
        // Special handling for Waves plugins which are notoriously problematic
        return path.find(L"Waves") != std::wstring::npos && 
               path.find(L"WaveShell") != std::wstring::npos;
    }
}

// EnhancedVSTHost Implementation
EnhancedVSTHost::EnhancedVSTHost() {
    scanner = std::make_unique<PluginScanner>();
    notificationMgr = std::make_unique<NotificationManager>(nullptr);
    errorLogger = std::make_unique<ErrorLogger>(L"VSTHost.log");
    bridge32 = std::make_unique<PluginBridge32>();
}

EnhancedVSTHost::~EnhancedVSTHost() {
    shutdown();
}

bool EnhancedVSTHost::initialize(HWND parentWindow) {
    this->parentWindow = parentWindow;
    
    // Setup high DPI support
    setupHighDPI();
    
    // Initialize COM for WASAPI
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        logError(L"Failed to initialize COM");
        return false;
    }
    
    // Initialize 32-bit bridge
    if (!bridge32->initialize()) {
        logError(L"Failed to initialize 32-bit plugin bridge");
        // Don't fail completely, just disable 32-bit support
    }
    
    // Load blacklist from file
    std::wifstream blacklistFile(L"blacklist.txt");
    if (blacklistFile.is_open()) {
        std::wstring line;
        while (std::getline(blacklistFile, line)) {
            if (!line.empty()) {
                blacklistedPlugins.insert(line);
            }
        }
        blacklistFile.close();
    }
    
    return true;
}

void EnhancedVSTHost::shutdown() {
    // Stop audio first
    stopAudio();
    
    // Unload all plugins
    unloadAllPlugins();
    
    // Shutdown components
    if (bridge32) {
        bridge32->shutdown();
    }
    
    // Save blacklist
    std::wofstream blacklistFile(L"blacklist.txt");
    if (blacklistFile.is_open()) {
        for (const auto& plugin : blacklistedPlugins) {
            blacklistFile << plugin << L"\n";
        }
        blacklistFile.close();
    }
    
    CoUninitialize();
}

void EnhancedVSTHost::scanPlugins(const std::vector<std::wstring>& searchPaths) {
    std::vector<PluginInfo> foundPlugins;
    int totalScanned = 0;
    
    auto onPluginFound = [&](const PluginInfo& info) {
        if (!isBlacklisted(info.path)) {
            foundPlugins.push_back(info);
        }
    };
    
    auto onProgress = [&](int current, int total, const std::wstring& currentPlugin) {
        totalScanned = total;
        if (scanProgressCb) {
            scanProgressCb(current, total, currentPlugin);
        }
    };
    
    for (const auto& path : searchPaths) {
        scanner->scanDirectory(path, onPluginFound, onProgress);
    }
    
    // Store found plugins (in real implementation, would store in a database)
    errorLogger->logError(L"Plugin scan complete. Found " + 
                         std::to_wstring(foundPlugins.size()) + 
                         L" plugins out of " + 
                         std::to_wstring(totalScanned) + L" scanned.");
}

bool EnhancedVSTHost::loadPlugin(const std::wstring& path) {
    if (isBlacklisted(path)) {
        logError(L"Plugin is blacklisted: " + path);
        return false;
    }
    
    // Validate plugin first
    if (!validatePlugin(path)) {
        logError(L"Plugin validation failed: " + path);
        return false;
    }
    
    // Get plugin info
    PluginInfo info;
    if (!scanner->scanPluginInProcess(path, info)) {
        logError(L"Failed to scan plugin: " + path);
        return false;
    }
    
    // Create plugin instance
    auto instance = createPluginInstance(info);
    if (!instance) {
        logError(L"Failed to create plugin instance: " + path);
        return false;
    }
    
    // Load the plugin
    try {
        if (!instance->load()) {
            logError(L"Failed to load plugin: " + path);
            return false;
        }
    } catch (const std::exception& e) {
        logError(L"Exception loading plugin: " + 
                std::wstring(e.what(), e.what() + strlen(e.what())));
        return false;
    }
    
    // Add to loaded plugins
    int pluginId = nextPluginId++;
    {
        std::lock_guard<std::mutex> lock(pluginMutex);
        loadedPlugins[pluginId] = std::move(instance);
    }
    
    return true;
}

void EnhancedVSTHost::unloadPlugin(int pluginId) {
    std::lock_guard<std::mutex> lock(pluginMutex);
    
    auto it = loadedPlugins.find(pluginId);
    if (it != loadedPlugins.end()) {
        try {
            it->second->unload();
        } catch (const std::exception& e) {
            logError(L"Exception unloading plugin: " + 
                    std::wstring(e.what(), e.what() + strlen(e.what())));
        }
        loadedPlugins.erase(it);
    }
    
    // Remove from chain
    auto chainIt = std::find(pluginChain.begin(), pluginChain.end(), pluginId);
    if (chainIt != pluginChain.end()) {
        pluginChain.erase(chainIt);
    }
}

void EnhancedVSTHost::unloadAllPlugins() {
    std::lock_guard<std::mutex> lock(pluginMutex);
    
    for (auto& [id, plugin] : loadedPlugins) {
        try {
            plugin->unload();
        } catch (...) {
            // Ignore exceptions during shutdown
        }
    }
    
    loadedPlugins.clear();
    pluginChain.clear();
}

bool EnhancedVSTHost::startAudio(AudioDriverType driverType) {
    if (audioRunning.load()) {
        return true;
    }
    
    // For now, only support WASAPI
    if (driverType != AudioDriverType::WASAPI) {
        logError(L"Only WASAPI audio driver is currently supported");
        return false;
    }
    
    // Create audio engine
    audioEngine = std::make_unique<WASAPIEngine>();
    
    // Initialize audio engine
    if (!audioEngine->initialize(currentSampleRate, currentBufferSize)) {
        logError(L"Failed to initialize audio engine");
        audioEngine.reset();
        return false;
    }
    
    // Set audio callback
    audioEngine->setAudioCallback([this](const float** inputs, float** outputs, int numSamples) {
        // Process audio through plugin chain
        std::lock_guard<std::mutex> lock(pluginMutex);
        
        // Clear output buffers
        for (int ch = 0; ch < 2; ++ch) {
            std::fill_n(outputs[ch], numSamples, 0.0f);
        }
        
        // Copy input to output for now (pass-through)
        for (int ch = 0; ch < 2; ++ch) {
            if (inputs && inputs[ch]) {
                std::copy_n(inputs[ch], numSamples, outputs[ch]);
            }
        }
        
        // Process through each plugin in chain
        for (int pluginId : pluginChain) {
            auto it = loadedPlugins.find(pluginId);
            if (it != loadedPlugins.end() && !it->second->isBypassed()) {
                try {
                    it->second->processReplacing(
                        const_cast<float**>(outputs),  // Use output as input for chain
                        outputs, 
                        numSamples
                    );
                } catch (const std::exception& e) {
                    handlePluginCrash(pluginId);
                }
            }
        }
    });
    
    // Start audio
    if (!audioEngine->start()) {
        logError(L"Failed to start audio engine");
        audioEngine.reset();
        return false;
    }
    
    audioRunning = true;
    currentDriverType = driverType;
    return true;
}

void EnhancedVSTHost::stopAudio() {
    if (!audioRunning.load()) {
        return;
    }
    
    audioRunning = false;
    
    if (audioEngine) {
        audioEngine->stop();
        audioEngine->shutdown();
        audioEngine.reset();
    }
}

void EnhancedVSTHost::addPluginToChain(int pluginId) {
    std::lock_guard<std::mutex> lock(pluginMutex);
    
    if (loadedPlugins.find(pluginId) != loadedPlugins.end()) {
        // Check if already in chain
        if (std::find(pluginChain.begin(), pluginChain.end(), pluginId) == pluginChain.end()) {
            pluginChain.push_back(pluginId);
        }
    }
}

void EnhancedVSTHost::removePluginFromChain(int pluginId) {
    std::lock_guard<std::mutex> lock(pluginMutex);
    
    auto it = std::find(pluginChain.begin(), pluginChain.end(), pluginId);
    if (it != pluginChain.end()) {
        pluginChain.erase(it);
    }
}

void EnhancedVSTHost::movePluginInChain(int pluginId, int newPosition) {
    std::lock_guard<std::mutex> lock(pluginMutex);
    
    auto it = std::find(pluginChain.begin(), pluginChain.end(), pluginId);
    if (it != pluginChain.end()) {
        pluginChain.erase(it);
        
        if (newPosition >= 0 && newPosition <= static_cast<int>(pluginChain.size())) {
            pluginChain.insert(pluginChain.begin() + newPosition, pluginId);
        } else {
            pluginChain.push_back(pluginId);
        }
    }
}

void EnhancedVSTHost::bypassPlugin(int pluginId, bool bypass) {
    std::lock_guard<std::mutex> lock(pluginMutex);
    
    auto it = loadedPlugins.find(pluginId);
    if (it != loadedPlugins.end()) {
        it->second->setBypass(bypass);
    }
}

void EnhancedVSTHost::setSampleRate(double rate) {
    if (audioRunning.load()) {
        stopAudio();
        currentSampleRate = rate;
        startAudio(currentDriverType);
    } else {
        currentSampleRate = rate;
    }
}

void EnhancedVSTHost::setBufferSize(int size) {
    if (audioRunning.load()) {
        stopAudio();
        currentBufferSize = size;
        startAudio(currentDriverType);
    } else {
        currentBufferSize = size;
    }
}

void EnhancedVSTHost::addToBlacklist(const std::wstring& pluginPath) {
    std::lock_guard<std::mutex> lock(blacklistMutex);
    blacklistedPlugins.insert(pluginPath);
}

void EnhancedVSTHost::removeFromBlacklist(const std::wstring& pluginPath) {
    std::lock_guard<std::mutex> lock(blacklistMutex);
    blacklistedPlugins.erase(pluginPath);
}

bool EnhancedVSTHost::isBlacklisted(const std::wstring& pluginPath) const {
    std::lock_guard<std::mutex> lock(blacklistMutex);
    return blacklistedPlugins.find(pluginPath) != blacklistedPlugins.end();
}

std::vector<std::wstring> EnhancedVSTHost::getRecentErrors() const {
    return errorLogger->getRecentErrors();
}

void EnhancedVSTHost::clearErrors() {
    errorLogger->clearLog();
}

void EnhancedVSTHost::setupHighDPI() {
    if (IsWindows8Point1OrGreater()) {
        // SetProcessDpiAwareness would be better but requires Windows 8.1 SDK
        SetProcessDPIAware();
        highDpiAware = true;
    } else {
        SetProcessDPIAware();
    }
}

void EnhancedVSTHost::handlePluginCrash(int pluginId) {
    auto it = loadedPlugins.find(pluginId);
    if (it != loadedPlugins.end()) {
        std::wstring pluginName = it->second->getInfo().name;
        
        // Log the crash
        errorLogger->logPluginCrash(pluginName, L"Plugin crashed during audio processing");
        
        // Show notification
        notificationMgr->showPluginCrashNotification(pluginName);
        
        // Remove from chain
        removePluginFromChain(pluginId);
        
        // Call crash callback
        if (crashCb) {
            crashCb(pluginId, pluginName);
        }
    }
}

void EnhancedVSTHost::logError(const std::wstring& error) {
    errorLogger->logError(error);
    
    if (errorCb) {
        errorCb(error);
    }
}

bool EnhancedVSTHost::validatePlugin(const std::wstring& path) {
    // Check if file exists
    if (!PathFileExistsW(path.c_str())) {
        return false;
    }
    
    // Check if it's a DLL or VST3
    std::wstring ext = PathFindExtensionW(path.c_str());
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext != L".dll" && ext != L".vst3") {
        return false;
    }
    
    // Try to load the DLL to check for corruption
    HMODULE hModule = LoadLibraryExW(path.c_str(), nullptr, 
                                     DONT_RESOLVE_DLL_REFERENCES | 
                                     LOAD_LIBRARY_AS_DATAFILE);
    if (!hModule) {
        return false;
    }
    
    FreeLibrary(hModule);
    return true;
}

std::unique_ptr<PluginInstance> EnhancedVSTHost::createPluginInstance(const PluginInfo& info) {
    return std::make_unique<PluginInstance>(info);
}

std::vector<EVH::PluginInfo> EnhancedVSTHost::getAvailablePlugins() const {
    // This would return the list of scanned plugins from a database
    // For now, return empty vector
    return std::vector<EVH::PluginInfo>();
}

EVH::PluginInfo EnhancedVSTHost::getPluginInfo(int pluginId) const {
    std::lock_guard<std::mutex> lock(pluginMutex);
    
    auto it = loadedPlugins.find(pluginId);
    if (it != loadedPlugins.end()) {
        return it->second->getInfo();
    }
    
    return EVH::PluginInfo();
}

void EnhancedVSTHost::setAudioDriver(AudioDriverType type) {
    if (audioRunning) {
        stopAudio();
        currentDriverType = type;
        startAudio(type);
    } else {
        currentDriverType = type;
    }
}