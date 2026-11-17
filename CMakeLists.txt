cmake_minimum_required(VERSION 3.19)
project(ArcusOdysseyRecomp VERSION 1.0.0 LANGUAGES C CXX ASM)

# ---------------------------------------------------------------------------
# Arcus Odyssey (Wolf Team, 1991) — native static recompilation of our patched
# ROM (twin-stick + 424-px widescreen, see ../patch and ../tools/build.py --native).
# Engine: segagenesisrecomp (engine/ submodule, branch arcus-native).
# Layout and build follow mstan/RocketKnightAdventuresRecomp.
# The ROM is a build input only: it is copied to game/arcus.bin (gitignored),
# recompiled, and embedded in the executable. It is never committed.
# ---------------------------------------------------------------------------

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

set(ARCUS_ROM "${CMAKE_SOURCE_DIR}/../build/arcus-native.bin" CACHE FILEPATH
    "Patched Arcus Odyssey ROM to recompile and embed")
if(NOT EXISTS "${ARCUS_ROM}")
    message(FATAL_ERROR "ARCUS_ROM not found: ${ARCUS_ROM} (run ../tools/build.py)")
endif()
# The full-screen build (ARCUS_TALL, below) copies its ROM to its own name so a native and a
# full-screen build tree can coexist.
option(ARCUS_TALL "Full-screen view (needs the --tall ROM)" OFF)
if(ARCUS_TALL)
    set(ARCUS_ROM_NAME "arcus-odyssey-usa-dx.bin")   # your ROM with the DX patch
else()
    set(ARCUS_ROM_NAME "arcus.bin")
