cmake_minimum_required(VERSION 3.20)
project(msfs-linuxrebind VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "msfs-linuxrebind is Linux only (evdev + Wayland/X11).")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(GLFW REQUIRED IMPORTED_TARGET glfw3>=3.4)
find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)

set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)
if(NOT EXISTS ${IMGUI_DIR}/imgui.h)
  message(FATAL_ERROR "Dear ImGui submodule missing. Run: git submodule update --init --recursive")
endif()

add_library(imgui STATIC
  ${IMGUI_DIR}/imgui.cpp
  ${IMGUI_DIR}/imgui_draw.cpp
  ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/imgui_demo.cpp
  ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
  ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
  ${IMGUI_DIR}/misc/cpp/imgui_stdlib.cpp
)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends ${IMGUI_DIR}/misc/cpp)
target_link_libraries(imgui PUBLIC PkgConfig::GLFW OpenGL::GL)

add_executable(msfs-linuxrebind
  src/main.cpp
  src/sim/SimConnectClient.cpp
  src/sim/SimBridge.cpp
  src/input/Evdev.cpp
  src/haptics/WinwingHid.cpp
  src/haptics/Haptics.cpp
  src/app/Profile.cpp
  src/app/Catalog.cpp
  src/app/Engine.cpp
  src/app/Paths.cpp
  src/ui/Ui.cpp
)
target_include_directories(msfs-linuxrebind PRIVATE src third_party)
target_link_libraries(msfs-linuxrebind PRIVATE imgui Threads::Threads)
target_compile_definitions(msfs-linuxrebind PRIVATE
  LINUXREBIND_VERSION="${PROJECT_VERSION}"
  LINUXREBIND_SOURCE_DATA_DIR="${CMAKE_SOURCE_DIR}/data"
)
target_compile_options(msfs-linuxrebind PRIVATE -Wall -Wextra -Wno-unused-parameter)

option(LINUXREBIND_STATIC_CXX "Link libstdc++ and libgcc statically (for portable AppImage builds)" OFF)
if(LINUXREBIND_STATIC_CXX)
  target_link_options(msfs-linuxrebind PRIVATE -static-libstdc++ -static-libgcc)
endif()

install(TARGETS msfs-linuxrebind RUNTIME DESTINATION bin)
install(DIRECTORY data/ DESTINATION share/msfs-linuxrebind/data)
install(FILES packaging/msfs-linuxrebind.desktop DESTINATION share/applications)
install(FILES packaging/icons/msfs-linuxrebind.svg DESTINATION share/icons/hicolor/scalable/apps)
foreach(size 16 32 48 64 128 256 512)
  install(FILES packaging/icons/msfs-linuxrebind-${size}.png DESTINATION share/icons/hicolor/${size}x${size}/apps RENAME msfs-linuxrebind.png)
endforeach()

option(LINUXREBIND_BUILD_TESTS "Build the config compatibility test (ctest)" ON)
if(LINUXREBIND_BUILD_TESTS)
  enable_testing()
  add_executable(config_roundtrip tests/config_roundtrip.cpp src/app/Profile.cpp src/input/Evdev.cpp)
  target_include_directories(config_roundtrip PRIVATE src third_party)
  target_link_libraries(config_roundtrip PRIVATE Threads::Threads)
  target_compile_definitions(config_roundtrip PRIVATE LINUXREBIND_TEST_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures")
  add_test(NAME config_roundtrip COMMAND config_roundtrip)
  # Every documented command-line option must be accepted (exit code 2 = "unknown option"). Regression test for
  # a refactor that once silently dropped --config/--host/--port/--profile/--catalog/--motor-sweep.
  add_test(NAME cli_options COMMAND bash ${CMAKE_SOURCE_DIR}/tests/cli_options.sh $<TARGET_FILE:msfs-linuxrebind>
           ${CMAKE_SOURCE_DIR}/tests/fixtures/config_v1.json)
endif()
