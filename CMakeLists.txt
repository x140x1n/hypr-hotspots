cmake_minimum_required(VERSION 3.30)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(pixman REQUIRED pixman-1)
pkg_check_modules(hyprland REQUIRED hyprland)
pkg_check_modules(libdrm REQUIRED libdrm)
pkg_check_modules(xkbcommon REQUIRED xkbcommon)

project(hypr-hotspots)

add_library(hypr-hotspots SHARED Main.cpp)
set_target_properties(hypr-hotspots PROPERTIES PREFIX "")

target_include_directories(hypr-hotspots PRIVATE 
    ${pixman_INCLUDE_DIRS} 
    ${hyprland_INCLUDE_DIRS} 
    ${libdrm_INCLUDE_DIRS}
    ${xkbcommon_INCLUDE_DIRS}
)

target_link_libraries(hypr-hotspots 
    ${pixman_LIBRARIES}
    ${hyprland_LIBRARIES} 
    ${libdrm_LIBRARIES}
    ${xkbcommon_LIBRARIES}
)

target_compile_definitions(hypr-hotspots PRIVATE 
    WLR_USE_UNSTABLE
)

target_compile_options(hypr-hotspots PRIVATE 
    -Wall -Wextra -Wno-unused-parameter 
    -Wno-unused-value -Wno-missing-field-initializers 
    -Wno-narrowing
)