endif()
configure_file("${ARCUS_ROM}" "${CMAKE_SOURCE_DIR}/game/${ARCUS_ROM_NAME}" COPYONLY)
# Embed the same bytes the recompiler reads (game/<ROM name>) in the executable.
set(ARCUS_ROM_EMBED "${CMAKE_SOURCE_DIR}/game/${ARCUS_ROM_NAME}")
configure_file("${CMAKE_SOURCE_DIR}/game/rom_embed.S.in" "${CMAKE_BINARY_DIR}/rom_embed.S" @ONLY)
set_source_files_properties("${CMAKE_BINARY_DIR}/rom_embed.S" PROPERTIES
    OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/game/${ARCUS_ROM_NAME}")

# The Linux package's program (tools/build-linux.sh); see below.
option(ARCUS_LINUX_APP "Build the Linux package's program" OFF)

# Generated inputs (the engine, the extras) live in ARCUS_WORK_DIR: build/ in the public repo
# (build.py), this folder in the development tree.
set(ARCUS_WORK_DIR "${CMAKE_SOURCE_DIR}" CACHE PATH "Folder holding engine/ and extras-cache/")
set(RECOMP_ROOT "${ARCUS_WORK_DIR}/engine")
set(RUNNER_ROOT "${RECOMP_ROOT}/runner")

if(NINTENDO_SWITCH)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
    set(ARCUS_SDL_LIBS PkgConfig::SDL2)
else()
    if(APPLE)
        find_program(BREW_EXECUTABLE brew)
        if(BREW_EXECUTABLE)
            execute_process(COMMAND "${BREW_EXECUTABLE}" --prefix sdl2
                OUTPUT_VARIABLE _brew_sdl2_prefix OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET RESULT_VARIABLE _brew_sdl2_rc)
            if(_brew_sdl2_rc EQUAL 0 AND _brew_sdl2_prefix)
                list(APPEND CMAKE_PREFIX_PATH "${_brew_sdl2_prefix}")
            endif()
        endif()
    endif()
    find_package(SDL2 REQUIRED)
    set(ARCUS_SDL_LIBS SDL2::SDL2main SDL2::SDL2)
    # The Linux package links SDL2 statically when it was built that way (tools/build-linux.sh
    # in platform/linux/Dockerfile): the game then needs only glibc, and SDL loads X11 or
    # Wayland, PipeWire/PulseAudio/ALSA and OpenGL from the system at run time.
    if(ARCUS_LINUX_APP AND TARGET SDL2::SDL2-static)
        set(ARCUS_SDL_LIBS SDL2::SDL2main SDL2::SDL2-static)
    endif()
endif()
# The runner includes <SDL2/SDL.h>: add the parent of every .../SDL2 include dir.
set(SDL2_PARENT_INCLUDE_DIRS "")
foreach(_inc IN LISTS SDL2_INCLUDE_DIRS)
    get_filename_component(_inc_name "${_inc}" NAME)
    if(_inc_name STREQUAL "SDL2")
        get_filename_component(_inc_parent "${_inc}" DIRECTORY)
        list(APPEND SDL2_PARENT_INCLUDE_DIRS "${_inc_parent}")
    endif()
endforeach()

# Switch builds have no sockets: the TCP debug server is off there.
if(NINTENDO_SWITCH)
    set(_trace_default OFF)
else()
    set(_trace_default ON)
endif()
option(GEN_ENABLE_TRACE "TCP debug server + frame snapshots (dev builds only)" ${_trace_default})
# Mac-only test configuration: compile the Switch code paths (libnx pad input,
# applet pause, heap fiber stack with canary, sdmc base dir) against the small
# libnx stubs in platform/switch/stub, so they run in the headless test suite.
option(ARCUS_FAKE_SWITCH "Build the Switch code paths against libnx stubs (Mac tests)" OFF)
option(SONIC_REVERSE_DEBUG "Reverse-debug WRAM store ring (dev builds)" OFF)

include("${RECOMP_ROOT}/cmake/GenesisRecompGenerated.cmake")
genesisrecomp_runner_sources(RUNNER_SOURCES
    TRACE ${GEN_ENABLE_TRACE} REVERSE_DEBUG ${SONIC_REVERSE_DEBUG})

set(FM_SOURCES
    "${RUNNER_ROOT}/audio/ym2612_ymfm.cpp"
    "${RUNNER_ROOT}/external/ymfm/src/ymfm_opn.cpp"
    "${RUNNER_ROOT}/external/ymfm/src/ymfm_ssg.cpp"
    "${RUNNER_ROOT}/external/ymfm/src/ymfm_adpcm.cpp"
    "${RUNNER_ROOT}/external/ymfm/src/ymfm_pcm.cpp")

set(OWN_BACKEND_SOURCES
    "${RUNNER_ROOT}/video/genesis_vdp.c"
    "${RUNNER_ROOT}/video/genesis_bus.c"
    "${RUNNER_ROOT}/video/genesis_machine.c"
    "${RUNNER_ROOT}/external/superzazu/z80.c")

set(GAME "${CMAKE_SOURCE_DIR}/game")
genesisrecomp_add_generated_sources(GENERATED_SOURCES arcus "${GAME}" ${ARCUS_ROM_NAME}
    REVERSE_DEBUG "${SONIC_REVERSE_DEBUG}")

# Host-side game modules: controls (track D), settings/overlay and presentation
# (track C). arcus_options_stub.c stands in while arcus_options.c is absent.
set(ARCUS_HOST_SOURCES "${GAME}/arcus_input.c")
if(EXISTS "${GAME}/arcus_options.c")
    list(APPEND ARCUS_HOST_SOURCES "${GAME}/arcus_options.c" "${GAME}/arcus_saves.c"
                                   "${GAME}/arcus_cheats.c" "${GAME}/arcus_magic.c" "${GAME}/arcus_manual.c" "${GAME}/arcus_extras.c" "${GAME}/arcus_minimap.c"
                                   "${GAME}/arcus_title_dx.c")
else()
    list(APPEND ARCUS_HOST_SOURCES "${GAME}/arcus_options_stub.c")
endif()
# The CRT shader is embedded like the ROM.
set(ARCUS_CRT_GLSL "${CMAKE_SOURCE_DIR}/shaders/crt.glsl")
configure_file("${GAME}/shader_embed.S.in" "${CMAKE_BINARY_DIR}/shader_embed.S" @ONLY)
set_source_files_properties("${CMAKE_BINARY_DIR}/shader_embed.S" PROPERTIES
    OBJECT_DEPENDS "${ARCUS_CRT_GLSL}")
list(APPEND ARCUS_HOST_SOURCES
    "${GAME}/arcus_video.c" "${GAME}/arcus_gl.c" "${CMAKE_BINARY_DIR}/shader_embed.S")
# Extras ("books", made by the build scripts: tools/fetch-extras.py and tools/make-guide.py),
# never committed: extras-cache/books/<NN-name>/ holds book.txt (one line,
# "kind|group|title|detail") and page01.jpg ... All pages go in one blob plus an offsets
# table, and a catalog string has a "<pages>|<book.txt>" line per book, in folder order. No
# books (offline, nothing built): an empty catalog, and the EXTRAS menu entry is hidden.
file(GLOB ARCUS_BOOKS CONFIGURE_DEPENDS LIST_DIRECTORIES true "${ARCUS_WORK_DIR}/extras-cache/books/*")
list(SORT ARCUS_BOOKS)
set(ARCUS_EXTRAS_PAGES "")
set(ARCUS_EXTRAS_CATALOG "")
foreach(_book IN LISTS ARCUS_BOOKS)
    if(NOT EXISTS "${_book}/book.txt")
        continue()
    endif()
    file(GLOB _pages CONFIGURE_DEPENDS "${_book}/page*.jpg")
    list(SORT _pages)
    list(LENGTH _pages _n)
    if(_n EQUAL 0)
        continue()
    endif()
    file(STRINGS "${_book}/book.txt" _line LIMIT_COUNT 1)
    string(REPLACE "\"" "'" _line "${_line}")
    string(APPEND ARCUS_EXTRAS_CATALOG "${_n}|${_line}\\n")
    list(APPEND ARCUS_EXTRAS_PAGES ${_pages})
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_book}/book.txt")
endforeach()
list(LENGTH ARCUS_EXTRAS_PAGES ARCUS_EXTRAS_N)
string(CONCAT _x "/* extras_embed.S - generated by CMakeLists.txt: the extras' pages (extras-cache, never\n"
       " * committed) as one blob; offsets has count + 1 entries; catalog lists the books. */\n"
       "#if defined(__APPLE__)\n#define SYM(x) _##x\n        .section __TEXT,__const\n"
       "#else\n#define SYM(x) x\n        .section .rodata\n#endif\n"
       "        .globl  SYM(arcus_extras_data)\n        .globl  SYM(arcus_extras_offsets)\n"
       "        .globl  SYM(arcus_extras_count)\n        .globl  SYM(arcus_extras_catalog)\n"
       "SYM(arcus_extras_catalog):\n        .asciz  \"${ARCUS_EXTRAS_CATALOG}\"\n"
       "        .p2align 4\nSYM(arcus_extras_data):\n")
