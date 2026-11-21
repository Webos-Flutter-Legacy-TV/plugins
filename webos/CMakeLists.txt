cmake_minimum_required(VERSION 3.10)
set(PROJECT_NAME "webos_native_player")
project(${PROJECT_NAME} LANGUAGES C CXX)

find_package(PkgConfig)
pkg_check_modules(GLIB REQUIRED glib-2.0)

set(PLUGIN_NAME "webos_native_player_plugin")
set(FFMPEG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ffmpeg")
set(FFMPEG_LIBS
  "${FFMPEG_DIR}/lib/libavformat.so.61"
  "${FFMPEG_DIR}/lib/libavcodec.so.61"
  "${FFMPEG_DIR}/lib/libswresample.so.5"
  "${FFMPEG_DIR}/lib/libavutil.so.59"
)

# Link-time stand-in for the firmware's libplayerAPIs.so.1 (not packaged).
add_library(playerAPIs_stub SHARED "starfish_stub.cc")
set_target_properties(playerAPIs_stub PROPERTIES
  OUTPUT_NAME "playerAPIs" VERSION 1.0.0 SOVERSION 1)

add_library(${PLUGIN_NAME} SHARED
  "plugin.cc"
  "player.cc"
)
apply_standard_settings(${PLUGIN_NAME})
set_target_properties(${PLUGIN_NAME} PROPERTIES CXX_VISIBILITY_PRESET hidden)
target_compile_definitions(${PLUGIN_NAME} PRIVATE FLUTTER_PLUGIN_IMPL)
target_include_directories(${PLUGIN_NAME} INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(${PLUGIN_NAME} PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${FFMPEG_DIR}/include"
  ${GLIB_INCLUDE_DIRS}
)
target_link_libraries(${PLUGIN_NAME} PRIVATE
  flutter
  playerAPIs_stub
  ${FFMPEG_LIBS}
  ${GLIB_LIBRARIES}
  pthread
)

# Shipped next to the plugin in the app's lib/ (LGPL: shared, replaceable).
set(webos_native_player_bundled_libraries ${FFMPEG_LIBS} PARENT_SCOPE)
