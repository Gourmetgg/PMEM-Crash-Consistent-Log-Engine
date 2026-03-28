#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <new>
#include <random>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "PersistentLinkedList.h"

namespace {

using namespace std::chrono_literals;

constexpr std::size_t kMappedBytes = 8 * 1024 * 1024;
constexpr int kWorkerThreads = 6;
constexpr int kCrashRounds = 20;

struct SharedControl {
    std::atomic<std::uint32_t> ready_threads;
    std::atomic<std::uint32_t> pause_workers;
    std::atomic<std::uint32_t> stop_workers;
    std::atomic<std::uint64_t> attempted_inserts;
    std::atomic<std::uint64_t> successful_inserts;
};

[[noreturn]] void die_perror(const char* msg) {
    std::perror(msg);
    std::_Exit(111);
}

void worker_loop(pmem::PersistentLinkedList* list,
                 SharedControl* control,
                 std::atomic<std::uint64_t>* sequence,
                 int thread_id,
                 std::uint64_t seed_base) {
    std::mt19937_64 rng(seed_base + static_cast<std::uint64_t>(thread_id));
    std::uniform_int_distribution<int> backoff_us(1, 80);

    control->ready_threads.fetch_add(1, std::memory_order_release);

    while (control->stop_workers.load(std::memory_order_acquire) == 0U) {
        while (control->pause_workers.load(std::memory_order_acquire) != 0U &&
               control->stop_workers.load(std::memory_order_acquire) == 0U) {
            std::this_thread::sleep_for(std::chrono::microseconds(backoff_us(rng)));
        }

        const std::uint64_t value = sequence->fetch_add(1, std::memory_order_relaxed);
        control->attempted_inserts.fetch_add(1, std::memory_order_relaxed);

        if (list->insert(value)) {
            control->successful_inserts.fetch_add(1, std::memory_order_relaxed);
        }

        if ((value & 0x7U) == 0U) {
            std::this_thread::sleep_for(std::chrono::microseconds(backoff_us(rng)));
        }
    }
}

int run_worker_process(const std::string& mapped_file,
                       const std::string& shm_name,
                       std::uint64_t seed) {
    const int shm_fd = ::shm_open(shm_name.c_str(), O_RDWR, 0600);
    if (shm_fd < 0) {
        die_perror("shm_open worker");
    }

    void* control_mem = ::mmap(nullptr,
                               sizeof(SharedControl),
                               PROT_READ | PROT_WRITE,
                               MAP_SHARED,
                               shm_fd,
                               0);
    if (control_mem == MAP_FAILED) {
        die_perror("mmap shared control worker");
    }

    auto* control = static_cast<SharedControl*>(control_mem);

    std::unique_ptr<pmem::PersistentLinkedList> list;
    try {
        list = std::make_unique<pmem::PersistentLinkedList>(mapped_file, kMappedBytes);
    } catch (const std::exception& ex) {
        std::cerr << "worker init failed: " << ex.what() << std::endl;
        std::_Exit(112);
    }

    std::atomic<std::uint64_t> sequence{1};
    std::vector<std::thread> threads;
    threads.reserve(kWorkerThreads);

    for (int i = 0; i < kWorkerThreads; ++i) {
        threads.emplace_back(worker_loop, list.get(), control, &sequence, i, seed);
    }

    // Worker runs until parent sends SIGKILL (expected), or stop flag for graceful shutdown.
    while (control->stop_workers.load(std::memory_order_acquire) == 0U) {
        std::this_thread::sleep_for(200us);
    }

    for (auto& t : threads) {
        t.join();
    }

    ::munmap(control_mem, sizeof(SharedControl));
    ::close(shm_fd);
    return 0;
}

int run_recovery_process(const std::string& mapped_file) {
    try {
        pmem::PersistentLinkedList list(mapped_file, kMappedBytes);
        const bool recovered = list.recover();
        if (!recovered) {
            std::cerr << "recover() returned false" << std::endl;
            return 2;
        }

        const pmem::IntegrityReport report = list.validate();
        if (report.has_cycle || report.has_out_of_range_pointer) {
            std::cerr << "integrity failure after recovery: cycle=" << report.has_cycle
                      << " out_of_range=" << report.has_out_of_range_pointer << std::endl;
            return 3;
        }

        const std::vector<std::uint64_t> durable_values = list.snapshot(2'000'000);
        if (durable_values.size() > report.reachable_nodes) {
            std::cerr << "durable snapshot cannot exceed reachable nodes" << std::endl;
            return 4;
        }

        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "recovery process exception: " << ex.what() << std::endl;
        return 6;
    }
}

int spawn_and_wait_or_die(const std::vector<std::string>& args) {
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);

    pid_t pid = ::fork();
    if (pid < 0) {
        die_perror("fork");
    }

    if (pid == 0) {
        ::execv(argv[0], argv.data());
        die_perror("execv");
    }