set(_i 0)
foreach(_page IN LISTS ARCUS_EXTRAS_PAGES)
    string(APPEND _x "        .incbin \"${_page}\"\nSYM(arcus_extras_end${_i}):\n")
    math(EXPR _i "${_i} + 1")
endforeach()
string(APPEND _x "        .p2align 2\nSYM(arcus_extras_offsets):\n        .long   0\n")
set(_i 0)
foreach(_page IN LISTS ARCUS_EXTRAS_PAGES)
    string(APPEND _x "        .long   SYM(arcus_extras_end${_i}) - SYM(arcus_extras_data)\n")
    math(EXPR _i "${_i} + 1")
endforeach()
string(APPEND _x "SYM(arcus_extras_count):\n        .long   ${ARCUS_EXTRAS_N}\n")
string(APPEND _x "#if !defined(__APPLE__)\n        .section .note.GNU-stack,\"\",%progbits\n#endif\n")
file(WRITE "${CMAKE_BINARY_DIR}/extras_embed.S.new" "${_x}")
configure_file("${CMAKE_BINARY_DIR}/extras_embed.S.new" "${CMAKE_BINARY_DIR}/extras_embed.S" COPYONLY)
set_source_files_properties("${CMAKE_BINARY_DIR}/extras_embed.S" PROPERTIES
    OBJECT_DEPENDS "${ARCUS_EXTRAS_PAGES}")
