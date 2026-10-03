# Phyzo plugin (VST3 + AU). JUCE is fetched at the pinned commit by scripts/fetch_deps.sh into <repo>/work/deps/JUCE.
cmake_minimum_required(VERSION 3.22)

set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
project(Phyzo VERSION 0.5.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(JUCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../work/deps/JUCE" CACHE PATH "JUCE source directory")
if(NOT EXISTS "${JUCE_DIR}/CMakeLists.txt")
  message(FATAL_ERROR "JUCE not found in ${JUCE_DIR}. Run scripts/fetch_deps.sh juce, or set -DJUCE_DIR=...")
endif()
add_subdirectory("${JUCE_DIR}" "${CMAKE_BINARY_DIR}/JUCE" EXCLUDE_FROM_ALL)

# The emulator (phyzo_core), from the same repository.
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../phyzo-emu" "${CMAKE_BINARY_DIR}/phyzo-emu" EXCLUDE_FROM_ALL)

# The skin runtime (RmlUi, Lua 5.4, FreeType; fetched by scripts/fetch_deps.sh), and its OpenGL renderer: RmlUi's
# own GL3 backend, compiled from the RmlUi package (kept apart from JUCE's GL headers).
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/skin" "${CMAKE_BINARY_DIR}/skin" EXCLUDE_FROM_ALL)
add_library(phyzo_skin_gl STATIC
  Source/gl/RmlGLRenderer.cpp
  "${PHYZO_DEPS_DIR}/RmlUi/Backends/RmlUi_Renderer_GL3.cpp")
target_include_directories(phyzo_skin_gl PRIVATE "${PHYZO_DEPS_DIR}/RmlUi/Backends")
target_compile_definitions(phyzo_skin_gl PRIVATE GL_SILENCE_DEPRECATION)
set_target_properties(phyzo_skin_gl PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(phyzo_skin_gl PUBLIC phyzo_skin ${CMAKE_DL_LIBS})

# AU exists only on macOS. Other systems build the VST3 and a standalone app (used to compile-check the code and
# to look at the skins on Linux, without a host).
if(APPLE)
  set(PHYZO_FORMATS VST3 AU)
else()
  set(PHYZO_FORMATS VST3 Standalone)
endif()

# Plugin identity. NEVER change any of these after the first build the owner tested: hosts use them (and the
# VST3 class ID derived from them) to find the plugin in saved projects. See CLAUDE.md.
juce_add_plugin(Phyzo
  COMPANY_NAME "DHammers"
  PLUGIN_MANUFACTURER_CODE DHmr
  PLUGIN_CODE Phyz
  PRODUCT_NAME "Phyzo"
  PLUGIN_NAME "Phyzo"
  BUNDLE_ID com.dhammers.phyzo
  DESCRIPTION "Phyzo synthesizer"
  FORMATS ${PHYZO_FORMATS}
  IS_SYNTH TRUE
  NEEDS_MIDI_INPUT TRUE
  NEEDS_MIDI_OUTPUT TRUE
  IS_MIDI_EFFECT FALSE
  VST3_CATEGORIES Instrument Synth
  AU_MAIN_TYPE kAudioUnitType_MusicDevice
  COPY_PLUGIN_AFTER_BUILD FALSE)

target_sources(Phyzo PRIVATE
  Source/PluginProcessor.cpp
  Source/PluginEditor.cpp
  Source/RmlSkinComponent.cpp
  Source/VirtualMidiPort.cpp)

target_compile_definitions(Phyzo PUBLIC
  JUCE_VST3_CAN_REPLACE_VST2=0      # part of the identity: there was never a VST2 version (fixes the VST3 class ID)
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0)

if(APPLE)
  target_link_libraries(Phyzo PRIVATE "-framework CoreMIDI")

  # Loopback test of the virtual MIDI port (CoreMIDI, no ROMs): run in CI after the plugin build.
  add_executable(midi_port_test tests/midi_port_test.cpp Source/VirtualMidiPort.cpp)
  target_include_directories(midi_port_test PRIVATE Source)
  target_link_libraries(midi_port_test PRIVATE phyzo_core "-framework CoreMIDI" "-framework CoreFoundation")
endif()

target_link_libraries(Phyzo
  PRIVATE
    phyzo_core
    phyzo_skin_gl
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_opengl
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)
