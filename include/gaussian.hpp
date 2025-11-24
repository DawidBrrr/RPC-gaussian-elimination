#pragma once

#include "matrix.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

namespace detail {

inline void validate_augmented(const CppMatrix &augmented) {
    if (augmented.rows == 0) {
        throw std::invalid_argument("Matrix must have at least one row");
    }

    if (augmented.cols != augmented.rows + 1) {
        throw std::invalid_argument("Augmented matrix must have exactly one more column than rows");
    }
}

enum class WorkerCommand : std::size_t { Work = 1, Exit = 2 };

struct WorkerTask {
    std::size_t command;
    std::size_t column;
    std::size_t start_row;
    std::size_t end_row;
};

struct WorkerAck {
    int status;
};

inline bool fd_write_full(int fd, const void *buffer, std::size_t bytes) {
    const auto *data = static_cast<const std::byte *>(buffer);
    std::size_t written = 0;
    while (written < bytes) {
        const ssize_t ret = write(fd, data + written, bytes - written);
        if (ret == -1) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        written += static_cast<std::size_t>(ret);
    }
    return true;
}

inline bool fd_read_full(int fd, void *buffer, std::size_t bytes) {
    auto *data = static_cast<std::byte *>(buffer);
    std::size_t read_total = 0;
    while (read_total < bytes) {
        const ssize_t ret = read(fd, data + read_total, bytes - read_total);
        if (ret == 0) {
            return false;
        }
        if (ret == -1) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        read_total += static_cast<std::size_t>(ret);
    }
    return true;
}

[[noreturn]] inline void worker_loop(int read_fd, int write_fd, double *shared_data, std::size_t width) {
    for (;;) {
        WorkerTask task{};
        if (!fd_read_full(read_fd, &task, sizeof(task))) {
            _exit(1);
        }

        if (static_cast<WorkerCommand>(task.command) == WorkerCommand::Exit) {
            WorkerAck ack{0};
            fd_write_full(write_fd, &ack, sizeof(ack));
            _exit(0);
        }

        if (task.start_row < task.end_row) {
            const double pivot = shared_data[task.column * width + task.column];
            const double *pivot_row = &shared_data[task.column * width];
            for (std::size_t row = task.start_row; row < task.end_row; ++row) {
                double *row_ptr = &shared_data[row * width];
                const double factor = row_ptr[task.column] / pivot;
                for (std::size_t k = task.column; k < width; ++k) {
                    row_ptr[k] -= factor * pivot_row[k];
                }
            }
        }

        WorkerAck ack{0};
        if (!fd_write_full(write_fd, &ack, sizeof(ack))) {
            _exit(1);
        }
    }
}

inline std::string errno_message(const char *prefix) {
    return std::string(prefix) + ": " + std::strerror(errno);
}

} // namespace detail

// Sekwencyjna wersja eliminacji Gaussa
inline std::vector<double> gaussian_sequential(const CppMatrix &augmented) {
    detail::validate_augmented(augmented);
    const std::size_t n = augmented.rows;
    const std::size_t width = augmented.cols;
    constexpr double kEpsilon = 1e-12;

    std::vector<double> data = augmented.data;
    auto at = [width, &data](std::size_t r, std::size_t c) -> double & {
        return data[r * width + c];
    };

    for (std::size_t col = 0; col < n; ++col) {
        const double pivot = at(col, col);
        if (std::fabs(pivot) < kEpsilon) {
            throw std::runtime_error("Matrix is singular or ill-conditioned");
        }

        for (std::size_t row = col + 1; row < n; ++row) {
            const double factor = at(row, col) / pivot;
            for (std::size_t k = col; k < width; ++k) {
                at(row, k) -= factor * at(col, k);
            }
        }
    }

    std::vector<double> solution(n, 0.0);
    for (std::size_t i = n; i-- > 0;) {
        double rhs = at(i, width - 1);
        for (std::size_t j = i + 1; j < n; ++j) {
            rhs -= at(i, j) * solution[j];
        }

        const double pivot = at(i, i);
        if (std::fabs(pivot) < kEpsilon) {
            throw std::runtime_error("Matrix is singular or ill-conditioned");
        }

        solution[i] = rhs / pivot;
    }

    return solution;
}

