cmake_minimum_required(VERSION 3.20)
project(EnhancedVSTHost VERSION 1.0.0 LANGUAGES CXX)

# Use C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Windows-specific flags
if(WIN32)
  add_definitions(-DUNICODE -D_UNICODE)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /EHsc /MP")
  set(CMAKE_EXE_LINKER_FLAGS
      "${CMAKE_EXE_LINKER_FLAGS} /MANIFEST:EMBED /MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/manifest.xml"
  )
endif()

# Find threading library
find_package(Threads REQUIRED)

# SDK paths (configured by user)
set(VST3_SDK_PATH "C:/SDKs/vst3sdk" CACHE PATH "Path to VST3 SDK")
set(ASIO_SDK_PATH "C:/SDKs/asiosdk" CACHE PATH "Path to ASIO SDK")

# Locate VST3 headers
find_path(VST3_PI_FOLDER
  NAMES base/ipluginbase.h
  HINTS "${VST3_SDK_PATH}/pluginterfaces"
)
find_path(VST3_PUBLIC_SDK_FOLDER
  NAMES vst/vsttypes.h
  HINTS "${VST3_SDK_PATH}/public.sdk/source/vst"
)

if(NOT VST3_PI_FOLDER OR NOT VST3_PUBLIC_SDK_FOLDER)
  message(FATAL_ERROR
    "Could not locate VST3 headers under ${VST3_SDK_PATH}.\n"
    "Ensure the SDK root contains both:\n"
    "   pluginterfaces/   (with base/ipluginbase.h)\n"
    "   public.sdk/source/vst/"
  )
endif()

# Gather sources & headers
set(SOURCES
  src/EnhancedVSTHost.cpp
  src/PluginScanner.cpp
  src/AudioEngines.cpp
  src/PluginInstance.cpp
  src/HelperComponents.cpp
)

set(HEADERS
  include/EnhancedVSTHost.h
)

# Main static library
add_library(EnhancedVSTHostLib STATIC
  ${SOURCES}
  ${HEADERS}
)

# Include paths for our code + VST3 SDK
target_include_directories(EnhancedVSTHostLib
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${VST3_PI_FOLDER}
    ${VST3_PUBLIC_SDK_FOLDER}
)

# ASIO support (optional)
if(EXISTS ${ASIO_SDK_PATH})
  target_include_directories(EnhancedVSTHostLib PUBLIC "${ASIO_SDK_PATH}/common")
  target_compile_definitions(EnhancedVSTHostLib PUBLIC ASIO_SUPPORT)
endif()

# Link libraries
target_link_libraries(EnhancedVSTHostLib
  PRIVATE
    Threads::Threads
    shlwapi
    shell32
    ole32
    uuid
    avrt
)

# Scanner helper executable
add_executable(VSTScanner
  src/PluginScanner.cpp
)
target_compile_definitions(VSTScanner PRIVATE BUILD_SCANNER_PROCESS)
target_include_directories(VSTScanner
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${VST3_PI_FOLDER}
    ${VST3_PUBLIC_SDK_FOLDER}
)
if(EXISTS ${ASIO_SDK_PATH})
  target_include_directories(VSTScanner PRIVATE "${ASIO_SDK_PATH}/common")
  target_compile_definitions(VSTScanner PRIVATE ASIO_SUPPORT)
endif()
target_link_libraries(VSTScanner PRIVATE shlwapi)

# 32-bit bridge (only in 32-bit build)
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
  add_executable(VSTBridge32 src/Bridge32.cpp)
  target_include_directories(VSTBridge32 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(VSTBridge32 PRIVATE shlwapi)
endif()

# Example host application
add_executable(VSTHostExample examples/main.cpp)
target_link_libraries(VSTHostExample PRIVATE EnhancedVSTHostLib)

# Output directories for executables
set_target_properties(VSTScanner VSTHostExample
  PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
if(TARGET VSTBridge32)
  set_target_properties(VSTBridge32
    PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  )
endif()

# Install rules
install(TARGETS EnhancedVSTHostLib VSTScanner VSTHostExample
  RUNTIME DESTINATION bin
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib
)
install(FILES ${HEADERS} DESTINATION include)

# High-DPI manifest
file(WRITE "${CMAKE_CURRENT_SOURCE_DIR}/manifest.xml" 
"<?xml version='1.0' encoding='UTF-8' standalone='yes'?>\n"
"<assembly xmlns='urn:schemas-microsoft-com:asm.v1' manifestVersion='1.0'>\n"
"  <application xmlns='urn:schemas-microsoft-com:asm.v3'>\n"
"    <windowsSettings>\n"
"      <dpiAware xmlns='http://schemas.microsoft.com/SMI/2005/WindowsSettings'>True/PM</dpiAware>\n"
"      <dpiAwareness xmlns='http://schemas.microsoft.com/SMI/2016/WindowsSettings'>PerMonitorV2</dpiAwareness>\n"
"    </windowsSettings>\n"
"  </application>\n"
"</assembly>"
)

# Generate pkg-config file
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/enhanced-vst-host.pc.in"
  "${CMAKE_CURRENT_BINARY_DIR}/enhanced-vst-host.pc"
  @ONLY
)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/enhanced-vst-host.pc"
  DESTINATION lib/pkgconfig
)
