#include "proc.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <format>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace readonly::core::proc {

Result<Output> run(const std::vector<std::string> &argv) {
  if (argv.empty())
    return fail("proc::run called with empty argv");

  int err_pipe[2];
  if (::pipe(err_pipe) != 0)
    return fail(std::format("pipe failed: {}", std::strerror(errno)));

  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_adddup2(&fa, err_pipe[1], STDERR_FILENO);
  posix_spawn_file_actions_addclose(&fa, err_pipe[0]);
  posix_spawn_file_actions_addclose(&fa, err_pipe[1]);

  std::vector<char *> cargv;
  cargv.reserve(argv.size() + 1);
  for (const auto &s : argv)
    cargv.push_back(const_cast<char *>(s.c_str()));
  cargv.push_back(nullptr);

  pid_t pid = -1;
  const int rc =
      ::posix_spawnp(&pid, cargv[0], &fa, nullptr, cargv.data(), environ);
  posix_spawn_file_actions_destroy(&fa);
  ::close(err_pipe[1]);

  if (rc != 0) {
    ::close(err_pipe[0]);
    return fail(std::format("cannot spawn {}: {}", argv[0], std::strerror(rc)));
  }

  std::string captured;
  std::array<char, 4096> buf;
  for (ssize_t n; (n = ::read(err_pipe[0], buf.data(), buf.size())) != 0;) {
    if (n < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    captured.append(buf.data(), static_cast<std::size_t>(n));
  }
  ::close(err_pipe[0]);

  int status = 0;
  while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }

  Output out;
  out.stderr_text = std::move(captured);
  out.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return out;
}

} // namespace readonly::core::proc
