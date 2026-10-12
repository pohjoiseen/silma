cmake_minimum_required(VERSION 3.24)
project(Silma VERSION 1.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O3 -g -DNDEBUG")
add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -march=x86-64-v3)
# Keep the build warning-free. Packagers on newer compilers can opt out with
# `cmake --compile-no-warning-as-error` or -DCMAKE_COMPILE_WARNING_AS_ERROR=OFF.
if(NOT DEFINED CMAKE_COMPILE_WARNING_AS_ERROR)
  set(CMAKE_COMPILE_WARNING_AS_ERROR ON)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Concurrent Network DBus Test)
find_library(ONNXRUNTIME_LIB onnxruntime REQUIRED)
find_package(OpenCV REQUIRED COMPONENTS core imgproc features stitching photo video)
find_package(PkgConfig REQUIRED)
find_package(OpenMP REQUIRED)
pkg_check_modules(LIBRAW REQUIRED IMPORTED_TARGET libraw_r)
pkg_check_modules(EXIV2 REQUIRED IMPORTED_TARGET exiv2)
pkg_check_modules(LENSFUN REQUIRED IMPORTED_TARGET lensfun)
pkg_check_modules(JPEG REQUIRED IMPORTED_TARGET libjpeg)
pkg_check_modules(TIFF REQUIRED IMPORTED_TARGET libtiff-4)
pkg_check_modules(SQLITE REQUIRED IMPORTED_TARGET sqlite3)
pkg_check_modules(LCMS REQUIRED IMPORTED_TARGET lcms2)
pkg_check_modules(JXL REQUIRED IMPORTED_TARGET libjxl libjxl_cms)
pkg_check_modules(HEIF REQUIRED IMPORTED_TARGET libheif)

set(CMAKE_AUTOMOC ON)

add_subdirectory(src/core)
add_subdirectory(src/app)

enable_testing()
add_subdirectory(tests)

include(GNUInstallDirs)
install(FILES data/silma.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES data/silma-mime.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/mime/packages)
install(FILES data/silma.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
