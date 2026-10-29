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
)

# Multistream: its own outputs to YouTube, Kick and the rest, and the
# window in OBS where their stream keys are typed (kept locked to the
# Windows account with crypt32).
target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE src/multistream/multistream.cpp src/multistream/places.cpp src/obs/keystore.cpp src/obs/settings-window.cpp
)
if(WIN32)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE crypt32)
endif()

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
