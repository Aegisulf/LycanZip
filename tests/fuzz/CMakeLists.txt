# libFuzzer targets (MSVC /fsanitize=fuzzer). Built only with -DLYCAN_FUZZ=ON (see scripts\fuzz.ps1).
foreach(name paths encoding open iso update)
  add_executable(fuzz_${name} fuzz_${name}.cpp)
  target_link_libraries(fuzz_${name} PRIVATE lycan_core)
  target_compile_options(fuzz_${name} PRIVATE /fsanitize=fuzzer)
endforeach()
