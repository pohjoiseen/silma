cmake_minimum_required(VERSION 3.24)
project(Silma VERSION 0.1.0 LANGUAGES CXX)

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

set(CMAKE_AUTOMOC ON)

add_subdirectory(src/core)
add_subdirectory(src/app)

enable_testing()
add_subdirectory(tests)

# AI models that are not downloaded but made from the original weights (NAFNet width 32,
# full precision and 8-bit). Not part of the default build: needs Python 3 and the network.
#   cmake --build <dir> --target ai-models
# The newest Python found anywhere: by default the first directory in PATH with any
# python3.X wins, which can be an old one (e.g. in ~/.local/bin).
set(Python3_FIND_STRATEGY VERSION)
find_package(Python3 COMPONENTS Interpreter QUIET)
set(SILMA_CALIBRATION_RAWS "${CMAKE_SOURCE_DIR}/example-raws" CACHE PATH "Raw files for calibrating the 8-bit AI denoise model")
if(Python3_FOUND)
  message(STATUS "ai-models target: Python ${Python3_VERSION} (${Python3_EXECUTABLE})")
  set(AI_VENV "${CMAKE_BINARY_DIR}/ai-models-venv")
  add_custom_target(ai-models
    COMMAND ${Python3_EXECUTABLE} -m venv "${AI_VENV}"
    COMMAND "${AI_VENV}/bin/pip" install --quiet -r "${CMAKE_SOURCE_DIR}/tools/ai-models-requirements.txt"
    COMMAND "${AI_VENV}/bin/python" "${CMAKE_SOURCE_DIR}/tools/prepare_ai_models.py"
            --silma $<TARGET_FILE:silma> --raws "${SILMA_CALIBRATION_RAWS}" --work "${CMAKE_BINARY_DIR}/ai-models-work"
    DEPENDS silma
    USES_TERMINAL
    COMMENT "Preparing NAFNet width-32 denoise models")
endif()

include(GNUInstallDirs)
install(FILES data/silma.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES data/silma-mime.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/mime/packages)
