file(GLOB APP_SOURCES CONFIGURE_DEPENDS *.cpp *.h ../cli/*.cpp ../cli/*.h)
list(FILTER APP_SOURCES EXCLUDE REGEX ".*/main\\.cpp$")
add_library(silmaapp STATIC ${APP_SOURCES})
target_include_directories(silmaapp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../cli)
target_link_libraries(silmaapp PUBLIC silmacore Qt6::Widgets Qt6::DBus)

add_executable(silma main.cpp)
target_link_libraries(silma PRIVATE silmaapp)
# The application icon, built in for the window icon when it is not installed.
qt_add_resources(silma "icon" PREFIX "/" BASE ../../data FILES ../../data/silma.svg)
install(TARGETS silma)

# KDE color schemes (optional): lets the app default to Breeze Dark and offer the
# installed schemes like other KDE applications.
find_package(KF6ColorScheme QUIET)
if(KF6ColorScheme_FOUND)
  target_link_libraries(silmaapp PUBLIC KF6::ColorScheme)
  target_compile_definitions(silmaapp PRIVATE SILMA_HAVE_KCOLORSCHEME)
  message(STATUS "KDE color schemes: enabled")
else()
  message(STATUS "KDE color schemes: not found, using a built-in dark palette")
endif()
