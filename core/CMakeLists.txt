add_library(readonly_core STATIC
  src/kv.cpp
  src/config.cpp
  src/registry.cpp
  src/mounts.cpp
)
target_include_directories(readonly_core PUBLIC include PRIVATE src)
target_link_libraries(readonly_core
    PUBLIC  readonly_shared
    PRIVATE readonly_options
)