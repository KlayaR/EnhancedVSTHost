// PluginInstance.cpp - Plugin instance wrapper
#include "EnhancedVSTHost.h"
#include <windows.h>
#include <algorithm>

// For this simplified version, we'll focus on VST3 support only
// VST2 is deprecated and the SDK is no longer available

PluginInstance::PluginInstance(const EVH::PluginInfo& info) 
    : info(info) {
}

PluginInstance::~PluginInstance() {
    unload();
}

bool PluginInstance::load() {
    if (state != EVH::PluginState::Unloaded) {
        return false;
    }
    
    state = EVH::PluginState::Loading;
    
    bool success = false;
    
    __try {
        if (info.type == EVH::PluginType::VST3) {
            success = loadVST3();
        } else {
            // Unsupported plugin type
            success = false;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        state = EVH::PluginState::Error;
        return false;
    }
    
    if (success) {
        state = EVH::PluginState::Loaded;
    } else {
        state = EVH::PluginState::Error;
    }
    
    return success;
}

void PluginInstance::unload() {
    if (state == EVH::PluginState::Unloaded) {
        return;
    }
    
    // Close editor if open
    if (editorWindow) {
        closeEditor();
    }
    
    __try {
        // Clean up VST3 interfaces
        if (component) {
            // In a real implementation, would call component->terminate()
            component = nullptr;
        }
        
        if (processor) {
            processor = nullptr;
        }
        
        if (moduleHandle) {
            FreeLibrary(moduleHandle);
            moduleHandle = nullptr;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        // Force cleanup even if plugin crashes
        component = nullptr;
        processor = nullptr;
        if (moduleHandle) {
            FreeLibrary(moduleHandle);
            moduleHandle = nullptr;
        }
    }
    
    state = EVH::PluginState::Unloaded;
}

void PluginInstance::process(const float** inputs, float** outputs, int numSamples) {
    if (state != EVH::PluginState::Active || bypassed) {
        // Copy input to output when bypassed
        for (int ch = 0; ch < info.numOutputs; ++ch) {
            if (ch < info.numInputs && inputs && inputs[ch]) {
                std::copy_n(inputs[ch], numSamples, outputs[ch]);
            } else {
                std::fill_n(outputs[ch], numSamples, 0.0f);
            }
        }
        return;
    }
    
    std::lock_guard<std::mutex> lock(processMutex);
    
    __try {
        if (processor) {
            // VST3 processing would go here
            // For now, just pass through
            for (int ch = 0; ch < info.numOutputs; ++ch) {
                if (ch < info.numInputs && inputs && inputs[ch]) {
                    std::copy_n(inputs[ch], numSamples, outputs[ch]);
                } else {
                    std::fill_n(outputs[ch], numSamples, 0.0f);
                }
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        // Plugin crashed - bypass it
        state = EVH::PluginState::Crashed;
        
        // Copy input to output
        for (int ch = 0; ch < info.numOutputs; ++ch) {
            if (ch < info.numInputs && inputs && inputs[ch]) {
                std::copy_n(inputs[ch], numSamples, outputs[ch]);
            } else {
                std::fill_n(outputs[ch], numSamples, 0.0f);
            }
        }
    }
}

void PluginInstance::processReplacing(float** inputs, float** outputs, int numSamples) {
    if (state != EVH::PluginState::Active || bypassed) {
        // Copy input to output when bypassed
        for (int ch = 0; ch < info.numOutputs; ++ch) {
            if (ch < info.numInputs && inputs && inputs[ch] && inputs[ch] != outputs[ch]) {
                std::copy_n(inputs[ch], numSamples, outputs[ch]);
            } else if (ch >= info.numInputs || !inputs || !inputs[ch]) {
                std::fill_n(outputs[ch], numSamples, 0.0f);
            }
        }
        return;
    }
    
    std::lock_guard<std::mutex> lock(processMutex);
    
    __try {
        if (processor) {
            // VST3 processing would go here
            // For now, just pass through
            for (int ch = 0; ch < info.numOutputs; ++ch) {
                if (ch < info.numInputs && inputs && inputs[ch] && inputs[ch] != outputs[ch]) {
                    std::copy_n(inputs[ch], numSamples, outputs[ch]);
                } else if (ch >= info.numInputs || !inputs || !inputs[ch]) {
                    std::fill_n(outputs[ch], numSamples, 0.0f);
                }
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        // Plugin crashed - bypass it
        state = EVH::PluginState::Crashed;
        
        // Copy input to output
        for (int ch = 0; ch < info.numOutputs; ++ch) {
            if (ch < info.numInputs && inputs && inputs[ch] && inputs[ch] != outputs[ch]) {
                std::copy_n(inputs[ch], numSamples, outputs[ch]);
            } else if (ch >= info.numInputs || !inputs || !inputs[ch]) {
                std::fill_n(outputs[ch], numSamples, 0.0f);
            }
        }
    }
}

void PluginInstance::suspend() {
    state = EVH::PluginState::Loaded;
}

void PluginInstance::resume() {
    state = EVH::PluginState::Active;
}

void PluginInstance::openEditor(HWND parentWindow) {
    if (!info.hasCustomEditor || editorWindow) {
        return;
    }
    
    // Create editor window
    editorWindow = CreateWindowExW(
        0,
        L"STATIC",
        info.name.c_str(),
        WS_CHILD | WS_VISIBLE,
        0, 0, 640, 480,
        parentWindow,
        nullptr,
        GetModuleHandle(nullptr),
        nullptr
    );
    
    if (editorWindow && component) {
        // VST3 editor creation would go here
    }
}

void PluginInstance::closeEditor() {
    if (!editorWindow) {
        return;
    }
    
    if (component) {
        // VST3 editor closing would go here
    }
    
    DestroyWindow(editorWindow);
    editorWindow = nullptr;
}

int PluginInstance::getParameterCount() const {
    // VST3 parameter count would be retrieved here
    return 0;
}

float PluginInstance::getParameter(int index) const {
    // VST3 parameter value would be retrieved here
    return 0.0f;
}

void PluginInstance::setParameter(int index, float value) {
    // VST3 parameter would be set here
}

std::wstring PluginInstance::getParameterName(int index) const {
    // VST3 parameter name would be retrieved here
    return L"";
}

std::wstring PluginInstance::getParameterLabel(int index) const {
    // VST3 parameter label would be retrieved here
    return L"";
}

std::wstring PluginInstance::getParameterDisplay(int index) const {
    // VST3 parameter display would be retrieved here
    return L"";
}

bool PluginInstance::loadVST3() {
    // Load VST3 bundle/DLL
    std::wstring modulePath = info.path;
    
    // For Windows, VST3 files can be either .vst3 bundles or .dll files
    if (modulePath.substr(modulePath.length() - 5) == L".vst3") {
        // It's a bundle, need to find the actual DLL inside
        // On Windows: pluginname.vst3/Contents/x86_64-win/pluginname.vst3
        modulePath += L"\\Contents\\x86_64-win\\";
        
        // Extract plugin name from path
        size_t lastSlash = info.path.find_last_of(L"\\");
        size_t lastDot = info.path.find_last_of(L".");
        if (lastSlash != std::wstring::npos && lastDot != std::wstring::npos) {
            std::wstring pluginName = info.path.substr(lastSlash + 1, lastDot - lastSlash - 1);
            modulePath += pluginName + L".vst3";
        }
    }
    
    moduleHandle = LoadLibraryW(modulePath.c_str());
    if (!moduleHandle) {
        return false;
    }
    
    // Get VST3 factory function
    typedef void* (*GetPluginFactory)();
    GetPluginFactory getFactory = (GetPluginFactory)GetProcAddress(moduleHandle, "GetPluginFactory");
    
    if (!getFactory) {
        FreeLibrary(moduleHandle);
        moduleHandle = nullptr;
        return false;
    }
    
    // Get factory interface
    void* factory = getFactory();
    if (!factory) {
        FreeLibrary(moduleHandle);
        moduleHandle = nullptr;
        return false;
    }
    
    // Here we would query the factory for component and processor interfaces
    // For this simplified version, we'll just mark it as successful
    
    // Store the factory pointer (in real implementation, would query interfaces)
    component = factory;
    processor = factory;  // In reality, these would be different interfaces
    
    // Set sample rate and block size
    // In real VST3, would call processor->setupProcessing()
    
    // Activate the plugin
    resume();
    
    return true;
}