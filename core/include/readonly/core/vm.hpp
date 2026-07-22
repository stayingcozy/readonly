#pragma once 
#include <filesystem>
#include "readonly/core/error.hpp"

namespace readonly::core {
namespace fs = std::filesystem;

enum class Accel { Kvm, Hvf, Whpx, None };
Accel detect_accel();  // try /dev/kvm open; OS-specific fallbacks

struct QemuConfig {
    fs::path image;
    fs::path src_share; 
    fs::path out_share;
    Accel    accel{Accel::None}; 
    unsigned guest_cid{3};
    unsigned memory_mb{2048};
};

class Vm {
public:
    static Result<Vm> launch(const QemuConfig&);
    Result<int>       wait();   // block until qemu exits
    void              kill();   // hard-kill
    ~Vm();

    Vm(Vm&&) noexcept;
    Vm& operator=(Vm&&) noexcept;
    Vm(const Vm&) = delete;
    Vm& operator=(const Vm&) = delete;
private:
    int pid_{-1};
};
} // namespace readonly::core