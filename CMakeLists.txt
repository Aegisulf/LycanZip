cmake_minimum_required(VERSION 3.25)
project(LycanZip VERSION 0.0.1 LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# The version (the file VERSION is the single source). The context menu DLL has it in its file name (LycanZipShell-<version>.dll): an update
# then never has to overwrite a DLL that Explorer has loaded (see src/core/include/lycan/shellreg.h).
file(READ "${CMAKE_SOURCE_DIR}/VERSION" LYCAN_VERSION_RAW)
string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" LYCAN_VERSION_TEXT "${LYCAN_VERSION_RAW}")
if(NOT LYCAN_VERSION_TEXT)
  message(FATAL_ERROR "VERSION does not hold a version like 1.2.3")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/VERSION")

# 7-Zip sources (only the headers are used, its .cpp files are not compiled) are downloaded by scripts/fetch-7zip.ps1
set(LYCAN_7ZIP_SRC "${CMAKE_SOURCE_DIR}/third_party/7zip" CACHE PATH "7-Zip source root")
if(NOT EXISTS "${LYCAN_7ZIP_SRC}/CPP/7zip/Archive/IArchive.h")
  message(FATAL_ERROR "7-Zip sources not found, run scripts/fetch-7zip.ps1 first")
endif()

# The 7z.dll used at run time (not distributed with the repo). Order: -DLYCAN_7Z_DLL, the environment variable, common install places
set(LYCAN_7Z_DLL "" CACHE FILEPATH "Path to 7z.dll copied next to the binaries for development")
if(NOT LYCAN_7Z_DLL)
  # Prefer the DLL of the same version as the headers, got with scripts/fetch-7zip.ps1 -Binaries; only then fall back to a locally installed 7-Zip
  find_file(LYCAN_7Z_DLL_FOUND 7z.dll NO_DEFAULT_PATH
    HINTS "${CMAKE_SOURCE_DIR}/third_party/7zip-bin")
  if(NOT LYCAN_7Z_DLL_FOUND)
    find_file(LYCAN_7Z_DLL_FOUND 7z.dll
      HINTS ENV LYCAN_7Z_DLL "$ENV{ProgramFiles}/7-Zip" "$ENV{USERPROFILE}/scoop/apps/7zip/current")
  endif()
  if(LYCAN_7Z_DLL_FOUND)
    set(LYCAN_7Z_DLL "${LYCAN_7Z_DLL_FOUND}")
  endif()
endif()

# A build with the 7z.dll of third_party/7zip-bin (the one a release ships) remembers its hash and loads no other 7z.dll (src/core/sevenzip.cpp)
set(LYCAN_7Z_PIN_SHA256 "")
if(LYCAN_7Z_DLL MATCHES "third_party/7zip-bin/7z\\.dll$")
  file(SHA256 "${LYCAN_7Z_DLL}" LYCAN_7Z_PIN_SHA256)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${LYCAN_7Z_DLL}")
endif()

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8 /Zc:__cplusplus)
  # Note: WIN32_LEAN_AND_MEAN must not be defined, the 7-Zip headers need the full OLE / BSTR / PROPVARIANT declarations
  add_compile_definitions(UNICODE _UNICODE NOMINMAX _CRT_SECURE_NO_WARNINGS)
endif()

# Robustness builds (MSVC): AddressSanitizer for everything (use a Release config: /RTC1 is incompatible), libFuzzer targets
option(LYCAN_ASAN "Build with AddressSanitizer" OFF)
option(LYCAN_FUZZ "Build the libFuzzer targets in tests/fuzz" OFF)
if(MSVC AND (LYCAN_ASAN OR LYCAN_FUZZ))
  add_compile_options(/fsanitize=address /Zi)
  add_link_options(/INCREMENTAL:NO)
endif()

add_subdirectory(src/core)
add_subdirectory(src/cli)
add_subdirectory(shellext)

enable_testing()
add_subdirectory(tests)
if(LYCAN_FUZZ)
  add_subdirectory(tests/fuzz)
endif()
