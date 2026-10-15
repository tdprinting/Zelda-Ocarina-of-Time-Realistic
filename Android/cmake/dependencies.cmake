# Libraries desktop builds get from the system or vcpkg but the NDK lacks: the audio codecs custom
# music and sound effects need, and SDL_net. OVERRIDE_FIND_PACKAGE lets vorbis and opusfile find the
# ogg and opus built here.
include(FetchContent)

set(BUILD_SHARED_LIBS OFF)
set(BUILD_TESTING OFF)
set(INSTALL_DOCS OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    Ogg
    GIT_REPOSITORY https://github.com/xiph/ogg.git
    GIT_TAG v1.3.6
    OVERRIDE_FIND_PACKAGE
)
FetchContent_Declare(
    Opus
    GIT_REPOSITORY https://github.com/xiph/opus.git
    GIT_TAG v1.5.2
    OVERRIDE_FIND_PACKAGE
)
FetchContent_MakeAvailable(Ogg Opus)

FetchContent_Declare(
    Vorbis
    GIT_REPOSITORY https://github.com/xiph/vorbis.git
    GIT_TAG v1.3.7
    OVERRIDE_FIND_PACKAGE
)
FetchContent_Declare(
    OpusFile
    GIT_REPOSITORY https://github.com/xiph/opusfile.git
    GIT_TAG v0.12
)
FetchContent_MakeAvailable(Vorbis)

# opusfile's last release predates its CMake support, so build the decoder half by hand
FetchContent_GetProperties(OpusFile)
if(NOT opusfile_POPULATED)
    FetchContent_Populate(OpusFile)
endif()
add_library(opusfile STATIC
    ${opusfile_SOURCE_DIR}/src/info.c
    ${opusfile_SOURCE_DIR}/src/internal.c
    ${opusfile_SOURCE_DIR}/src/opusfile.c
    ${opusfile_SOURCE_DIR}/src/stream.c
)
target_include_directories(opusfile PUBLIC ${opusfile_SOURCE_DIR}/include)
target_link_libraries(opusfile PUBLIC Ogg::ogg Opus::opus)
target_compile_options(opusfile PRIVATE -w)
add_library(OpusFile::opusfile ALIAS opusfile)

# SDL_net for the online features in soh/Network, built against the SDL libultraship fetched
set(SDL2NET_INSTALL OFF CACHE BOOL "" FORCE)
set(SDL2NET_SAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    SDL2_net
    GIT_REPOSITORY https://github.com/libsdl-org/SDL_net.git
    GIT_TAG release-2.2.0
)
FetchContent_MakeAvailable(SDL2_net)
# soh includes it as <SDL2/SDL_net.h>, the layout an installed SDL_net has
file(COPY ${sdl2_net_SOURCE_DIR}/SDL_net.h DESTINATION ${sdl2_net_BINARY_DIR}/include/SDL2)
target_include_directories(SDL2_net INTERFACE ${sdl2_net_BINARY_DIR}/include)
