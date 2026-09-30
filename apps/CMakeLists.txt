cmake_minimum_required(VERSION 3.16)
project(elevende-apps VERSION 3.5.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_AUTOMOC ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets Network)
find_package(X11)

# target name -> source dir (elevende-calc keeps the short Win-style name
# used by runbox aliases and the .desktop entry)
add_executable(elevende-notepad notepad/main.cpp)
add_executable(elevende-calc calculator/main.cpp)
add_executable(elevende-taskmgr taskmgr/main.cpp)
add_executable(elevende-settings settings/main.cpp settings/extra.cpp)

foreach(app elevende-notepad elevende-calc elevende-taskmgr elevende-settings)
    target_link_libraries(${app} PRIVATE Qt6::Widgets Qt6::Network)
    target_include_directories(${app} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/common)
endforeach()

# Settings links X11 for sending the wallpaper-reload client message and
# reading the root window for live theme changes.
if(X11_FOUND)
    target_link_libraries(elevende-settings PRIVATE X11::X11)
    target_compile_definitions(elevende-settings PRIVATE ELEVENDE_HAVE_X11=1)
endif()

install(TARGETS elevende-notepad elevende-calc elevende-taskmgr elevende-settings
        RUNTIME DESTINATION bin)
