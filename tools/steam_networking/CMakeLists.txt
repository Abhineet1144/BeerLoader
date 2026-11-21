cmake_minimum_required(VERSION 3.21)
project(BeerSteamNetworkingBridge LANGUAGES C CXX)
# The Valve runtime stays outside Beer; this adds first-party integration without copying its sources.
set(GNS_SOURCE_DIR "" CACHE PATH "External GameNetworkingSockets checkout")
if(NOT EXISTS "${GNS_SOURCE_DIR}/src/steamnetworkingsockets/steamnetworkingsockets_certstore.h")
    message(FATAL_ERROR "Set GNS_SOURCE_DIR to a GameNetworkingSockets checkout")
endif()
set(BUILD_SHARED_LIB ON CACHE BOOL "" FORCE)
set(BUILD_STATIC_LIB OFF CACHE BOOL "" FORCE)
set(ENABLE_ICE ON CACHE BOOL "" FORCE)
set(USE_STEAMWEBRTC OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${GNS_SOURCE_DIR}" gns)
find_path(BEER_JSON_INCLUDE_DIR json-c/json.h REQUIRED)
find_library(BEER_JSON_LIBRARY json-c REQUIRED)
target_sources(GameNetworkingSockets PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/steam_auth_bridge.cpp")
target_include_directories(GameNetworkingSockets PRIVATE
    "${GNS_SOURCE_DIR}/src/steamnetworkingsockets"
    "${GNS_SOURCE_DIR}/src/steamnetworkingsockets/clientlib"
    "${BEER_JSON_INCLUDE_DIR}")
target_link_libraries(GameNetworkingSockets PRIVATE "${BEER_JSON_LIBRARY}")
