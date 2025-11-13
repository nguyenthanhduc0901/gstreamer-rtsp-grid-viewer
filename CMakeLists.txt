cmake_minimum_required(VERSION 3.20)

project(gstreamer_demo LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)

# On Windows, prefer static runtime for MSVC if desired (optional)
# set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Try to find GStreamer via pkg-config (works if pkg-config is available)
include(CheckIncludeFiles)
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
  pkg_check_modules(GST QUIET gstreamer-1.0>=1.18 gstreamer-video-1.0 gobject-2.0 glib-2.0)
  if(UNIX AND NOT APPLE)
    pkg_check_modules(GTK3 QUIET gtk+-3.0)
  endif()
endif()

# Fallback: use Windows GStreamer MSI environment variable
# Typical default: C:\\gstreamer\\1.0\\msvc_x86_64
if(NOT GST_FOUND)
  message(STATUS "pkg-config not found or GStreamer not found via pkg-config; trying Windows MSI layout")
  set(GSTREAMER_ROOT "$ENV{GSTREAMER_1_0_ROOT_X86_64}")
  if(NOT GSTREAMER_ROOT)
    set(GSTREAMER_ROOT "$ENV{GSTREAMER_1_0_ROOT_X86}")
  endif()
  if(GSTREAMER_ROOT)
    message(STATUS "Using GStreamer root: ${GSTREAMER_ROOT}")
    set(GST_INCLUDE_DIRS
      ${GSTREAMER_ROOT}/include/gstreamer-1.0
      ${GSTREAMER_ROOT}/include
      ${GSTREAMER_ROOT}/lib/gstreamer-1.0/include
    )
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
      set(GST_LIB_DIR ${GSTREAMER_ROOT}/lib)
    else()
      set(GST_LIB_DIR ${GSTREAMER_ROOT}/lib)
    endif()
    set(GST_LIBRARIES
      ${GST_LIB_DIR}/gstreamer-1.0.lib
      ${GST_LIB_DIR}/gstvideo-1.0.lib
      ${GST_LIB_DIR}/gobject-2.0.lib
      ${GST_LIB_DIR}/glib-2.0.lib
      ${GST_LIB_DIR}/gstbase-1.0.lib
    )
    set(GST_FOUND TRUE)
  else()
    message(WARNING "GSTREAMER_1_0_ROOT_X86_64 environment variable not set. You can set CMAKE_PREFIX_PATH to point to your GStreamer root and reconfigure.")
  endif()
endif()

if(NOT GST_FOUND)
  message(FATAL_ERROR "GStreamer not found. Install GStreamer (Runtime + Development) and ensure either pkg-config works or GSTREAMER_1_0_ROOT_X86_64 is set.")
endif()

# Choose platform-specific main
if (WIN32)
  set(APP_SOURCES src/main.cpp)
else()
  set(APP_SOURCES src/main_pi.cpp)
endif()

add_executable(gstreamer_demo ${APP_SOURCES})

target_include_directories(gstreamer_demo PRIVATE
  ${GST_INCLUDE_DIRS}
)

if(PKG_CONFIG_FOUND AND GST_FOUND)
  # Use compile OPTIONS (not DEFINITIONS) to avoid turning flags like -pthread into -D-pthread
  if(GST_CFLAGS_OTHER)
    target_compile_options(gstreamer_demo PRIVATE ${GST_CFLAGS_OTHER})
  endif()
  target_include_directories(gstreamer_demo PRIVATE ${GST_INCLUDE_DIRS})
  if(GST_LIBRARY_DIRS)
    target_link_directories(gstreamer_demo PRIVATE ${GST_LIBRARY_DIRS})
  endif()
endif()

# Link against GStreamer and Windows system libs
if(GST_LIBRARIES)
  target_link_libraries(gstreamer_demo PRIVATE ${GST_LIBRARIES})
else()
  # For pkg-config case, use the imported names
  target_link_libraries(gstreamer_demo PRIVATE ${GST_LDFLAGS} ${GST_LDFLAGS_OTHER})
endif()

# GTK3 (Linux) for gtksink UI
if(UNIX AND NOT APPLE AND GTK3_FOUND)
  target_include_directories(gstreamer_demo PRIVATE ${GTK3_INCLUDE_DIRS})
  if(GTK3_CFLAGS_OTHER)
    target_compile_options(gstreamer_demo PRIVATE ${GTK3_CFLAGS_OTHER})
  endif()
  if(GTK3_LIBRARY_DIRS)
    target_link_directories(gstreamer_demo PRIVATE ${GTK3_LIBRARY_DIRS})
  endif()
  target_link_libraries(gstreamer_demo PRIVATE ${GTK3_LIBRARIES})
