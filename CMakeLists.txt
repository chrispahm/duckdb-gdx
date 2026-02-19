cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME duckdb_gdx)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src/include)

# Workaround for GCC 14+ / ld 2.41+ with DuckDB unity builds:
# constexpr static members (e.g. LogicalType::VARCHAR) get emitted as strong symbols in
# DuckDB's unity translation units and as COMDAT symbols in separately-compiled extension
# object files.  The linker rejects the duplication.  --allow-multiple-definition lets the
# linker merge these identical definitions, which is safe because they originate from the
# same inline constexpr declarations.
if(NOT APPLE AND NOT WIN32 AND NOT EMSCRIPTEN AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--allow-multiple-definition" CACHE STRING "" FORCE)
  set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--allow-multiple-definition" CACHE STRING "" FORCE)
endif()

set(NO_TESTS ON CACHE BOOL "Skip building unit tests" FORCE)
set(NO_EXAMPLES ON CACHE BOOL "Skip building example programs/executables" FORCE)
set(NO_TOOLS ON CACHE BOOL "Skip building GDX tools" FORCE)

# MINIMAL_WASM_DEBUG: Set to ON to build a minimal extension without GDX library
# This helps debug WASM loading issues by isolating the problem
option(MINIMAL_WASM_DEBUG "Build minimal extension without GDX library" OFF)

if(NOT MINIMAL_WASM_DEBUG)
  add_subdirectory(gdx)
endif()

if(EMSCRIPTEN AND NOT MINIMAL_WASM_DEBUG)
  # _LINKED_LIBS influences only Wasm compilation - the GDX static library must be
  # re-linked when the WASM binary is produced, otherwise its symbols remain as
  # unresolved imports which causes the extension to hang during LOAD.
  # Use generator expression to get the actual library path at build time.
  # Note: zlib is bundled in gdx-static when gdx/zlib source files are present.
  set(DUCKDB_EXTENSION_DUCKDB_GDX_LINKED_LIBS
       "$<TARGET_FILE:gdx-static>")
endif()

if(MINIMAL_WASM_DEBUG)
  # Minimal extension - only the extension entry point
  set(EXTENSION_SOURCES
    src/gdx_extension.cpp
  )
else()
  set(EXTENSION_SOURCES
    src/gdx_extension.cpp
    src/gdx_read_function.cpp
    src/gdx_symbols_function.cpp
    src/gdx_domain_values_function.cpp
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
  
  # Sidecar feature requires nlohmann JSON, not available in WASM
  if(NOT EMSCRIPTEN)
    list(APPEND EXTENSION_SOURCES src/gdx_sidecar.cpp)
  endif()
endif()

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} "" ${EXTENSION_SOURCES})

if(NOT MINIMAL_WASM_DEBUG)
  target_link_libraries(${EXTENSION_NAME} $<BUILD_INTERFACE:gdx-static>)
  target_link_libraries(${LOADABLE_EXTENSION_NAME} $<BUILD_INTERFACE:gdx-static>)
endif()

target_compile_features(${EXTENSION_NAME} PUBLIC cxx_std_17)
target_compile_features(${LOADABLE_EXTENSION_NAME} PUBLIC cxx_std_17)

if(NOT MINIMAL_WASM_DEBUG)
  target_include_directories(${EXTENSION_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/gdx/src
    ${CMAKE_CURRENT_SOURCE_DIR}/gdx/generated
    ${CMAKE_CURRENT_SOURCE_DIR}/gdx/src/gdlib
    ${CMAKE_CURRENT_SOURCE_DIR}/gdx/zlib
  )

  target_include_directories(${LOADABLE_EXTENSION_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/gdx/src
    ${CMAKE_CURRENT_SOURCE_DIR}/gdx/generated
    ${CMAKE_CURRENT_SOURCE_DIR}/gdx/src/gdlib
    ${CMAKE_CURRENT_SOURCE_DIR}/gdx/zlib
  )
endif()

if(EMSCRIPTEN)
  # Disable HTTP fetching in WASM - DuckDB-WASM doesn't provide emscripten_fetch imports
  # HTTP URLs should be handled by JavaScript and passed to the extension via registerFileBuffer
  target_compile_definitions(${EXTENSION_NAME} PRIVATE DUCKDB_GDX_NO_WASM_HTTP=1)
  target_compile_definitions(${LOADABLE_EXTENSION_NAME} PRIVATE DUCKDB_GDX_NO_WASM_HTTP=1)
endif()

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")

if(NOT MINIMAL_WASM_DEBUG)
  install(
    TARGETS gdx-static
    EXPORT "${DUCKDB_EXPORT_SET}"
    ARCHIVE DESTINATION "${INSTALL_LIB_DIR}"
  )
endif()

if(DEFINED ENABLE_UNITTEST_CPP_TESTS AND ENABLE_UNITTEST_CPP_TESTS AND NOT MINIMAL_WASM_DEBUG)
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
