add_executable(readonly_tests
  test_main.cpp
  test_config.cpp  # __config, __accel
  test_mirror.cpp  # __mirror
  test_snapshot.cpp # __overlay, __info
)

target_link_libraries(readonly_tests
  PRIVATE readonly_core doctest::doctest readonly_options)

add_test(NAME readonly_unit COMMAND readonly_tests --test-suite-exclude=integration)
#add_test(NAME readonly_integration COMMAND readonly_tests --test-suite=readonly_integration)

#set_tests_properties(readonly_integration PROPERTIES LABELS integration RESOURCE_LOCK qemu TIMEOUT 120)
set_tests_properties(readonly_unit PROPERTIES LABELS unit)

add_test(NAME readonly_help COMMAND readonly --help)
