// PluginInstance.cpp - Plugin instance wrapper with exception handling
#include "EnhancedVSTHost.h"
#include <windows.h>
#include <algorithm>

// VST2 opcodes
enum {
    effOpen = 0,
    effClose,
    effSetProgram,
    effGetProgram,
    effSetProgramName,
    effGetProgramName,
    effGetParamLabel,
    effGetParamDisplay,
    effGetParamName,
    effGetVu,
    effSetSampleRate,
    effSetBlockSize,
    effMainsChanged,
    effEditGetRect,
    effEditOpen,
    effEditClose,
    effEditDraw,
    effEditMouse,
    effEditKey,
    effEditIdle,
    effEditTop,
    effEditSleep,
    effIdentify,
    effGetChunk,
    effSetChunk,
    effProcessEvents,
    effCanBeAutomated,
    effString2Parameter,
    effGetNumProgramCategories,
    effGetProgramNameIndexed,
    effCopyProgram,
    effConnectInput,
    effConnectOutput,
    effGetInputProperties,
    effGetOutputProperties,
    effGetPlugCategory,
    effGetCurrentPosition,
    effGetDestinationBuffer,
    effOfflineNotify,
    effOfflinePrepare,
    effOfflineRun,
    effProcessVarIo,
    effSetSpeakerArrangement,
    effSetBlockSizeAndSampleRate,
    effSetBypass,
    effGetEffectName,
    effGetErrorText,
    effGetVendorString,
    effGetProductString,
    effGetVendorVersion,
    effVendorSpecific,
    effCanDo,
    effGetTailSize,
    effIdle,
    effGetIcon,
    effSetViewPosition,
    effGetParameterProperties,
    effKeysRequired,
    effGetVstVersion,
    effEditKeyDown,
    effEditKeyUp,
    effSetEditKnobMode,
    effGetMidiProgramName,
    effGetCurrentMidiProgram,
    effGetMidiProgramCategory,
    effHasMidiProgramsChanged,
    effGetMidiKeyName,
    effBeginSetProgram,
    effEndSetProgram,
    effGetSpeakerArrangement,
    effShellGetNextPlugin,
    effStartProcess,
    effStopProcess,
    effSetTotalSampleToProcess,
    effSetPanLaw,
    effBeginLoadBank,
    effBeginLoadProgram,
    effSetProcessPrecision,
    effGetNumMidiInputChannels,
    effGetNumMidiOutputChannels
};

// VST2 flags
enum {
    effFlagsHasEditor = 1 << 0,
    effFlagsCanReplacing = 1 << 4,
    effFlagsProgramChunks = 1 << 5,
    effFlagsIsSynth = 1 << 8,
    effFlagsNoSoundInStop = 1 << 9,
    effFlagsCanDoubleReplacing = 1 << 12
};

