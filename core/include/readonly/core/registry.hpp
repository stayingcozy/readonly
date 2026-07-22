#pragma once
#include <string>
#include <string_view>
#include <vector>
#include "readonly/core/config.hpp"
#include "readonly/core/error.hpp"
#include "readonly/shared/descriptor.hpp"

namespace readonly::core {
using shared::AgentDescriptor;

class Registry {
public:
    explicit Registry(Paths paths);

    static bool         is_reserved(std::string_view name);
    static Result<void> validate_name(std::string_view name); 

    Result<std::vector<AgentDescriptor>> list() const;
    bool                                 exists(std::string_view name) const;
    Result<AgentDescriptor>              resolve(std::string_view name) const;
    Result<void>                         write(const AgentDescriptor&) const; 
    fs::path                             overlay_path(std::string_view name) const;
private: 
    Paths paths_;
};
} // namespace readonly::core