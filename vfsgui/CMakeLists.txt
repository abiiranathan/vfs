cmake_minimum_required(VERSION 3.16)
project(VFSExplorer LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find Qt6 components
find_package(Qt6 REQUIRED COMPONENTS Widgets)
qt_standard_project_setup()

# Thread mapping required by pthread in vfs.c
find_package(Threads REQUIRED)

add_executable(VFSExplorer
    main.cpp
    mainwindow.hpp
    mainwindow.cpp
)

target_link_libraries(VFSExplorer PRIVATE 
    Qt6::Widgets
    Threads::Threads
    vfs
)