    int status = 0;
    if (::waitpid(pid, &status, 0) < 0) {
        die_perror("waitpid");
    }

    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return 255;
}

int run_harness(const std::string& self_binary) {
    const std::string map_file = "/tmp/pmem_crash_engine_map.bin";
    const std::string shm_name = "/pmem_crash_engine_ctl_" + std::to_string(::getpid());

    std::filesystem::remove(map_file);

    const int shm_fd = ::shm_open(shm_name.c_str(), O_CREAT | O_RDWR | O_EXCL, 0600);
    if (shm_fd < 0) {
        die_perror("shm_open harness");
    }

    if (::ftruncate(shm_fd, sizeof(SharedControl)) != 0) {
        die_perror("ftruncate shared control");
    }

    void* control_mem = ::mmap(nullptr,
                               sizeof(SharedControl),
                               PROT_READ | PROT_WRITE,
                               MAP_SHARED,
                               shm_fd,
                               0);
    if (control_mem == MAP_FAILED) {
        die_perror("mmap shared control harness");
    }

    auto* control = static_cast<SharedControl*>(control_mem);
    control = new (control) SharedControl{};
    control->ready_threads.store(0, std::memory_order_release);
    control->pause_workers.store(0, std::memory_order_release);
    control->stop_workers.store(0, std::memory_order_release);
    control->attempted_inserts.store(0, std::memory_order_release);
    control->successful_inserts.store(0, std::memory_order_release);

    std::mt19937_64 rng(static_cast<std::uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    std::uniform_int_distribution<int> jitter_us(50, 1200);
    std::uniform_int_distribution<int> action(0, 2);

    for (int round = 0; round < kCrashRounds; ++round) {
        control->ready_threads.store(0, std::memory_order_release);
        control->pause_workers.store(0, std::memory_order_release);
        control->stop_workers.store(0, std::memory_order_release);

        pid_t worker_pid = ::fork();
        if (worker_pid < 0) {
            die_perror("fork worker");
        }

        if (worker_pid == 0) {
            const int rc = run_worker_process(map_file,
                                              shm_name,
                                              static_cast<std::uint64_t>(rng()) +
                                                  static_cast<std::uint64_t>(round));
            std::_Exit(rc);
        }

        while (control->ready_threads.load(std::memory_order_acquire) < static_cast<std::uint32_t>(kWorkerThreads)) {
            std::this_thread::sleep_for(100us);
        }

        std::this_thread::sleep_for(std::chrono::microseconds(jitter_us(rng)));

        const int crash_action = action(rng);
        if (crash_action == 0) {
            control->pause_workers.store(1, std::memory_order_release);
            std::this_thread::sleep_for(std::chrono::microseconds(jitter_us(rng)));
            control->pause_workers.store(0, std::memory_order_release);
            std::this_thread::sleep_for(std::chrono::microseconds(jitter_us(rng) / 2));
        } else if (crash_action == 1) {
            ::kill(worker_pid, SIGSTOP);
            std::this_thread::sleep_for(std::chrono::microseconds(jitter_us(rng)));
            ::kill(worker_pid, SIGCONT);
        }

        // Power-cut model: force kill -9 at unpredictable microsecond point.
        ::kill(worker_pid, SIGKILL);

        int status = 0;
        if (::waitpid(worker_pid, &status, 0) < 0) {
            die_perror("waitpid worker");
        }

        if (!(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL)) {
            std::cerr << "worker did not terminate with SIGKILL in round " << round << std::endl;
            return 10;
        }

        // Restart recovery process to emulate reboot + recovery.
        const int recover_rc = spawn_and_wait_or_die({self_binary, "--recover", map_file});
        if (recover_rc != 0) {
            std::cerr << "recovery subprocess failed on round " << round << " with rc=" << recover_rc
                      << std::endl;
            return 11;
        }
    }

    const std::uint64_t attempted = control->attempted_inserts.load(std::memory_order_relaxed);
    const std::uint64_t successful = control->successful_inserts.load(std::memory_order_relaxed);

    ::munmap(control_mem, sizeof(SharedControl));
    ::close(shm_fd);
    ::shm_unlink(shm_name.c_str());

    std::cout << "Crash simulation passed. attempted=" << attempted
              << " successful=" << successful << std::endl;

    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc >= 2) {
            const std::string mode = argv[1];
            if (mode == "--recover") {
                if (argc < 3) {
                    std::cerr << "usage: crash_recovery_test --recover <mapped_file>" << std::endl;
                    return 64;
                }
                return run_recovery_process(argv[2]);
            }
        }

        const std::string self = std::filesystem::absolute(argv[0]).string();
        return run_harness(self);
    } catch (const std::exception& ex) {
        std::cerr << "fatal exception: " << ex.what() << std::endl;
        return 70;
    }
}