// Równoległa wersja eliminacji Gaussa z użyciem fork() i współdzielonej pamięci
inline std::vector<double> gaussian_parallel(const CppMatrix &augmented, std::size_t max_processes = 0) {
    detail::validate_augmented(augmented);

    const std::size_t n = augmented.rows;
    if (n < 2) {
        return gaussian_sequential(augmented);
    }

    const std::size_t width = augmented.cols;
    const std::size_t total_elements = n * width;
    const std::size_t total_bytes = total_elements * sizeof(double);

    double *shared_data = static_cast<double *>(mmap(nullptr, total_bytes, PROT_READ | PROT_WRITE,
                                                     MAP_SHARED | MAP_ANONYMOUS, -1, 0));
    if (shared_data == MAP_FAILED) {
        throw std::runtime_error("mmap failed: " + std::string(std::strerror(errno)));
    }

    struct SharedMatrixGuard {
        double *ptr;
        std::size_t bytes;
        ~SharedMatrixGuard() {
            if (ptr && ptr != MAP_FAILED) {
                munmap(ptr, bytes);
            }
        }
    } matrix_guard{shared_data, total_bytes};

    std::copy(augmented.data.begin(), augmented.data.end(), shared_data);

    const long cpu_available = sysconf(_SC_NPROCESSORS_ONLN);
    std::size_t process_budget = max_processes > 0
                                     ? max_processes
                                     : static_cast<std::size_t>(cpu_available > 0 ? cpu_available : 1);
    process_budget = std::max<std::size_t>(1, std::min(process_budget, n - 1));

    struct WorkerProcess {
        pid_t pid{-1};
        int write_fd{-1};
        int read_fd{-1};
    };

    auto close_fd = [](int fd) {
        if (fd >= 0) {
            close(fd);
        }
    };

    std::vector<WorkerProcess> workers;
    workers.reserve(process_budget);

    auto spawn_worker = [&]() {
        int to_child[2]{-1, -1};
        int to_parent[2]{-1, -1};
        if (pipe(to_child) == -1 || pipe(to_parent) == -1) {
            if (to_child[0] != -1) close(to_child[0]);
            if (to_child[1] != -1) close(to_child[1]);
            if (to_parent[0] != -1) close(to_parent[0]);
            if (to_parent[1] != -1) close(to_parent[1]);
            throw std::runtime_error(detail::errno_message("pipe failed"));
        }

        const pid_t pid = fork();
        if (pid == -1) {
            close(to_child[0]);
            close(to_child[1]);
            close(to_parent[0]);
            close(to_parent[1]);
            throw std::runtime_error(detail::errno_message("fork failed"));
        }

        if (pid == 0) {
            close(to_child[1]);
            close(to_parent[0]);
            detail::worker_loop(to_child[0], to_parent[1], shared_data, width);
        }

        close(to_child[0]);
        close(to_parent[1]);
        workers.push_back(WorkerProcess{pid, to_child[1], to_parent[0]});
    };

    for (std::size_t i = 0; i < process_budget; ++i) {
        spawn_worker();
    }

    constexpr double kEpsilon = 1e-12;

    auto send_task = [&](std::size_t worker_index, detail::WorkerCommand command, std::size_t column,
                         std::size_t start, std::size_t end) {
        detail::WorkerTask task{};
        task.command = static_cast<std::size_t>(command);
        task.column = column;
        task.start_row = start;
        task.end_row = end;
        if (!detail::fd_write_full(workers[worker_index].write_fd, &task, sizeof(task))) {
            throw std::runtime_error(detail::errno_message("write to worker failed"));
        }
    };

    auto wait_ack = [&](std::size_t worker_index) {
        detail::WorkerAck ack{};
        if (!detail::fd_read_full(workers[worker_index].read_fd, &ack, sizeof(ack))) {
            throw std::runtime_error(detail::errno_message("read from worker failed"));
        }
        if (ack.status != 0) {
            throw std::runtime_error("worker reported failure");
        }
    };

    for (std::size_t col = 0; col < n; ++col) {
        const double pivot = shared_data[col * width + col];
        if (std::fabs(pivot) < kEpsilon) {
            throw std::runtime_error("Matrix is singular or ill-conditioned");
        }

        const std::size_t remaining_rows = n - col - 1;
        if (remaining_rows == 0) {
            continue;
        }

        const std::size_t active_workers = std::min(process_budget, remaining_rows);
        const std::size_t chunk = (remaining_rows + active_workers - 1) / active_workers;

        std::size_t assigned = 0;
        for (; assigned < active_workers; ++assigned) {
            const std::size_t start = col + 1 + assigned * chunk;
            if (start >= n) {
                break;
            }
            const std::size_t end = std::min(n, start + chunk);
            send_task(assigned, detail::WorkerCommand::Work, col, start, end);
        }

        for (std::size_t idx = 0; idx < assigned; ++idx) {
            wait_ack(idx);
        }
    }

    for (std::size_t idx = 0; idx < workers.size(); ++idx) {
        send_task(idx, detail::WorkerCommand::Exit, 0, 0, 0);
    }

    for (std::size_t idx = 0; idx < workers.size(); ++idx) {
        wait_ack(idx);
    }

    for (const auto &worker : workers) {
        int status = 0;
        while (waitpid(worker.pid, &status, 0) == -1) {
            if (errno != EINTR) {
                throw std::runtime_error(detail::errno_message("waitpid failed"));
            }
        }
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            throw std::runtime_error("worker exited abnormally");
        }
        close_fd(worker.write_fd);
        close_fd(worker.read_fd);
    }

    std::vector<double> solution(n, 0.0);
    for (std::size_t i = n; i-- > 0;) {
        double rhs = shared_data[i * width + (width - 1)];
        for (std::size_t j = i + 1; j < n; ++j) {
            rhs -= shared_data[i * width + j] * solution[j];
        }

        const double pivot = shared_data[i * width + i];
        if (std::fabs(pivot) < kEpsilon) {
            throw std::runtime_error("Matrix is singular or ill-conditioned");
        }

        solution[i] = rhs / pivot;
    }

    return solution;
}
