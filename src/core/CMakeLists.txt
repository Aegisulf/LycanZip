add_library(lycan_core STATIC
  sevenzip.cpp
  archive_source.cpp
  security.cpp
  extract.cpp
  util.cpp
  encoding.cpp
  compress.cpp
  probe.cpp
  scan.cpp
  diag.cpp
  datalocation.cpp
  hash.cpp
  checksum.cpp
  defender.cpp
  taskqueue.cpp
  taskexec.cpp
  workerproc.cpp
  passwords.cpp
  repack.cpp
  settings.cpp
  presets.cpp
  launch.cpp
  shellreg.cpp
  fileassoc.cpp
  fileicons.cpp
  tree.cpp
  i18n.cpp
  crashreport.cpp
  audiotags.cpp
  tempdir.cpp
  iso.cpp
  versioncheck.cpp
  updatefeed.cpp
)
target_include_directories(lycan_core
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${LYCAN_7ZIP_SRC}/CPP ${LYCAN_7ZIP_SRC}/C)
target_link_libraries(lycan_core PUBLIC oleaut32 ole32 shell32 shlwapi advapi32 cabinet wevtapi bcrypt winhttp crypt32)
if(LYCAN_7Z_PIN_SHA256)
  target_compile_definitions(lycan_core PRIVATE LYCAN_PINNED_7Z_SHA256="${LYCAN_7Z_PIN_SHA256}")
endif()
# The 7-Zip headers give many warnings at /W4; relax them for this target only
if(MSVC)
  target_compile_options(lycan_core PRIVATE /wd4100 /wd4505)
endif()
