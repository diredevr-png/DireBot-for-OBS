cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

# OBS's window: when it has loaded and when it closes, and the stream's
# dropped frames for the usage report.
find_package(obs-frontend-api REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)

target_compile_features(${CMAKE_PROJECT_NAME} PRIVATE cxx_std_17)
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE src deps)

if(WIN32)
  target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE _WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN NOMINMAX)
  # Sockets for the link, and Windows' random numbers for its password.
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ws2_32 bcrypt)
  # Windows' data protection, which locks stream keys to the Windows account.
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE crypt32)
endif()

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.cpp
    src/link/crypto.cpp
    src/link/ws.cpp
    src/link/protocol.cpp
    src/link/server.cpp
    src/obs/websocket.cpp
    src/obs/meters.cpp
    src/obs/usage.cpp
    src/obs/link-file.cpp
    src/obs/address.cpp
    src/obs/keystore.cpp
    src/obs/settings-window.cpp
    src/obs/recording.cpp
)

# The vertical picture (src/vertical).
target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/vertical/rules.cpp
    src/vertical/options.cpp
    src/vertical/canvas.cpp
    src/vertical/outputs.cpp
    src/vertical/settings.cpp
    src/vertical/vertical.cpp
)

# "Follow me": a crop of your camera that keeps you in the middle.
target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/follow/tracker.cpp src/follow/follow-cam.cpp)

# Multistream: its own outputs to YouTube, Kick and the rest (their stream
# keys are typed into its window in OBS, locked like the vertical ones).
target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/multistream/multistream.cpp src/multistream/places.cpp)

# 0.3.0: clean recordings, DireBot Move and DireBot Blur.
target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/record/rules.cpp
    src/record/record.cpp
    src/moves/plan.cpp
    src/moves/move-transition.cpp
    src/filters/blur-filter.cpp
)

# 0.4.0: the face avatar - your real eyes and mouth on a character.
target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/face/tracker.cpp src/face/face-avatar.cpp)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
