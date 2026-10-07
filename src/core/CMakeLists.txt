file(GLOB CORE_SOURCES CONFIGURE_DEPENDS *.cpp *.h)
add_library(silmacore STATIC ${CORE_SOURCES})
target_include_directories(silmacore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(silmacore PUBLIC Qt6::Core Qt6::Gui Qt6::Concurrent OpenMP::OpenMP_CXX
  PkgConfig::LIBRAW PkgConfig::EXIV2 PkgConfig::LENSFUN PkgConfig::JPEG PkgConfig::TIFF PkgConfig::SQLITE PkgConfig::LCMS)
target_compile_definitions(silmacore PUBLIC SILMA_VERSION="${PROJECT_VERSION}")
