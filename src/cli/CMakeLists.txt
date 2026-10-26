add_executable(lycanzip main.cpp lycanzip-cli.rc)
# the version resource changes with version.h (a resource script does not report the headers it includes, so say it here)
set_source_files_properties(lycanzip-cli.rc PROPERTIES OBJECT_DEPENDS ${CMAKE_SOURCE_DIR}/src/core/include/lycan/version.h)
# The file name must not be lycanzip.exe: Windows file names are case-insensitive, so it would overwrite the GUI LycanZip.exe in the same folder (portable and MSIX keep them together)
set_target_properties(lycanzip PROPERTIES OUTPUT_NAME "lycanzip-cli")
target_link_libraries(lycanzip PRIVATE lycan_core version)
target_include_directories(lycanzip PRIVATE ${CMAKE_SOURCE_DIR}/src/core/include)   # the version resource

if(LYCAN_7Z_DLL)
  add_custom_command(TARGET lycanzip POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${LYCAN_7Z_DLL}" "$<TARGET_FILE_DIR:lycanzip>/7z.dll"
    COMMENT "Copy 7z.dll for development")
else()
  message(WARNING "7z.dll not found; set the environment variable LYCAN_7Z_DLL or put 7z.dll next to the exe")
endif()

