cmake_minimum_required(VERSION 3.16)
project(phyzo_emu VERSION 0.1.0 LANGUAGES C CXX)

# Headless boot harness for an MC68340-based synthesizer OS.
# Contains no manufacturer code or data: the OS image is supplied at run time.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Musashi is fetched at the pinned commit by scripts/fetch_deps.sh into <repo>/work/deps/Musashi.
set(_musashi_default "${CMAKE_CURRENT_SOURCE_DIR}/../work/deps/Musashi")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/work/deps/Musashi/m68kcpu.c")
  set(_musashi_default "${CMAKE_CURRENT_SOURCE_DIR}/work/deps/Musashi")
endif()
set(MUSASHI_DIR "${_musashi_default}" CACHE PATH "Musashi source directory")
if(NOT EXISTS "${MUSASHI_DIR}/m68kcpu.c")
  message(FATAL_ERROR "Musashi not found in ${MUSASHI_DIR}. Run scripts/fetch_deps.sh musashi, or set -DMUSASHI_DIR=...")
endif()

set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GENERATED_DIR}")

# Opcode-table bounds patch (portable table walk); see THIRD_PARTY_NOTICES.md.
set(MUSASHI_INPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/musashi-input")
set(MUSASHI_INPUT "${MUSASHI_INPUT_DIR}/m68k_in.c")
file(MAKE_DIRECTORY "${MUSASHI_INPUT_DIR}")
file(READ "${MUSASHI_DIR}/m68k_in.c" MUSASHI_INPUT_SOURCE)
string(REPLACE "const opcode_handler_struct *ostruct;"
  "const opcode_handler_struct *ostruct;\n\tconst opcode_handler_struct *table_end;"
  MUSASHI_INPUT_SOURCE "${MUSASHI_INPUT_SOURCE}")
string(REPLACE "ostruct = m68k_opcode_handler_table;"
  "ostruct = m68k_opcode_handler_table;\n\ttable_end = m68k_opcode_handler_table + (sizeof(m68k_opcode_handler_table) / sizeof(m68k_opcode_handler_table[0])) - 1;"
  MUSASHI_INPUT_SOURCE "${MUSASHI_INPUT_SOURCE}")
string(REPLACE "while(ostruct->mask" "while(ostruct < table_end && ostruct->mask"
  MUSASHI_INPUT_SOURCE "${MUSASHI_INPUT_SOURCE}")
file(WRITE "${MUSASHI_INPUT}" "${MUSASHI_INPUT_SOURCE}")

add_executable(m68kmake "${MUSASHI_DIR}/m68kmake.c")
add_custom_command(
  OUTPUT "${GENERATED_DIR}/m68kops.c" "${GENERATED_DIR}/m68kops.h"
  COMMAND m68kmake "${GENERATED_DIR}" "${MUSASHI_INPUT}"
  DEPENDS m68kmake "${MUSASHI_INPUT}"
  VERBATIM)

set(MUSASHI_SOURCES
  "${CMAKE_CURRENT_SOURCE_DIR}/src/musashi_state.c"
  "${MUSASHI_DIR}/m68kcpu.c"
  "${MUSASHI_DIR}/m68kdasm.c"
  "${MUSASHI_DIR}/softfloat/softfloat.c"
  "${GENERATED_DIR}/m68kops.c")

# Two builds of the core: "fast" (no per-instruction hook, used for speed
# measurement) and "trace" (instruction hook for checkpoints and PC history).
foreach(variant fast trace)
  add_library(musashi_${variant} STATIC ${MUSASHI_SOURCES})
  target_include_directories(musashi_${variant} PUBLIC
    "${MUSASHI_DIR}" "${MUSASHI_DIR}/softfloat" "${GENERATED_DIR}")
  target_compile_options(musashi_${variant} PRIVATE
    "-include${CMAKE_CURRENT_SOURCE_DIR}/src/musashi_config.h" -w)
  if(variant STREQUAL "trace")
    target_compile_definitions(musashi_${variant} PRIVATE HARNESS_TRACE=1)
  endif()
  target_link_libraries(musashi_${variant} PUBLIC m)
  set_target_properties(musashi_${variant} PROPERTIES POSITION_INDEPENDENT_CODE ON)
endforeach()

# The emulator as a library (fast core): used by the plugin and the tests.
# One static library with the Musashi core inside (the core and our glue call each other).
add_library(phyzo_core STATIC
  src/machine.cpp src/mc68340.cpp src/esp2_stub.cpp src/esp2_core.cpp src/voice_core.cpp src/panel_model.cpp
  src/os_image.cpp src/rom_id.cpp src/musashi_glue.cpp src/resampler.cpp src/engine.cpp src/midi_stream.cpp src/user_flash.cpp src/midi_learn.cpp src/midi_clock.cpp
  src/library/syx.cpp src/library/library.cpp src/library/hidden_machine.cpp src/library/service.cpp
  src/editor/editor_proto.cpp src/editor/editor_tables.cpp src/editor/editor_pages.cpp src/editor/editor_fields.cpp src/editor/editor_controller.cpp
  ${MUSASHI_SOURCES})
target_include_directories(phyzo_core PUBLIC src PRIVATE "${MUSASHI_DIR}" "${MUSASHI_DIR}/softfloat" "${GENERATED_DIR}")
find_package(Threads REQUIRED)
target_link_libraries(phyzo_core PUBLIC Threads::Threads m)
target_compile_options(phyzo_core PRIVATE
  "$<$<COMPILE_LANGUAGE:C>:-include${CMAKE_CURRENT_SOURCE_DIR}/src/musashi_config.h;-w>"
  "$<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra>")
set_target_properties(phyzo_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(HARNESS_SOURCES
  src/machine.cpp
  src/mc68340.cpp
  src/esp2_stub.cpp
  src/esp2_core.cpp
  src/voice_core.cpp
  src/panel_model.cpp
  src/os_image.cpp
  src/rom_id.cpp
  src/musashi_glue.cpp
  src/boot_test.cpp)

foreach(variant fast trace)
  if(variant STREQUAL "fast")
    set(exe phyzo_boot)
  else()
    set(exe phyzo_boot_trace)
  endif()
  add_executable(${exe} ${HARNESS_SOURCES})
  target_include_directories(${exe} PRIVATE src)
  target_link_libraries(${exe} PRIVATE musashi_${variant})
  if(variant STREQUAL "trace")
    target_compile_definitions(${exe} PRIVATE HARNESS_TRACE=1)
  endif()
  target_compile_options(${exe} PRIVATE -Wall -Wextra)
endforeach()

add_executable(editor_tables tools/editor_tables.cpp src/os_image.cpp src/rom_id.cpp src/editor/editor_tables.cpp
  src/editor/editor_pages.cpp)
target_include_directories(editor_tables PRIVATE src)
target_compile_options(editor_tables PRIVATE -Wall -Wextra)

add_executable(os_disasm tools/os_disasm.cpp src/os_image.cpp)
target_include_directories(os_disasm PRIVATE src)
target_link_libraries(os_disasm PRIVATE musashi_fast)

enable_testing()
# The boot test needs the user's OS image; pass it at configure time.
set(OS_IMAGE "" CACHE FILEPATH "Path to the OS image (user-supplied, never committed)")
if(OS_IMAGE)
  add_test(NAME boot_to_display COMMAND phyzo_boot --os "${OS_IMAGE}" --out "${CMAKE_CURRENT_BINARY_DIR}/boot_out" --expect "P 01")
endif()

# Script-driven player: boots the OS with the voice-chip core and renders WAV (fast core).
add_executable(phyzo_play src/machine.cpp src/mc68340.cpp src/esp2_stub.cpp src/esp2_core.cpp src/voice_core.cpp src/panel_model.cpp
  src/os_image.cpp src/rom_id.cpp src/musashi_glue.cpp src/play_main.cpp)
target_include_directories(phyzo_play PRIVATE src)
target_link_libraries(phyzo_play PRIVATE musashi_fast)
target_compile_options(phyzo_play PRIVATE -Wall -Wextra)

# Chip-level filter tests (sweep renders, stability stress, CPU bench); needs the user's OS and native wave image.
add_executable(filter_test tools/filter_test.cpp src/voice_core.cpp src/os_image.cpp)
target_include_directories(filter_test PRIVATE src)
target_compile_options(filter_test PRIVATE -Wall -Wextra)

# Chip-level ESP2 tests (testsine, bypass, instruction-rate bench); programs are read from the user's OS image.
add_executable(esp2_test tools/esp2_test.cpp src/esp2_core.cpp src/os_image.cpp)
target_include_directories(esp2_test PRIVATE src)
target_compile_options(esp2_test PRIVATE -Wall -Wextra)

# Tests that need no ROM data (run by CI).
add_executable(rom_id_test tests/rom_id_test.cpp src/rom_id.cpp src/os_image.cpp)
target_include_directories(rom_id_test PRIVATE src)
target_compile_options(rom_id_test PRIVATE -Wall -Wextra)
add_test(NAME rom_id COMMAND rom_id_test)
add_executable(os_update_test tests/os_update_test.cpp src/rom_id.cpp src/os_image.cpp)
target_include_directories(os_update_test PRIVATE src)
target_compile_options(os_update_test PRIVATE -Wall -Wextra)
add_test(NAME os_update COMMAND os_update_test)
add_executable(editor_test tests/editor_test.cpp src/editor/editor_proto.cpp src/editor/editor_tables.cpp src/editor/editor_pages.cpp
  src/editor/editor_fields.cpp src/editor/editor_controller.cpp)
target_include_directories(editor_test PRIVATE src)
target_compile_options(editor_test PRIVATE -Wall -Wextra)
add_test(NAME editor COMMAND editor_test)
add_executable(editor_edit_test tests/editor_edit_test.cpp src/editor/editor_proto.cpp src/editor/editor_tables.cpp
  src/editor/editor_pages.cpp src/editor/editor_fields.cpp src/editor/editor_controller.cpp)
target_include_directories(editor_edit_test PRIVATE src)
target_compile_options(editor_edit_test PRIVATE -Wall -Wextra)
add_test(NAME editor_edit COMMAND editor_edit_test)
add_executable(editor_fx_test tests/editor_fx_test.cpp src/editor/editor_proto.cpp src/editor/editor_tables.cpp
  src/editor/editor_pages.cpp src/editor/editor_fields.cpp src/editor/editor_controller.cpp)
target_include_directories(editor_fx_test PRIVATE src)
target_compile_options(editor_fx_test PRIVATE -Wall -Wextra)
add_test(NAME editor_fx COMMAND editor_fx_test)
add_executable(esp2_core_test tests/esp2_core_test.cpp src/esp2_core.cpp)
target_include_directories(esp2_core_test PRIVATE src)
target_compile_options(esp2_core_test PRIVATE -Wall -Wextra)
add_test(NAME esp2_core COMMAND esp2_core_test)
add_executable(voice_core_test tests/voice_core_test.cpp src/voice_core.cpp)
target_include_directories(voice_core_test PRIVATE src)
target_compile_options(voice_core_test PRIVATE -Wall -Wextra)
add_test(NAME voice_core COMMAND voice_core_test)
add_executable(machine_test tests/machine_test.cpp src/machine.cpp src/mc68340.cpp src/esp2_stub.cpp src/esp2_core.cpp
  src/voice_core.cpp src/panel_model.cpp src/os_image.cpp src/musashi_glue.cpp)
target_include_directories(machine_test PRIVATE src)
target_link_libraries(machine_test PRIVATE musashi_fast)
target_compile_options(machine_test PRIVATE -Wall -Wextra)
add_test(NAME machine COMMAND machine_test)
add_executable(state_xarch_test tests/state_xarch_test.cpp src/machine.cpp src/mc68340.cpp src/esp2_stub.cpp
  src/esp2_core.cpp src/voice_core.cpp src/panel_model.cpp src/os_image.cpp src/musashi_glue.cpp)
target_include_directories(state_xarch_test PRIVATE src)
target_link_libraries(state_xarch_test PRIVATE musashi_fast)
target_compile_options(state_xarch_test PRIVATE -Wall -Wextra)
add_test(NAME state_xarch COMMAND state_xarch_test)
add_executable(resampler_test tests/resampler_test.cpp src/resampler.cpp)
target_include_directories(resampler_test PRIVATE src)
target_compile_options(resampler_test PRIVATE -Wall -Wextra)
add_test(NAME resampler COMMAND resampler_test)
# Real-time stress of the plugin engine (knob sweeps, buttons, state requests while audio runs); not a CI test.
add_executable(phyzo_stress src/stress_main.cpp)
target_link_libraries(phyzo_stress PRIVATE phyzo_core)
target_compile_options(phyzo_stress PRIVATE -Wall -Wextra)

# Component speed benchmark without ROMs (not a CI test).
add_executable(speed_bench tools/speed_bench.cpp)
target_link_libraries(speed_bench PRIVATE phyzo_core)
target_include_directories(speed_bench PRIVATE src)

add_executable(engine_test tests/engine_test.cpp)
target_link_libraries(engine_test PRIVATE phyzo_core)
target_compile_options(engine_test PRIVATE -Wall -Wextra)
add_test(NAME engine COMMAND engine_test)
add_executable(midi_clock_test tests/midi_clock_test.cpp src/midi_clock.cpp)
target_include_directories(midi_clock_test PRIVATE src)
target_compile_options(midi_clock_test PRIVATE -Wall -Wextra)
add_test(NAME midi_clock COMMAND midi_clock_test)
add_executable(idle_test tests/idle_test.cpp)
target_link_libraries(idle_test PRIVATE phyzo_core)
target_compile_options(idle_test PRIVATE -Wall -Wextra)
add_test(NAME idle COMMAND idle_test)

add_executable(midi_learn_test tests/midi_learn_test.cpp)
target_link_libraries(midi_learn_test PRIVATE phyzo_core)
target_compile_options(midi_learn_test PRIVATE -Wall -Wextra)
add_test(NAME midi_learn COMMAND midi_learn_test)

add_executable(midi_test tests/midi_test.cpp)
target_link_libraries(midi_test PRIVATE phyzo_core)
target_compile_options(midi_test PRIVATE -Wall -Wextra)
add_test(NAME midi COMMAND midi_test)
add_executable(editor_engine_test tests/editor_engine_test.cpp)
target_link_libraries(editor_engine_test PRIVATE phyzo_core)
target_compile_options(editor_engine_test PRIVATE -Wall -Wextra)
add_test(NAME editor_engine COMMAND editor_engine_test)

add_executable(audio_in_test tests/audio_in_test.cpp)
target_link_libraries(audio_in_test PRIVATE phyzo_core)
target_compile_options(audio_in_test PRIVATE -Wall -Wextra)
add_test(NAME audio_in COMMAND audio_in_test)
add_executable(panel_test tests/panel_test.cpp src/panel_model.cpp)
target_include_directories(panel_test PRIVATE src)
target_compile_options(panel_test PRIVATE -Wall -Wextra)
add_test(NAME panel COMMAND panel_test)
add_executable(user_flash_test tests/user_flash_test.cpp)
target_link_libraries(user_flash_test PRIVATE phyzo_core)
target_compile_options(user_flash_test PRIVATE -Wall -Wextra)
add_test(NAME user_flash COMMAND user_flash_test)
add_executable(library_test tests/library_test.cpp)
target_link_libraries(library_test PRIVATE phyzo_core)
target_compile_options(library_test PRIVATE -Wall -Wextra)
add_test(NAME library COMMAND library_test)