list(LENGTH ARCUS_BOOKS _nb)
message(STATUS "Extras: ${ARCUS_EXTRAS_N} pages in ${_nb} book folders")
list(APPEND ARCUS_HOST_SOURCES "${CMAKE_BINARY_DIR}/extras_embed.S")
set(SWITCH_PLATFORM "${CMAKE_SOURCE_DIR}/platform/switch")
if(ARCUS_FAKE_SWITCH)
    list(APPEND ARCUS_HOST_SOURCES "${SWITCH_PLATFORM}/stub/libnx_stub.c")
endif()

add_executable(ArcusOdyssey
    ${RUNNER_SOURCES} ${FM_SOURCES} ${OWN_BACKEND_SOURCES} ${GENERATED_SOURCES}
    "${GAME}/arcus_spec.c" ${ARCUS_HOST_SOURCES} "${CMAKE_BINARY_DIR}/rom_embed.S")
add_dependencies(ArcusOdyssey ${GENERATED_SOURCES_TARGET})
target_compile_definitions(ArcusOdyssey PRIVATE
    INCLUDE_GENERATED_CODE=1 ENABLE_RECOMPILED_CODE=1 OWN_BACKEND=1
    DEFAULT_DEBUG_PORT=4400
    $<$<BOOL:${GEN_ENABLE_TRACE}>:GEN_ENABLE_TRACE=1>
    $<$<BOOL:${SONIC_REVERSE_DEBUG}>:SONIC_REVERSE_DEBUG=1>)
target_include_directories(ArcusOdyssey PRIVATE
    "${RUNNER_ROOT}" "${RUNNER_ROOT}/include" "${RUNNER_ROOT}/video"
    "${RUNNER_ROOT}/external/ymfm/src" "${RUNNER_ROOT}/external/superzazu"
    "${RUNNER_ROOT}/external/clowncommon" "${RECOMP_ROOT}/recompiler/src" "${GAME}"
    ${SDL2_PARENT_INCLUDE_DIRS})
target_link_libraries(ArcusOdyssey PRIVATE ${ARCUS_SDL_LIBS})
if(NINTENDO_SWITCH)
    # Track C presenter: GLES2 entry points linked directly (GLESv2 is not in
    # sdl2.pc); SDL's opengles2 renderer owns the EGL context.
    target_compile_definitions(ArcusOdyssey PRIVATE ARCUS_GL_DIRECT=1)
    target_link_libraries(ArcusOdyssey PRIVATE GLESv2 EGL glapi drm_nouveau)
endif()
genesisrecomp_runner_target(ArcusOdyssey)
# Batched VDP plane rendering (engine opt-in): per-tile name-table/pattern fetches instead of
# per-pixel. Qualified for this game pixel-for-pixel (demo, act starts, full-screen areas);
# it halves the VDP's share of the frame, the largest part of the emulation work on the
# Switch (batch 5 profile: ~2/3 of the emulation time with the per-pixel renderer).
target_compile_definitions(ArcusOdyssey PRIVATE GENESIS_BATCHED_PLANES=1)

# Full-screen ("tall") build: embeds build/arcus-tall.bin (tools/build.py --tall) and fills
# the screen at an integer scale (480x270 docked / 426x240 handheld, chosen live). The Switch
# build always uses it; Mac builds default to the native ROM (the GPGX comparison tests run
# it) and use a separate build-tall directory for the full-screen tests.
set(ARCUS_ROM_CRC32 "" CACHE STRING "Expected CRC32 of ARCUS_ROM (0x...); empty = the native ROM")
if(ARCUS_ROM_CRC32)
    target_compile_definitions(ArcusOdyssey PRIVATE ARCUS_ROM_CRC32=${ARCUS_ROM_CRC32}u)
endif()
if(ARCUS_TALL)
    target_compile_definitions(ArcusOdyssey PRIVATE ARCUS_TALL_BUILD=1)
endif()
set(ARCUS_APP_DIR "arcus-odyssey-dx")

# Switch platform code paths: the real libnx on the console, stubs on the Mac.
if(NINTENDO_SWITCH OR ARCUS_FAKE_SWITCH)
    target_compile_definitions(ArcusOdyssey PRIVATE
        GENESIS_SWITCH_APP_DIR="${ARCUS_APP_DIR}")
