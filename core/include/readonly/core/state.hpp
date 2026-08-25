#pragma once
#include "readonly/core/config.hpp"
#include "readonly/core/error.hpp"

namespace readonly::core {

// Writes/maps teh single readonly-active byte (aivm.state)
class StateWriter {
public:
  static Result<StateWriter> open(const Paths &);
  void set_readonly(bool active); // atomic store
  ~StateWriter();

private:
  int fd_{-1};
  void *map_{nullptr};
};
} // namespace readonly::core