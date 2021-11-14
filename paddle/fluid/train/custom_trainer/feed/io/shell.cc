#include "paddle/fluid/train/custom_trainer/feed/io/shell.h"

namespace paddle {
namespace custom_trainer {
namespace feed {

void shell_add_write_converter(std::string& path, bool& is_pipe,      // NOLINT
        const std::string& converter) {
    if (converter == "") {
        return;
    }

    if (!is_pipe) {
        path = string::format_string("( %s ) > \"%s\"", converter.c_str(), path.c_str());
        is_pipe = true;
    } else {
        path = string::format_string("%s | %s", converter.c_str(), path.c_str());
    }
}

void shell_add_read_converter(std::string& path, bool& is_pipe, const std::string& converter) {
    if (converter == "") {
        return;
    }

    if (!is_pipe) {
        path = string::format_string("( %s ) < \"%s\"", converter.c_str(), path.c_str());
        is_pipe = true;
    } else {
        path = string::format_string("%s | %s", path.c_str(), converter.c_str());
    }
}

std::shared_ptr<FILE> shell_open(
        const std::string& path,
        bool is_pipe,
        const std::string& mode,
        size_t buffer_size,
        int* err_no) {
    std::shared_ptr<FILE> fp = nullptr;

    if (!is_pipe) {
        fp = shell_fopen(path, mode);
    } else {
        fp = shell_popen(path, mode, err_no);
    }

    if (buffer_size > 0) {
        char* buffer = new char[buffer_size];
        CHECK_EQ(0, setvbuf(&*fp, buffer, _IOFBF, buffer_size));
        fp = {&*fp, [fp, buffer](FILE*) mutable {  // NOLINT
                  CHECK(fp.unique());              // NOLINT
                  fp = nullptr;
                  delete[] buffer;
              }};
    }

    return fp;
}

std::shared_ptr<FILE> shell_fopen(const std::string& path, const std::string& mode) {
#if defined _WIN32 || defined __APPLE__
    return nullptr;
#else
    if (shell_verbose()) {
        LOG(INFO) << "Opening file[" << path << "] with mode[" << mode << "]";
    }
    FILE* fp;
    if (!(fp = fopen(path.c_str(), mode.c_str()))) {
        LOG(FATAL) << "fopen fail, path[" << path << "], mode[" << mode << "]";
    }
    return {fp, [path](FILE* fp) {
                if (shell_verbose()) {
                    LOG(INFO) << "Closing file[" << path << "]";
                }
                if (0 != fclose(fp)) {
                    LOG(FATAL) << "fclose fail, path[" << path << "]";
                }
            }};
#endif
}

// Close all open file descriptors
// The implementation is async signal safe
// Mostly copy from CPython code
static int close_open_fds_internal() {
#if defined _WIN32 || defined __APPLE__
    return 0;
#else
    struct linux_dirent {
        long d_ino = 0;  // NOLINT
        off_t d_off;
        unsigned short d_reclen = 0;  // NOLINT
        char d_name[256];
    };

    int dir_fd = -1;
    if ((dir_fd = open("/proc/self/fd", O_RDONLY)) < 0) {
        LOG(FATAL) << "proc/self/fd open fail";
        return -1;
    }
    char buffer[sizeof(linux_dirent)];

    for (;;) {
        int bytes = 0;
        if ((bytes =
                     syscall(SYS_getdents,
                             dir_fd,
                             reinterpret_cast<linux_dirent*>(buffer),
                             sizeof(buffer))) < 0) {
            LOG(FATAL) << "syscall fail";
            return -1;
        }

        if (bytes == 0) {
            break;
        }

        linux_dirent* entry = NULL;

        for (int offset = 0; offset < bytes; offset += entry->d_reclen) {
            entry = reinterpret_cast<linux_dirent*>(buffer + offset);
            int fd = 0;
            const char* s = entry->d_name;

            while (*s >= '0' && *s <= '9') {
                fd = fd * 10 + (*s - '0');
                s++;
            }

            if (s != entry->d_name && fd != dir_fd && fd >= 3) {
                close(fd);
            }
        }
    }

    close(dir_fd);
    return 0;
#endif
}

static int shell_popen_fork_internal(
        const char* real_cmd,
        bool do_read,
        int parent_end,
        int child_end) {
#if defined _WIN32 || defined __APPLE__
    return 0;
#else
    int child_pid = -1;
    // Too frequent calls to fork() makes openmpi very slow. Use vfork() instead.
    // But vfork() is very dangerous. Be careful.
    if ((child_pid = vfork()) < 0) {
        return -1;
    }

    // The following code is async signal safe (No memory allocation, no access to
    // global data, etc.)
    if (child_pid != 0) {
        return child_pid;
    }

    int child_std_end = do_read ? 1 : 0;
    close(parent_end);

    if (child_end != child_std_end) {
        if (dup2(child_end, child_std_end) != child_std_end) {
            exit(127);
        }
        close(child_end);
    }

    close_open_fds_internal();
    if (execl("/bin/bash", "bash", "-c", real_cmd, NULL) < 0) {
        exit(127);
    }
    exit(127);
#endif
}

std::shared_ptr<FILE> shell_popen(const std::string& cmd, const std::string& mode, int* err_no) {
#if defined _WIN32 || defined __APPLE__
    return nullptr;
#else
    bool do_read = mode == "r";
    bool do_write = mode == "w";
    if (!(do_read || do_write)) {
        *err_no = -1;
        return NULL;
    }

    if (shell_verbose()) {
        LOG(INFO) << "Opening pipe[" << cmd << "] with mode[" << mode << "]";
    }

    std::string real_cmd = "set -o pipefail; " + cmd;

    int pipe_fds[2];
    if (pipe(pipe_fds) != 0) {
        *err_no = -1;
        return NULL;
    }
    int parent_end = 0;
    int child_end = 0;

    if (do_read) {
        parent_end = pipe_fds[0];
        child_end = pipe_fds[1];
    } else if (do_write) {
        parent_end = pipe_fds[1];
        child_end = pipe_fds[0];
    }

    int child_pid = shell_popen_fork_internal(real_cmd.c_str(), do_read, parent_end, child_end);
    close(child_end);
    fcntl(parent_end, F_SETFD, FD_CLOEXEC);
    FILE* fp;
    if ((fp = fdopen(parent_end, mode.c_str())) == NULL) {
        *err_no = -1;
        return NULL;
    }
    return {fp, [child_pid, cmd, err_no](FILE* fp) {
                if (shell_verbose()) {
                    LOG(INFO) << "Closing pipe[" << cmd << "]";
                }

                if (fclose(fp) != 0) {
                    *err_no = -1;
                }
                int wstatus = -1;
                waitpid(child_pid, &wstatus, 0);
                if (wstatus == 0 || wstatus == (128 + SIGPIPE) * 256 ||
                    (wstatus == -1 && errno == ECHILD)) {
                } else {
                    *err_no = -1;
                    LOG(WARNING) << "status[" << wstatus << "], cmd[" << cmd << "]"
                                 << ", err_no[" << *err_no << "]";
                }
                if (wstatus == -1 && errno == ECHILD) {
                    LOG(WARNING) << "errno is ECHILD";
                }
            }};
#endif
}

static int shell_p2open_fork_internal(const char* real_cmd, int pipein_fds[2], int pipeout_fds[2]) {
#if defined _WIN32 || defined __APPLE__
    return 0;
#else
    int child_pid = -1;
    if ((child_pid = fork()) < 0) {
        return -1;
    }

    if (child_pid != 0) {
        return child_pid;
    }

    close(pipein_fds[0]);
    close(pipeout_fds[1]);

    if (pipein_fds[1] != 1) {
        if (dup2(pipein_fds[1], 1) != 1) {
            return -1;
        }
        close(pipein_fds[1]);
    }

    if (pipeout_fds[0] != 0) {
        if (dup2(pipeout_fds[0], 0) != 0) {
            return -1;
        }
        close(pipeout_fds[0]);
    }

    close_open_fds_internal();
    if (execl("/bin/sh", "sh", "-c", real_cmd, NULL) < 0) {
        return -1;
    }
    exit(127);
#endif
}

std::pair<std::shared_ptr<FILE>, std::shared_ptr<FILE>> shell_p2open(const std::string& cmd) {
#if defined _WIN32 || defined __APPLE__
    return {};
#else
    if (shell_verbose()) {
        LOG(INFO) << "Opening bidirectional pipe[" << cmd << "]";
    }

    std::string real_cmd = "set -o pipefail; " + cmd;

    int pipein_fds[2];
    int pipeout_fds[2];
    if (pipe(pipein_fds) != 0) {
        return {NULL, NULL};
    }
    if (pipe(pipeout_fds) != 0) {
        return {NULL, NULL};
    }

    int child_pid = shell_p2open_fork_internal(real_cmd.c_str(), pipein_fds, pipeout_fds);

    close(pipein_fds[1]);
    close(pipeout_fds[0]);
    fcntl(pipein_fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(pipeout_fds[1], F_SETFD, FD_CLOEXEC);

    std::shared_ptr<int> child_life = {
            NULL, [child_pid, cmd](void*) {
                if (shell_verbose()) {
                    LOG(INFO) << "Closing bidirectional pipe[" << cmd << "]";
                }

                int wstatus, ret;

                do {
                    PCHECK((ret = waitpid(child_pid, &wstatus, 0)) >= 0 ||
                           (ret == -1 && errno == EINTR));
                } while (ret == -1 && errno == EINTR);

                PCHECK(wstatus == 0 || wstatus == (128 + SIGPIPE) * 256 ||
                       (wstatus == -1 && errno == ECHILD))
                        << "status[" << wstatus << "], cmd[" << cmd << "]";

                if (wstatus == -1 && errno == ECHILD) {
                    LOG(WARNING) << "errno is ECHILD";
                }
            }};

    FILE* in_fp;
    PCHECK((in_fp = fdopen(pipein_fds[0], "r")) != NULL);
    FILE* out_fp;
    PCHECK((out_fp = fdopen(pipeout_fds[1], "w")) != NULL);
    return {{in_fp, [child_life](FILE* fp) { PCHECK(fclose(fp) == 0); }},
            {out_fp, [child_life](FILE* fp) { PCHECK(fclose(fp) == 0); }}};
#endif
}

std::string shell_get_command_output(const std::string& cmd) {
#if defined _WIN32 || defined __APPLE__
    return "";
#else
    int err_no = 0;
    do {
        if (err_no == -1) {
            sleep(10);
        }
        err_no = 0;
        std::shared_ptr<FILE> pipe = shell_popen(cmd, "r", &err_no);
        string::LineFileReader reader;
        if (reader.getdelim(&*pipe, 0)) {
            pipe = nullptr;
            if (err_no == 0) {
                return reader.get();
            }
        }
        VLOG(2) << "run shell cmd:" << cmd << ", errno:" << err_no;
    } while (err_no == -1);
    return "";
#endif
}

}  // namespace feed
}  // namespace custom_trainer
}  // namespace paddle