endif()
if(ARCUS_FAKE_SWITCH)
    target_compile_definitions(ArcusOdyssey PRIVATE GENESIS_SWITCH_STUBS=1)
    target_include_directories(ArcusOdyssey BEFORE PRIVATE "${SWITCH_PLATFORM}/stub")
    # E3: heap fiber stack + canary guard, checked in isolation.
    add_executable(fiber_canary_test "${CMAKE_SOURCE_DIR}/tests/fiber_canary_test.c"
        "${RUNNER_ROOT}/fiber_compat.c")
    target_compile_definitions(fiber_canary_test PRIVATE GENESIS_SWITCH_STUBS=1)
    target_include_directories(fiber_canary_test PRIVATE "${RUNNER_ROOT}")
endif()

# macOS app (tools/build-mac.sh): "Arcus Odyssey DX.app" with the full-screen ROM. Settings and
# saves go to ~/Library/Application Support/Arcus Odyssey DX/ (the bundle stays read-only); the
# script bundles SDL2, adds the icon and signs it ad hoc. Tests keep using build-tall.
option(ARCUS_MAC_APP "Build the macOS app bundle" OFF)
if(APPLE AND ARCUS_MAC_APP)
    set(ARCUS_APP_NAME "Arcus Odyssey DX")
    set_target_properties(ArcusOdyssey PROPERTIES
        MACOSX_BUNDLE TRUE
        OUTPUT_NAME "${ARCUS_APP_NAME}"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/platform/macos/Info.plist.in"
        MACOSX_BUNDLE_BUNDLE_NAME "${ARCUS_APP_NAME}"
        MACOSX_BUNDLE_GUI_IDENTIFIER "local.arcus-odyssey-dx"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_ICON_FILE "arcus-odyssey-dx.icns")
    target_compile_definitions(ArcusOdyssey PRIVATE GENESIS_MAC_APP="${ARCUS_APP_NAME}")
endif()

# Linux package (tools/build-linux.sh): the full-screen ROM as "arcus-odyssey-dx". Settings and
# saves go to ~/.local/share/Arcus Odyssey DX/ ($XDG_DATA_HOME), not next to the program, so it
# can live anywhere (an unpacked folder, /opt, a read-only mount). libgcc and libstdc++ are
# linked in so only glibc is needed.
if(ARCUS_LINUX_APP AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set_target_properties(ArcusOdyssey PROPERTIES OUTPUT_NAME "arcus-odyssey-dx")
    target_compile_definitions(ArcusOdyssey PRIVATE GENESIS_PREF_APP="Arcus Odyssey DX")
    target_link_options(ArcusOdyssey PRIVATE -static-libgcc -static-libstdc++)
endif()

if(NINTENDO_SWITCH)
    # Icon: a 256x256 JPEG made from a title-screen capture by
    # tools/build-switch.sh (derived from game graphics, so it stays in out/).
    set(ARCUS_NRO_ICON "${CMAKE_SOURCE_DIR}/out/icon.jpg" CACHE FILEPATH "256x256 JPEG icon")
    nx_generate_nacp(OUTPUT "${CMAKE_BINARY_DIR}/arcus-odyssey-dx.nacp"
        NAME "Arcus Odyssey DX" AUTHOR "sixtyeightk" VERSION "${PROJECT_VERSION}")
    if(EXISTS "${ARCUS_NRO_ICON}")
        nx_create_nro(ArcusOdyssey OUTPUT "${CMAKE_BINARY_DIR}/arcus-odyssey-dx.nro"
            ICON "${ARCUS_NRO_ICON}" NACP "${CMAKE_BINARY_DIR}/arcus-odyssey-dx.nacp")
    else()
        message(WARNING "No icon at ${ARCUS_NRO_ICON}; using the libnx default icon")
        nx_create_nro(ArcusOdyssey OUTPUT "${CMAKE_BINARY_DIR}/arcus-odyssey-dx.nro"
            NACP "${CMAKE_BINARY_DIR}/arcus-odyssey-dx.nacp")
    endif()
endif()
