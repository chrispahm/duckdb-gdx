cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME duckdb_gdx)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src/include)

set(NO_TESTS ON CACHE BOOL "Skip building unit tests" FORCE)
set(NO_EXAMPLES ON CACHE BOOL "Skip building example programs/executables" FORCE)
set(NO_TOOLS ON CACHE BOOL "Skip building GDX tools" FORCE)

add_subdirectory(gdx)

set(EXTENSION_SOURCES
  src/gdx_extension.cpp
  src/gdx_read_function.cpp
  src/gdx_symbols_function.cpp
  src/gdx_random_access_adapter.cpp
  src/gdx_file_provider.cpp
  src/gdx_handle.cpp
  src/gdx_error.cpp
  src/gdx_symbol_utils.cpp
)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} "" ${EXTENSION_SOURCES})

target_link_libraries(${EXTENSION_NAME} $<BUILD_INTERFACE:gdx-static>)
target_link_libraries(${LOADABLE_EXTENSION_NAME} $<BUILD_INTERFACE:gdx-static>)

target_compile_features(${EXTENSION_NAME} PUBLIC cxx_std_17)
target_compile_features(${LOADABLE_EXTENSION_NAME} PUBLIC cxx_std_17)

target_include_directories(${EXTENSION_NAME} PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/gdx/src
  ${CMAKE_CURRENT_SOURCE_DIR}/gdx/generated
  ${CMAKE_CURRENT_SOURCE_DIR}/gdx/src/gdlib
)

target_include_directories(${LOADABLE_EXTENSION_NAME} PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/gdx/src
  ${CMAKE_CURRENT_SOURCE_DIR}/gdx/generated
  ${CMAKE_CURRENT_SOURCE_DIR}/gdx/src/gdlib
)

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")

install(
  TARGETS gdx-static
  EXPORT "${DUCKDB_EXPORT_SET}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}"
)
