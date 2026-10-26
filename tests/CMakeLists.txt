# Each test takes at most 120 seconds: anything that hangs should fail instead of waiting for ever
set(CTEST_TEST_TIMEOUT 120)
add_executable(test_security test_security.cpp)
target_link_libraries(test_security PRIVATE lycan_core)
add_test(NAME security COMMAND test_security)

add_executable(test_hostile test_hostile.cpp)
target_link_libraries(test_hostile PRIVATE lycan_core)
add_test(NAME hostile COMMAND test_hostile)

add_executable(test_encoding test_encoding.cpp)
target_link_libraries(test_encoding PRIVATE lycan_core)
add_test(NAME encoding COMMAND test_encoding)

add_executable(test_taskqueue test_taskqueue.cpp)
target_link_libraries(test_taskqueue PRIVATE lycan_core)
add_test(NAME taskqueue COMMAND test_taskqueue)
add_dependencies(test_taskqueue lycanzip)
add_test(NAME taskqueue_process COMMAND test_taskqueue)
set_tests_properties(taskqueue_process PROPERTIES ENVIRONMENT "LYCAN_TEST_EXECUTOR=process" TIMEOUT 300)

add_executable(test_settings test_settings.cpp)
target_link_libraries(test_settings PRIVATE lycan_core)
add_test(NAME settings COMMAND test_settings)

add_executable(test_presets test_presets.cpp)
target_link_libraries(test_presets PRIVATE lycan_core)
add_test(NAME presets COMMAND test_presets)

add_executable(test_password test_password.cpp)
target_link_libraries(test_password PRIVATE lycan_core)
add_test(NAME password_conflict COMMAND test_password)

add_executable(test_crashreport test_crashreport.cpp)
target_link_libraries(test_crashreport PRIVATE lycan_core)
add_test(NAME crashreport COMMAND test_crashreport)

add_executable(test_i18n test_i18n.cpp)
target_link_libraries(test_i18n PRIVATE lycan_core)
add_test(NAME i18n COMMAND test_i18n)

add_executable(test_tree test_tree.cpp)
target_link_libraries(test_tree PRIVATE lycan_core)
add_test(NAME tree COMMAND test_tree)

add_executable(test_junk test_junk.cpp)
target_link_libraries(test_junk PRIVATE lycan_core)
add_test(NAME junk COMMAND test_junk)

add_executable(test_update test_update.cpp)
target_link_libraries(test_update PRIVATE lycan_core)
add_test(NAME update COMMAND test_update)

add_executable(test_diag test_diag.cpp)
target_link_libraries(test_diag PRIVATE lycan_core)
add_test(NAME diag COMMAND test_diag)

add_executable(test_hash test_hash.cpp)
target_link_libraries(test_hash PRIVATE lycan_core)
add_test(NAME hash COMMAND test_hash)
add_executable(test_checksum test_checksum.cpp)
target_link_libraries(test_checksum PRIVATE lycan_core)
add_test(NAME checksum COMMAND test_checksum)

add_executable(test_probe test_probe.cpp)
target_link_libraries(test_probe PRIVATE lycan_core)
add_test(NAME probe COMMAND test_probe)

add_executable(test_store test_store.cpp)
target_link_libraries(test_store PRIVATE lycan_core)
add_test(NAME store COMMAND test_store)
add_executable(test_readentries test_readentries.cpp)
target_link_libraries(test_readentries PRIVATE lycan_core)
add_test(NAME readentries COMMAND test_readentries)

add_executable(test_volumes test_volumes.cpp)
target_link_libraries(test_volumes PRIVATE lycan_core)
add_test(NAME volumes COMMAND test_volumes)

add_executable(test_scan test_scan.cpp)
target_link_libraries(test_scan PRIVATE lycan_core)
add_test(NAME scan COMMAND test_scan)

add_executable(test_launch_shell test_launch_shell.cpp)
target_link_libraries(test_launch_shell PRIVATE lycan_core)
add_test(NAME launch_shell COMMAND test_launch_shell)

add_executable(test_updatemode test_updatemode.cpp)
target_link_libraries(test_updatemode PRIVATE lycan_core)
add_test(NAME updatemode COMMAND test_updatemode)

add_executable(test_iso test_iso.cpp)
target_link_libraries(test_iso PRIVATE lycan_core)
add_test(NAME iso COMMAND test_iso)