// Audio Master opcodes
enum {
    audioMasterAutomate = 0,
    audioMasterVersion,
    audioMasterCurrentId,
    audioMasterIdle,
    audioMasterPinConnected,
    audioMasterWantMidi = 6,
    audioMasterGetTime,
    audioMasterProcessEvents,
    audioMasterSetTime,
    audioMasterTempoAt,
    audioMasterGetNumAutomatableParameters,
    audioMasterGetParameterQuantization,
    audioMasterIOChanged,
    audioMasterNeedIdle,
    audioMasterSizeWindow,
    audioMasterGetSampleRate,
    audioMasterGetBlockSize,
    audioMasterGetInputLatency,
    audioMasterGetOutputLatency,
    audioMasterGetPreviousPlug,
    audioMasterGetNextPlug,
    audioMasterWillReplaceOrAccumulate,
    audioMasterGetCurrentProcessLevel,
    audioMasterGetAutomationState,
    audioMasterOfflineStart,
    audioMasterOfflineRead,
    audioMasterOfflineWrite,
    audioMasterOfflineGetCurrentPass,
    audioMasterOfflineGetCurrentMetaPass,
    audioMasterSetOutputSampleRate,
    audioMasterGetOutputSpeakerArrangement,
    audioMasterGetVendorString,
    audioMasterGetProductString,
    audioMasterGetVendorVersion,
    audioMasterVendorSpecific,
    audioMasterSetIcon,
    audioMasterCanDo,
    audioMasterGetLanguage,
    audioMasterOpenWindow,
    audioMasterCloseWindow,
    audioMasterGetDirectory,
    audioMasterUpdateDisplay,
    audioMasterBeginEdit,
    audioMasterEndEdit,
    audioMasterOpenFileSelector,
    audioMasterCloseFileSelector,
    audioMasterEditFile,
    audioMasterGetChunkFile,
    audioMasterGetInputSpeakerArrangement
};

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
        if (info.type == EVH::PluginType::VST2) {
            success = loadVST2();
        } else if (info.type == EVH::PluginType::VST3) {
            success = loadVST3();
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
        if (effect) {
            // Suspend and close VST2 plugin
            effect->dispatcher(effect, effMainsChanged, 0, 0, nullptr, 0.0f);
            effect->dispatcher(effect, effClose, 0, 0, nullptr, 0.0f);
            effect = nullptr;
        }
        
        if (component) {
            // Terminate VST3 plugin
            component->terminate();
            component = nullptr;
            processor = nullptr;
        }
        
        if (moduleHandle) {
            FreeLibrary(moduleHandle);
            moduleHandle = nullptr;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        // Force cleanup even if plugin crashes
        effect = nullptr;
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
            if (ch < info.numInputs) {
                std::copy_n(inputs[ch], numSamples, outputs[ch]);
            } else {
                std::fill_n(outputs[ch], numSamples, 0.0f);
            }
        }
        return;
    }
    
    std::lock_guard<std::mutex> lock(processMutex);
    
    __try {
        if (effect) {
            // VST2 processing
            effect->process(effect, const_cast<float**>(inputs), outputs, numSamples);
        } else if (processor) {
            // VST3 processing would go here
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        // Plugin crashed - bypass it
        state = EVH::PluginState::Crashed;
        
        // Copy input to output
        for (int ch = 0; ch < info.numOutputs; ++ch) {
            if (ch < info.numInputs) {
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
            if (ch < info.numInputs && inputs[ch] != outputs[ch]) {
                std::copy_n(inputs[ch], numSamples, outputs[ch]);
            } else if (ch >= info.numInputs) {
                std::fill_n(outputs[ch], numSamples, 0.0f);
            }
        }
        return;
    }
    
    std::lock_guard<std::mutex> lock(processMutex);
    
    __try {
        if (effect && (effect->flags & effFlagsCanReplacing)) {
            // VST2 processReplacing
            effect->processReplacing(effect, inputs, outputs, numSamples);
        } else if (effect) {
            // Fall back to process
            effect->process(effect, const_cast<const float**>(inputs), outputs, numSamples);
        } else if (processor) {
            // VST3 processing would go here
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        // Plugin crashed - bypass it
        state = EVH::PluginState::Crashed;
        
        // Copy input to output
        for (int ch = 0; ch < info.numOutputs; ++ch) {
            if (ch < info.numInputs && inputs[ch] != outputs[ch]) {
                std::copy_n(inputs[ch], numSamples, outputs[ch]);
            } else if (ch >= info.numInputs) {
                std::fill_n(outputs[ch], numSamples, 0.0f);
            }
        }
    }
}

void PluginInstance::suspend() {
    if (effect) {
        effect->dispatcher(effect, effMainsChanged, 0, 0, nullptr, 0.0f);
    }
    state = EVH::PluginState::Loaded;
}

void PluginInstance::resume() {
    if (effect) {
        effect->dispatcher(effect, effMainsChanged, 0, 1, nullptr, 0.0f);
    }
    state = EVH::PluginState::Active;
}

void PluginInstance::openEditor(HWND parentWindow) {
    if (!info.hasCustomEditor || editorWindow) {
        return;
    }
    
    if (effect) {
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
        
        if (editorWindow) {
            // Get editor rect
            ERect* rect = nullptr;
            effect->dispatcher(effect, effEditGetRect, 0, 0, &rect, 0.0f);
            
            if (rect) {
                SetWindowPos(editorWindow, nullptr, 0, 0,
                           rect->right - rect->left,
                           rect->bottom - rect->top,
                           SWP_NOMOVE | SWP_NOZORDER);
            }
            
            // Open editor
            effect->dispatcher(effect, effEditOpen, 0, 0, editorWindow, 0.0f);
        }
    }
}

void PluginInstance::closeEditor() {
    if (!editorWindow) {
        return;
    }
    
    if (effect) {
        effect->dispatcher(effect, effEditClose, 0, 0, nullptr, 0.0f);
    }
    
    DestroyWindow(editorWindow);
    editorWindow = nullptr;
}

int PluginInstance::getParameterCount() const {
    if (effect) {
        return effect->numParams;
    }
    return 0;
}

float PluginInstance::getParameter(int index) const {
    if (effect && index >= 0 && index < effect->numParams) {
        return effect->getParameter(effect, index);
    }
    return 0.0f;
}

void PluginInstance::setParameter(int index, float value) {
    if (effect && index >= 0 && index < effect->numParams) {
        effect->setParameter(effect, index, value);
    }
}

std::wstring PluginInstance::getParameterName(int index) const {
    if (effect && index >= 0 && index < effect->numParams) {
        char name[256] = {0};
        effect->dispatcher(effect, effGetParamName, index, 0, name, 0.0f);
        return std::wstring(name, name + strlen(name));
    }
    return L"";
}

std::wstring PluginInstance::getParameterLabel(int index) const {
    if (effect && index >= 0 && index < effect->numParams) {
        char label[256] = {0};
        effect->dispatcher(effect, effGetParamLabel, index, 0, label, 0.0f);
        return std::wstring(label, label + strlen(label));
    }
    return L"";
}

std::wstring PluginInstance::getParameterDisplay(int index) const {
    if (effect && index >= 0 && index < effect->numParams) {
        char display[256] = {0};
        effect->dispatcher(effect, effGetParamDisplay, index, 0, display, 0.0f);
        return std::wstring(display, display + strlen(display));
    }
    return L"";
}

bool PluginInstance::loadVST2() {
    // Load DLL
    moduleHandle = LoadLibraryW(info.path.c_str());
    if (!moduleHandle) {
        return false;
    }
    
    // Get entry point
    typedef AEffect* (*VSTPluginMain)(audioMasterCallback);
    VSTPluginMain vstMain = (VSTPluginMain)GetProcAddress(moduleHandle, "VSTPluginMain");
    if (!vstMain) {
        vstMain = (VSTPluginMain)GetProcAddress(moduleHandle, "main");
    }
    
    if (!vstMain) {
        FreeLibrary(moduleHandle);
        moduleHandle = nullptr;
        return false;
    }
    
    // Create effect
    effect = vstMain(hostCallback);
    if (!effect || effect->magic != kEffectMagic) {
        FreeLibrary(moduleHandle);
        moduleHandle = nullptr;
        effect = nullptr;
        return false;
    }
    
    // Store instance pointer
    effect->resvd1 = reinterpret_cast<intptr_t>(this);
    
    // Open effect
    effect->dispatcher(effect, effOpen, 0, 0, nullptr, 0.0f);
    
    // Set sample rate and block size
    effect->dispatcher(effect, effSetSampleRate, 0, 0, nullptr, 44100.0f);
    effect->dispatcher(effect, effSetBlockSize, 0, 512, nullptr, 0.0f);
    
    // Resume
    effect->dispatcher(effect, effMainsChanged, 0, 1, nullptr, 0.0f);
    
    return true;
}

bool PluginInstance::loadVST3() {
    // VST3 loading would be implemented here
    // This requires the VST3 SDK and COM interfaces
    return false;
}

intptr_t VSTCALLBACK PluginInstance::hostCallback(AEffect* effect, int32_t opcode, 
                                                  int32_t index, intptr_t value, 
                                                  void* ptr, float opt) {
    // Get instance if available
    PluginInstance* instance = nullptr;
    if (effect && effect->resvd1) {
        instance = reinterpret_cast<PluginInstance*>(effect->resvd1);
    }
    
    switch (opcode) {
        case audioMasterAutomate:
            // Parameter changed
            if (instance) {
                // Handle parameter automation
            }
            return 0;
            
        case audioMasterVersion:
            return 2400; // VST 2.4
            
        case audioMasterCurrentId:
            return effect ? effect->uniqueID : 0;
            
        case audioMasterIdle:
            if (effect) {
                effect->dispatcher(effect, effEditIdle, 0, 0, nullptr, 0.0f);
            }
            return 0;
            
        case audioMasterGetSampleRate:
            return 44100; // Default sample rate
            
        case audioMasterGetBlockSize:
            return 512; // Default block size
            
        case audioMasterGetVendorString:
            if (ptr) {
                strcpy_s((char*)ptr, 256, "Enhanced VST Host");
            }
            return 1;
            
        case audioMasterGetProductString:
            if (ptr) {
                strcpy_s((char*)ptr, 256, "Enhanced VST Host");
            }
            return 1;
            
        case audioMasterGetVendorVersion:
            return 1000; // Version 1.0.0
            
        case audioMasterCanDo:
            if (ptr) {
                const char* canDo = (const char*)ptr;
                if (strcmp(canDo, "sendVstEvents") == 0 ||
                    strcmp(canDo, "sendVstMidiEvent") == 0 ||
                    strcmp(canDo, "sendVstTimeInfo") == 0 ||
                    strcmp(canDo, "receiveVstEvents") == 0 ||
                    strcmp(canDo, "receiveVstMidiEvent") == 0 ||
                    strcmp(canDo, "reportConnectionChanges") == 0 ||
                    strcmp(canDo, "acceptIOChanges") == 0 ||
                    strcmp(canDo, "sizeWindow") == 0 ||
                    strcmp(canDo, "offline") == 0 ||
                    strcmp(canDo, "openFileSelector") == 0 ||
                    strcmp(canDo, "closeFileSelector") == 0 ||
                    strcmp(canDo, "startStopProcess") == 0 ||
                    strcmp(canDo, "shellCategory") == 0 ||
                    strcmp(canDo, "sendVstMidiEventFlagIsRealtime") == 0) {
                    return 1;
                }
            }
            return 0;
            
        case audioMasterGetTime:
            // Return time info
            return 0;
            
        case audioMasterProcessEvents:
            // Process MIDI events
            return 0;
            
        case audioMasterIOChanged:
            // Plugin IO configuration changed
            return 0;
            
        case audioMasterSizeWindow:
            // Resize editor window
            if (instance && instance->editorWindow) {
                SetWindowPos(instance->editorWindow, nullptr, 0, 0,
                           static_cast<int>(index), static_cast<int>(value),
                           SWP_NOMOVE | SWP_NOZORDER);
                return 1;
            }
            return 0;
            
        case audioMasterUpdateDisplay:
            // Update host display
            return 0;
            
        case audioMasterBeginEdit:
            // Begin parameter edit
            return 0;
            
        case audioMasterEndEdit:
            // End parameter edit
            return 0;
            
        default:
            return 0;
    }
}