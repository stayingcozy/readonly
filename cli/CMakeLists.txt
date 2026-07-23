add_executable(readonly src/main.cpp)  # + src/terminal.cpp later
target_include_directories(readonly PRIVATE include)
target_link_libraries(readonly
    PRIVATE readonly_core CLI11::CLI11 readonly_options)