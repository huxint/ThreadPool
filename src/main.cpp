#include <cmath>
#include <cstddef>
#include <print>
#include <huxint/thread_pool.hpp>

int main() {
    ThreadPool pool;

    constexpr std::size_t num_tasks = 16;
    std::vector<std::future<double>> futures;

    auto start = std::chrono::high_resolution_clock::now();

    for (std::size_t i = 0; i < num_tasks; ++i) {
        futures.push_back(pool.submit([i] {
            double result = 0;
            for (int j = 0; j < 10000000; ++j) {
                result += std::sin(i + j * 0.001);
            }
            return result;
        }));
    }

    std::print("Submitted {} tasks\n", num_tasks);
    std::print("Pending tasks: {}\n", pool.pending_task_count());

    double total = 0;
    for (auto &f : futures) {
        total += f.get();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    std::print("All tasks completed in {}ms\n", duration.count());
    std::print("Total result: {}\n\n", total);
    return 0;
}