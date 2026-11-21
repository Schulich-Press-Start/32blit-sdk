#include "process.hpp"
#include <stdexcept>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace editor {
#ifdef _WIN32
  struct Process::State {
    HANDLE process = nullptr, job = nullptr, input = nullptr, output = nullptr, writer = nullptr;
    ~State() {
      if(job) CloseHandle(job); // KILL_ON_JOB_CLOSE also stops the watcher’s game.
      if(process) CloseHandle(process);
      if(input && input != INVALID_HANDLE_VALUE) CloseHandle(input);
      if(output) CloseHandle(output);
      if(writer) CloseHandle(writer);
    }
  };

  static std::wstring quote(const std::wstring &text) {
    std::wstring result = L"\"";
    size_t slashes = 0;
    for(auto c : text) {
      if(c == L'\\') { ++slashes; continue; }
      result.append(c == L'\"' ? 2 * slashes + 1 : slashes, L'\\');
      result += c;
      slashes = 0;
    }
    result.append(2 * slashes, L'\\');
    return result + L'\"';
  }

  void Process::start(const std::filesystem::path &directory, const std::vector<std::string> &args) {
    stop();
    auto s = std::make_unique<State>();
    auto fail = [&] { throw std::runtime_error(args.front() + ": Windows error " + std::to_string(GetLastError())); };
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    if(!CreatePipe(&s->output, &s->writer, &inherit, 0) ||
       !SetHandleInformation(s->output, HANDLE_FLAG_INHERIT, 0)) fail();
    s->input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);
    if(s->input == INVALID_HANDLE_VALUE) fail();
    s->job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(!s->job || !SetInformationJobObject(s->job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) fail();
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = s->input;
    startup.hStdOutput = startup.hStdError = s->writer;
    std::wstring line;
    for(const auto &arg : args) {
      if(!line.empty()) line += L' ';
      line += quote(std::filesystem::u8path(arg).wstring());
    }
    PROCESS_INFORMATION process{};
    if(!CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED | CREATE_NO_WINDOW,
                       nullptr, directory.c_str(), &startup, &process)) fail();
    s->process = process.hProcess;
    if(!AssignProcessToJobObject(s->job, s->process) || ResumeThread(process.hThread) == DWORD(-1)) {
      const auto error = GetLastError();
      TerminateProcess(s->process, 1);
      CloseHandle(process.hThread);
      SetLastError(error);
      fail();
    }
    CloseHandle(process.hThread);
    CloseHandle(s->writer);
    s->writer = nullptr;
    state_ = std::move(s);
  }

  std::string Process::read_log() {
    DWORD available = 0, count = 0;
    char buf[8192];
    if(!active() || !PeekNamedPipe(state_->output, nullptr, 0, nullptr, &available, nullptr) || !available) return {};
    if(!ReadFile(state_->output, buf, available < sizeof(buf) ? available : sizeof(buf), &count, nullptr)) return {};
    return {buf, count};
  }

  std::optional<int> Process::poll() {
    if(!active() || WaitForSingleObject(state_->process, 0) != WAIT_OBJECT_0) return {};
    DWORD code = 1;
    GetExitCodeProcess(state_->process, &code);
    stop();
    return static_cast<int>(code);
  }
#else
  struct Process::State {
    pid_t pid = -1;
    int output = -1;
    ~State() { if(output >= 0) close(output); }
  };

  void Process::start(const std::filesystem::path &directory, const std::vector<std::string> &args) {
    stop();
    auto s = std::make_unique<State>();
    std::vector<char *> argv;
    for(const auto &arg : args) argv.push_back(const_cast<char *>(arg.c_str()));
    argv.push_back(nullptr);
    const auto *cwd = directory.c_str();
    const int input = open("/dev/null", O_RDONLY);
    if(input < 0) throw std::runtime_error("Cannot open /dev/null");
    int pipefd[2];
    if(pipe(pipefd) < 0) { close(input); throw std::runtime_error("Cannot create build log pipe"); }
    s->output = pipefd[0];
    if(fcntl(s->output, F_SETFL, O_NONBLOCK) < 0) {
      close(pipefd[1]); close(input); throw std::runtime_error("Cannot configure build log pipe");
    }
    s->pid = fork();
    if(s->pid == 0) {
      if(setpgid(0, 0) < 0 || chdir(cwd) < 0 || dup2(input, STDIN_FILENO) < 0 ||
         dup2(pipefd[1], STDOUT_FILENO) < 0 || dup2(pipefd[1], STDERR_FILENO) < 0) _exit(126);
      close(input); close(pipefd[0]); close(pipefd[1]);
      execvp(argv[0], argv.data());
      constexpr char error[] = "Could not execute command; check PATH and permissions.\n";
      (void)write(STDERR_FILENO, error, sizeof(error) - 1);
      _exit(127);
    }
    close(input); close(pipefd[1]);
    if(s->pid < 0) throw std::runtime_error("Cannot launch " + args.front());
    (void)setpgid(s->pid, s->pid);
    state_ = std::move(s);
  }

  std::string Process::read_log() {
    if(!active()) return {};
    char buf[8192];
    const auto count = read(state_->output, buf, sizeof(buf));
    return count > 0 ? std::string(buf, static_cast<size_t>(count)) : std::string{};
  }

  std::optional<int> Process::poll() {
    if(!active()) return {};
    int status;
    if(waitpid(state_->pid, &status, WNOHANG) != state_->pid) return {};
    (void)kill(-state_->pid, SIGKILL);
    state_.reset();
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
  }
#endif

  Process::Process() = default;
  Process::~Process() { stop(); }
  bool Process::active() const { return bool(state_); }
  void Process::stop() {
#ifndef _WIN32
    if(state_) {
      (void)kill(-state_->pid, SIGKILL);
      while(waitpid(state_->pid, nullptr, 0) < 0 && errno == EINTR) {}
    }
#endif
    state_.reset();
  }
}
