cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME duckdb_gdx)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

# set(DUCKDB_EXTENSION_DUCKDB_GDX_ADDITIONAL_WASM_EXPORTS "\"_duckdb_gdx_wasm_set_http_header\",\"_duckdb_gdx_wasm_clear_http_headers\",\"_duckdb_gdx_wasm_set_load_path\", \"DLLLoadPath\"")
if(EMSCRIPTEN)
  # _LINKED_LIBS influences only Wasm compilation it's unclear why this is
  # needed, but somehow the global symbol pj_release from PROJ is not properly
  # exported otherwise this solves by basically re-linking (at the moment the
  # Wasm binary is actually produced)
  set(DUCKDB_EXTENSION_DUCKDB_GDX_LINKED_LIBS
      # "${CMAKE_CURRENT_SOURCE_DIR}/gdx/build-wasm/libgdx-static.a")
      "${CMAKE_CURRENT_SOURCE_DIR}/gdx/build-wasm_2/libgdx-static.a")
endif()

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
  src/gdx_wasm_support.cpp
  src/gdx_metadata.cpp
  src/gdx_metadata_cache.cpp
  src/gdx_preload_pragma.cpp
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

if(EMSCRIPTEN)
  target_link_options(${EXTENSION_NAME} PRIVATE "-sFETCH=1")
  target_link_options(${LOADABLE_EXTENSION_NAME} PRIVATE "-sFETCH=1")
endif()

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

if(DEFINED ENABLE_UNITTEST_CPP_TESTS AND ENABLE_UNITTEST_CPP_TESTS)
  set(DUCKDB_GDX_UNITTEST_SOURCES
      test/unit/gdx/test_gdx_symbol_utils.cpp
      test/unit/gdx/test_random_access_adapter.cpp)

  add_library_unity(duckdb_gdx_unittest OBJECT ${DUCKDB_GDX_UNITTEST_SOURCES})
  target_include_directories(duckdb_gdx_unittest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include
    ${CMAKE_CURRENT_SOURCE_DIR}/gdx/generated
    ${CMAKE_CURRENT_SOURCE_DIR}/gdx/src
    ${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party/catch
    ${CMAKE_CURRENT_SOURCE_DIR}/duckdb/test/include
  )

  set(ALL_OBJECT_FILES
      ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_gdx_unittest>
      PARENT_SCOPE)
endif()
