cmake_minimum_required(VERSION 3.20)
project(open-opal LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# depthai-core, built as a static library with the slim option set (see README).
# Point depthai_DIR at <install prefix>/lib/cmake/depthai.
# depthai::core is static and lists its private dependencies as LINK_ONLY, so the
# consumer has to locate them; they come from depthai-core's vcpkg tree.
foreach(dep fmt spdlog yaml-cpp httplib semver magic_enum lz4 liblzma Eigen3)
    find_package(${dep} CONFIG REQUIRED)
endforeach()
find_package(BZip2 REQUIRED)
find_package(ZLIB REQUIRED)
find_package(LibArchive REQUIRED)
find_package(Threads REQUIRED)
find_package(depthai CONFIG REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Widgets)

add_executable(open-opal
    src/main.cpp
    src/mainwindow.cpp
    src/camera.cpp
    src/settings.cpp
    src/usbstate.cpp
    src/v4l2out.cpp
    src/faces.cpp
)
target_link_libraries(open-opal PRIVATE depthai::core Qt6::Widgets)
target_compile_options(open-opal PRIVATE -Wall -Wextra)

install(TARGETS open-opal RUNTIME DESTINATION bin)