add_executable(test_keepdamaged test_keepdamaged.cpp)
target_link_libraries(test_keepdamaged PRIVATE lycan_core)
add_test(NAME keepdamaged COMMAND test_keepdamaged)

add_executable(test_dataafterend test_dataafterend.cpp)
target_link_libraries(test_dataafterend PRIVATE lycan_core)
add_test(NAME dataafterend COMMAND test_dataafterend)

add_executable(test_fileicons test_fileicons.cpp)
target_link_libraries(test_fileicons PRIVATE lycan_core)
add_test(NAME fileicons COMMAND test_fileicons)

add_executable(test_fileassoc test_fileassoc.cpp)
target_link_libraries(test_fileassoc PRIVATE lycan_core)
# the names of the file types are texts of the zh-TW language file (the folder of the sources: no copy next to the test)
target_compile_definitions(test_fileassoc PRIVATE LYCAN_LANG_DIR="${CMAKE_SOURCE_DIR}/app/Assets/Lang")
add_test(NAME fileassoc COMMAND test_fileassoc)

add_executable(test_robust test_robust.cpp)
target_link_libraries(test_robust PRIVATE lycan_core psapi)
add_test(NAME robust COMMAND test_robust)
set_tests_properties(robust PROPERTIES TIMEOUT 600)

add_executable(test_worker test_worker.cpp)
target_link_libraries(test_worker PRIVATE lycan_core)
add_dependencies(test_worker lycanzip)
add_test(NAME worker COMMAND test_worker)
set_tests_properties(worker PROPERTIES TIMEOUT 600)

add_executable(test_sevenzip_pin test_sevenzip_pin.cpp)
add_dependencies(test_sevenzip_pin lycanzip)
if(LYCAN_7Z_PIN_SHA256)
  target_compile_definitions(test_sevenzip_pin PRIVATE EXPECT_PINNED=1)
endif()
add_test(NAME sevenzip_pin COMMAND test_sevenzip_pin)

add_executable(test_updatefeed test_updatefeed.cpp)
target_link_libraries(test_updatefeed PRIVATE lycan_core)
add_test(NAME updatefeed COMMAND test_updatefeed)

# The update of the installed copy (needs third_party/velopack: scripts/fetch-velopack.ps1); run by the release flow (it makes the feed with vpk and signs it)
set(LYCAN_VELOPACK_DIR "${CMAKE_SOURCE_DIR}/third_party/velopack")
if(EXISTS "${LYCAN_VELOPACK_DIR}/include/Velopack.hpp")
  add_executable(test_updater test_updater.cpp ${CMAKE_SOURCE_DIR}/src/updater/installed_update.cpp)
  target_include_directories(test_updater PRIVATE ${CMAKE_SOURCE_DIR}/src/updater ${LYCAN_VELOPACK_DIR}/include)
  target_link_libraries(test_updater PRIVATE lycan_core ${LYCAN_VELOPACK_DIR}/lib/velopack_libc_win_x64_msvc.dll.lib)
  add_custom_command(TARGET test_updater POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${LYCAN_VELOPACK_DIR}/lib/velopack_libc_win_x64_msvc.dll" "$<TARGET_FILE_DIR:test_updater>/velopack_libc.dll")
endif()

add_executable(echo_args WIN32 echo_args.cpp)
add_executable(test_shellext test_shellext.cpp)
target_link_libraries(test_shellext PRIVATE lycan_core ole32 shell32 uuid)
add_dependencies(test_shellext LycanZipShell echo_args)
add_test(NAME shellext COMMAND test_shellext $<TARGET_FILE:LycanZipShell> $<TARGET_FILE:echo_args>)

find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_test(NAME cli_roundtrip
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_roundtrip.py $<TARGET_FILE:lycanzip>)
endif()

add_executable(test_audiotags test_audiotags.cpp)
target_link_libraries(test_audiotags PRIVATE lycan_core)
add_test(NAME audiotags COMMAND test_audiotags)

add_executable(test_tempdir test_tempdir.cpp)
target_link_libraries(test_tempdir PRIVATE lycan_core)
add_test(NAME tempdir COMMAND test_tempdir)

add_executable(test_versioncheck test_versioncheck.cpp)
target_link_libraries(test_versioncheck PRIVATE lycan_core)
add_test(NAME versioncheck COMMAND test_versioncheck)
