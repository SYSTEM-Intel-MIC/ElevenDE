cmake_minimum_required(VERSION 3.16)
project(sas-screen VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

if(POLICY CMP0077)
    cmake_policy(SET CMP0077 NEW)
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets Network DBus)

if(QT_VERSION VERSION_GREATER_EQUAL 6.3)
    qt_standard_project_setup()
endif()

qt_add_executable(sas-screen
    src/main.cpp
    src/commandmap.cpp
    src/networkdialog.cpp
    src/saswindow.cpp
    src/x11grabber.cpp
)

target_link_libraries(sas-screen PRIVATE Qt6::Widgets Qt6::Network Qt6::DBus)

find_package(XCB REQUIRED)
target_compile_definitions(sas-screen PRIVATE SAS_HAS_XCB=1)
target_link_libraries(sas-screen PRIVATE XCB::XCB)

install(TARGETS sas-screen RUNTIME DESTINATION bin)
install(FILES config/sas-screen.json DESTINATION share/sas-screen)