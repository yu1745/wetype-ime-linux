cmake_minimum_required(VERSION 3.16)
project(fcitx5-wetype VERSION 1.0 LANGUAGES CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Fcitx5Core REQUIRED)

add_library(fcitx5-wetype MODULE src/wetype.cpp)
target_link_libraries(fcitx5-wetype PRIVATE Fcitx5::Core)

install(TARGETS fcitx5-wetype LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/fcitx5)
install(FILES wetype-addon.conf RENAME wetype.conf DESTINATION ${CMAKE_INSTALL_DATADIR}/fcitx5/addon)
install(FILES wetype-im.conf DESTINATION ${CMAKE_INSTALL_DATADIR}/fcitx5/inputmethod)
