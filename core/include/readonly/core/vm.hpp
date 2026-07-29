#pragma once 
#include <filesystem>
#include <optional>
#include <string> 
#include <vector> 
#include "readonly/core/error.hpp"

namespace readonly::core {
namespace fs = std::filesystem;

enum class Accel { Kvm, Hvf, Whpx, None };
struct AccelInfo {
    Accel accel{Accel::None};
    bool kvm_present_but_denied{false};  // /dev/kvm exists but EACCES
};
AccelInfo detect_accel();  // try /dev/kvm open; OS-specific fallbacks

struct QemuConfig {
    fs::path image;                  // image to boot
    fs::path kernel;                 // external bzImage (another yocto artifact)
    fs::path src_share;              // host dir, readonly
    fs::path out_share;              // host dir, read and write
    Accel    accel{Accel::None}; 
    unsigned guest_cid{3};           // vsock id; unique per live vm
    unsigned memory_mb{2048};
    std::string append{"root=/dev/vda console=ttyS0 rw"};
    std::optional<fs::path> serial_log;   // unset -> no graphic (serial on stdio, debug)
};

class Vm {
public:
    static std::vector<std::string> build_argv(const QemuConfig&);  // Public so command can be seen

    static Result<Vm> launch(const QemuConfig&);
    Result<int>       wait();   // block until qemu exits
    void              kill();   // hard-kill

    int pid() const { return pid_; }
    bool running() const { return pid_ != -1; }

    ~Vm();
    Vm(Vm&&) noexcept;
    Vm& operator=(Vm&&) noexcept;
    Vm(const Vm&) = delete;
    Vm& operator=(const Vm&) = delete;

private:
    Vm() = default;
    int pid_{-1};
};
} // namespace readonly::core