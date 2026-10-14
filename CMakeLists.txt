cmake_minimum_required(VERSION 3.21)
project(Impasto VERSION 0.9.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets Svg)
find_package(ZLIB REQUIRED)   # zip for OpenRaster, deflate for Photoshop channels

file(GLOB_RECURSE IMPASTO_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/src/*.cpp
    ${CMAKE_SOURCE_DIR}/src/*.h)

qt_add_executable(impasto
    ${IMPASTO_SOURCES}
    resources/resources.qrc)

target_include_directories(impasto PRIVATE src)
target_compile_options(impasto PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_compile_definitions(impasto PRIVATE
    IMPASTO_VERSION="${PROJECT_VERSION}")

target_link_libraries(impasto PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::OpenGL Qt6::OpenGLWidgets Qt6::Svg ZLIB::ZLIB)

# Renders the app icon at the sizes desktops want (packaging/icons); built only when asked for.
add_executable(impasto-svg2png EXCLUDE_FROM_ALL tools/svg2png.cpp)
target_link_libraries(impasto-svg2png PRIVATE Qt6::Gui Qt6::Svg)

# ---------------------------------------------------------------- install
set(IMPASTO_APP_ID io.github.ozzy_apps.Impasto)
include(GNUInstallDirs)
install(TARGETS impasto RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES packaging/${IMPASTO_APP_ID}.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES packaging/${IMPASTO_APP_ID}.metainfo.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
install(FILES packaging/${IMPASTO_APP_ID}.mime.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/mime/packages RENAME ${IMPASTO_APP_ID}.xml)
install(FILES resources/icons/app.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps RENAME ${IMPASTO_APP_ID}.svg)
foreach(size 16 24 32 48 64 128 256 512)
    install(FILES packaging/icons/${size}.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps RENAME ${IMPASTO_APP_ID}.png)
endforeach()

# ---------------------------------------------------------------- .deb (cpack -G DEB)
set(CPACK_PACKAGE_NAME impasto)
set(CPACK_PACKAGE_VENDOR "ozzy-apps")
set(CPACK_PACKAGE_CONTACT "ozzy-apps <info@ozzymail.nl>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Painting app for pen tablets, in the spirit of Procreate")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/ozzy-apps/impasto")
set(CPACK_PACKAGING_INSTALL_PREFIX /usr)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SECTION graphics)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
# Qt loads these as plugins, so the library scan cannot see them.
set(CPACK_DEBIAN_PACKAGE_DEPENDS "qt6-qpa-plugins")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "qt6-wayland, qt6-image-formats-plugins")
set(CPACK_GENERATOR DEB)
include(CPack)
