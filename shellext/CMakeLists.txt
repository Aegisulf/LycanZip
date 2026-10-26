add_library(LycanZipShell SHARED shellext.cpp LycanZipShell.rc shellext.def ${CMAKE_SOURCE_DIR}/src/core/fileicons.cpp)
target_link_libraries(LycanZipShell PRIVATE ole32 shell32 shlwapi runtimeobject)
target_include_directories(LycanZipShell PRIVATE ${CMAKE_SOURCE_DIR}/src/core/include)
# the version resource changes with version.h (a resource script does not report the headers it includes, so say it here)
set_source_files_properties(LycanZipShell.rc PROPERTIES OBJECT_DEPENDS ${CMAKE_SOURCE_DIR}/src/core/include/lycan/version.h)
set_target_properties(LycanZipShell PROPERTIES OUTPUT_NAME "LycanZipShell-${LYCAN_VERSION_TEXT}")   # the version is part of the file name
# No dependency on Core and none beyond the C++ runtime: this DLL is loaded into explorer.exe
# The C++ runtime is linked in (/MT): explorer.exe does not look in the folder of the DLL for vcruntime140.dll, and a computer without the Visual C++
# Redistributable (a clean Windows 10) would silently not load the menu.
set_property(TARGET LycanZipShell PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_include_directories(LycanZipShell PRIVATE ${CMAKE_SOURCE_DIR}/src/core/include)   # the version resource
