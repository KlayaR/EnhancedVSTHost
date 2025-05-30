cmake_minimum_required(VERSION 3.20)
project(EnhancedVSTHost VERSION 1.0.0 LANGUAGES CXX)

# C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(WIN32)
  add_definitions(-DUNICODE -D_UNICODE)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /EHsc /MP")
  set(CMAKE_EXE_LINKER_FLAGS
    "${CMAKE_EXE_LINKER_FLAGS} /MANIFEST:EMBED /MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/manifest.xml"
  )
endif()

find_package(Threads REQUIRED)

# ——————————————————————————————
# User SDK paths
# ——————————————————————————————
set(VST3_SDK_PATH "C:/SDKs/vst3sdk" CACHE PATH "Root of your VST3 SDK clone")
set(ASIO_SDK_PATH "C:/SDKs/asiosdk" CACHE PATH "Root of your ASIO SDK (optional)")

# sanity checks:
if(NOT EXISTS "${VST3_SDK_PATH}/pluginterfaces/base/ipluginbase.h")
  message(FATAL_ERROR
    "Missing: ${VST3_SDK_PATH}/pluginterfaces/base/ipluginbase.h\n"
    "  → Make sure VST3_SDK_PATH is the root of your VST3 SDK."
  )
endif()
if(NOT EXISTS "${VST3_SDK_PATH}/public.sdk/source/vst/vsttypes.h")
  message(FATAL_ERROR
    "Missing: ${VST3_SDK_PATH}/public.sdk/source/vst/vsttypes.h\n"
    "  → Make sure VST3_SDK_PATH is the root of your VST3 SDK."
  )
endif()

# ——————————————————————————————
# Sources & headers
# ——————————————————————————————
set(SOURCES
  src/EnhancedVSTHost.cpp
  src/PluginScanner.cpp
  src/AudioEngines.cpp
  src/PluginInstance.cpp
  src/HelperComponents.cpp
)
set(HEADERS include/EnhancedVSTHost.h)

# helper macro to add the right includes
macro(setup_common_includes tgt)
  target_include_directories(${tgt}
    PUBLIC
      ${CMAKE_CURRENT_SOURCE_DIR}/include
      ${VST3_SDK_PATH}
  )
  if(EXISTS "${ASIO_SDK_PATH}/common")
    target_include_directories(${tgt} PUBLIC "${ASIO_SDK_PATH}/common")
    target_compile_definitions(${tgt} PUBLIC ASIO_SUPPORT)
  endif()
endmacro()

# ——————————————————————————————
# Main static library
# ——————————————————————————————
add_library(EnhancedVSTHostLib STATIC ${SOURCES} ${HEADERS})
setup_common_includes(EnhancedVSTHostLib)
target_link_libraries(EnhancedVSTHostLib
  PRIVATE Threads::Threads shlwapi shell32 ole32 uuid avrt
)

# ——————————————————————————————
# Scanner executable
# ——————————————————————————————
add_executable(VSTScanner src/PluginScanner.cpp)
target_compile_definitions(VSTScanner PRIVATE BUILD_SCANNER_PROCESS)
setup_common_includes(VSTScanner)
target_link_libraries(VSTScanner PRIVATE shlwapi)

# ——————————————————————————————
# 32-bit bridge (only if 32-bit build)
# ——————————————————————————————
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
  add_executable(VSTBridge32 src/Bridge32.cpp)
  setup_common_includes(VSTBridge32)
  target_link_libraries(VSTBridge32 PRIVATE shlwapi)
endif()

# ——————————————————————————————
# Example application
# ——————————————————————————————
add_executable(VSTHostExample examples/main.cpp)
setup_common_includes(VSTHostExample)
target_link_libraries(VSTHostExample PRIVATE EnhancedVSTHostLib)

# put all EXEs in bin/
foreach(_tgt VSTScanner VSTHostExample VSTBridge32)
  if(TARGET ${_tgt})
    set_target_properties(${_tgt}
      PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
  endif()
endforeach()

# ——————————————————————————————
# Install rules
# ——————————————————————————————
install(TARGETS EnhancedVSTHostLib VSTScanner VSTHostExample
  RUNTIME DESTINATION bin
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
)
install(FILES ${HEADERS} DESTINATION include)

# High-DPI manifest
file(WRITE "${CMAKE_CURRENT_SOURCE_DIR}/manifest.xml"
"<?xml version='1.0' encoding='UTF-8' standalone='yes'?>\n"
"<assembly xmlns='urn:schemas-microsoft-com:asm.v1' manifestVersion='1.0'>\n"
"  <application xmlns='urn:schemas.microsoft.com/SMI/2005/WindowsSettings'>\n"
"    <dpiAware xmlns='http://schemas.microsoft.com/SMI/2005/WindowsSettings'>True/PM</dpiAware>\n"
"    <dpiAwareness xmlns='http://schemas.microsoft.com/SMI/2016/WindowsSettings'>PerMonitorV2</dpiAwareness>\n"
"  </application>\n"
"</assembly>"
)

# pkg‐config
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/enhanced-vst-host.pc.in"
  "${CMAKE_CURRENT_BINARY_DIR}/enhanced-vst-host.pc" @ONLY
)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/enhanced-vst-host.pc"
  DESTINATION lib/pkgconfig
)
