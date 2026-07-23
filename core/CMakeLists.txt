add_library(readonly_core STATIC
  src/config.cpp
  src/registry.cpp
  # src/snapshot.cpp
  # src/mounts.cpp
  # src/vm.cpp
  # src/vsock.cpp
  # src/state.cpp
  # src/agent.cpp
)
target_include_directories(readonly_core PUBLIC include)
target_link_libraries(readonly_core
    PUBLIC  readonly_shared
    PRIVATE readonly_options
)
set_target_properties(readonly_core PROPERTIES LINKER_LANGUAGE CXX)