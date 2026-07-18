add_library(readonly_core STATIC 
# add below as created. ex:
# src/config.cpp
# <read of src/*.cpp>
)
target_include_directories(readonly_core PUBLIC include)
target_link_libraries(readonly_core
    PUBLIC  readonly_shared
    PRIVATE readonly_options
)
set_target_properties(readonly_core PROPERTIES LINKER_LANGUAGE CXX)