endif()

# X11 link no longer required when using GTK + gtksink; keep optional if present
if(UNIX AND NOT APPLE)
  find_package(X11 QUIET)
  if(X11_FOUND)
    target_link_libraries(gstreamer_demo PRIVATE X11::X11)
  endif()
endif()

# Win32 UI libs
if(MSVC OR WIN32)
  target_compile_definitions(gstreamer_demo PRIVATE UNICODE _UNICODE)
  target_link_libraries(gstreamer_demo PRIVATE user32.lib gdi32.lib)
endif()

# Helpful output dir
set_target_properties(gstreamer_demo PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# Additional Linux target with SW decoder forcing (avdec_h265): gstreamer_demo_swdec
if(UNIX AND NOT WIN32)
  add_executable(gstreamer_demo_swdec src/main_pi_swdec.cpp)
  target_include_directories(gstreamer_demo_swdec PRIVATE ${GST_INCLUDE_DIRS})
  if(PKG_CONFIG_FOUND AND GST_FOUND)
    if(GST_CFLAGS_OTHER)
      target_compile_options(gstreamer_demo_swdec PRIVATE ${GST_CFLAGS_OTHER})
    endif()
    target_include_directories(gstreamer_demo_swdec PRIVATE ${GST_INCLUDE_DIRS})
    if(GST_LIBRARY_DIRS)
      target_link_directories(gstreamer_demo_swdec PRIVATE ${GST_LIBRARY_DIRS})
    endif()
  endif()
  if(GST_LIBRARIES)
    target_link_libraries(gstreamer_demo_swdec PRIVATE ${GST_LIBRARIES})
  else()
    target_link_libraries(gstreamer_demo_swdec PRIVATE ${GST_LDFLAGS} ${GST_LDFLAGS_OTHER})
  endif()
  if(UNIX AND NOT APPLE AND GTK3_FOUND)
    target_include_directories(gstreamer_demo_swdec PRIVATE ${GTK3_INCLUDE_DIRS})
    if(GTK3_CFLAGS_OTHER)
      target_compile_options(gstreamer_demo_swdec PRIVATE ${GTK3_CFLAGS_OTHER})
    endif()
    if(GTK3_LIBRARY_DIRS)
      target_link_directories(gstreamer_demo_swdec PRIVATE ${GTK3_LIBRARY_DIRS})
    endif()
    target_link_libraries(gstreamer_demo_swdec PRIVATE ${GTK3_LIBRARIES})
  endif()
  set_target_properties(gstreamer_demo_swdec PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

  # Additional Linux target: GTK-optimized Raspberry Pi viewer
  add_executable(gstreamer_demo_pi_gtk src/main_pi_gtk_opt.cpp)
  target_include_directories(gstreamer_demo_pi_gtk PRIVATE ${GST_INCLUDE_DIRS})
  if(PKG_CONFIG_FOUND AND GST_FOUND)
    if(GST_CFLAGS_OTHER)
      target_compile_options(gstreamer_demo_pi_gtk PRIVATE ${GST_CFLAGS_OTHER})
    endif()
    target_include_directories(gstreamer_demo_pi_gtk PRIVATE ${GST_INCLUDE_DIRS})
    if(GST_LIBRARY_DIRS)
      target_link_directories(gstreamer_demo_pi_gtk PRIVATE ${GST_LIBRARY_DIRS})
    endif()
  endif()
  if(GST_LIBRARIES)
    target_link_libraries(gstreamer_demo_pi_gtk PRIVATE ${GST_LIBRARIES})
  else()
    target_link_libraries(gstreamer_demo_pi_gtk PRIVATE ${GST_LDFLAGS} ${GST_LDFLAGS_OTHER})
  endif()
  if(UNIX AND NOT APPLE AND GTK3_FOUND)
    target_include_directories(gstreamer_demo_pi_gtk PRIVATE ${GTK3_INCLUDE_DIRS})
    if(GTK3_CFLAGS_OTHER)
      target_compile_options(gstreamer_demo_pi_gtk PRIVATE ${GTK3_CFLAGS_OTHER})
    endif()
    if(GTK3_LIBRARY_DIRS)
      target_link_directories(gstreamer_demo_pi_gtk PRIVATE ${GTK3_LIBRARY_DIRS})
    endif()
    target_link_libraries(gstreamer_demo_pi_gtk PRIVATE ${GTK3_LIBRARIES})
  endif()
  set_target_properties(gstreamer_demo_pi_gtk PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()
