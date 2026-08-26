add_executable(readonly_tests
  test_main.cpp
)
target_link_libraries(readonly_tests
  PRIVATE readonly_core doctest::doctest readonly_options)
add_test(NAME readonly_tests COMMAND readonly_tests)
add_test(NAME readonly_help COMMAND readonly --help)
