#include "utils.hpp"
#include <concurrent/concurrent.hpp>
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <numeric>
#include <ranges>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace concurrent;
using namespace std::chrono_literals;

TEST_SUITE("concurrent.parallel") {

    // 惰性语义

    // 核心契约: 构造视图不提交任何任务, 未被迭代的视图什么也不做
    TEST_CASE("no_iteration_no_submission") {
        pool p({.threads = 4});
        std::atomic<int> calls{0};
        std::vector<int> data(100, 1);

        {
            auto v = parallel_map(p, data, [&calls](int x) {
                calls.fetch_add(1, std::memory_order_relaxed);
                return x;
            });
            CHECK(v.submitted() == std::size_t{0}); // launch 之前无槽位
            std::this_thread::sleep_for(20ms);      // 给"若已提交"充分的执行窗口
            CHECK(calls.load() == 0);
        }
        p.wait();
        CHECK(calls.load() == 0);
    }

    TEST_CASE("first_begin_submits_whole_batch") {
        pool p({.threads = 4});
        std::vector<int> data(64, 2);
        auto v = parallel_map(p, data, [](int x) { return x; });
        CHECK(v.submitted() == std::size_t{0});

        auto it = v.begin(); // begin() 即整批入队
        CHECK(v.submitted() == std::size_t{64});
        CHECK(it != v.end());
    }

    TEST_CASE("repeated_begin_is_idempotent") {
        pool p({.threads = 4});
        std::vector<int> data(32, 1);
        std::atomic<int> calls{0};
        auto v = parallel_map(p, data, [&calls](int x) {
            calls.fetch_add(1, std::memory_order_relaxed);
            return x;
        });
        static_cast<void>(v.begin());
        static_cast<void>(v.begin()); // launch 幂等
        CHECK(v.submitted() == std::size_t{32});
        static_cast<void>(v.run());
        CHECK(calls.load() == 32);
    }

    // 结果与顺序

    TEST_CASE("results_arrive_in_input_order") {
        pool p({.threads = 8});
        std::vector<int> data(200);
        std::iota(data.begin(), data.end(), 0);

        auto v = parallel_map(p, data, [](int x) {
            // 反向睡眠: 靠前的元素故意跑得更慢, 逼出乱序完成
            if (x < 4) {
                std::this_thread::sleep_for(std::chrono::milliseconds(4 - x));
            }
            return x * x;
        });

        std::vector<int> got;
        for (auto&& r : v) {
            REQUIRE(r.has_value());
            got.push_back(*r);
        }
        REQUIRE(got.size() == data.size());
        bool ordered = true;
        for (std::size_t i = 0; i < got.size(); ++i) {
            ordered &= (got[i] == static_cast<int>(i * i)); // 严格按原序
        }
        CHECK(ordered);
    }

    TEST_CASE("elements_run_concurrently") {
        pool p({.threads = 4});
        std::atomic<int> concurrent_now{0};
        std::atomic<int> peak{0};
        std::vector<int> data(32, 0);

        auto v = parallel_map(p, data, [&](int) {
            const int cur = concurrent_now.fetch_add(1, std::memory_order_acq_rel) + 1;
            int prev = peak.load(std::memory_order_relaxed);
            while (cur > prev && !peak.compare_exchange_weak(prev, cur, std::memory_order_relaxed))
                ;
            std::this_thread::sleep_for(5ms);
            concurrent_now.fetch_sub(1, std::memory_order_acq_rel);
            return 0;
        });
        CHECK(v.run().has_value());
        CHECK(peak.load() >= 2); // 至少两个元素同时在跑
    }

    TEST_CASE("element_type_transformation") {
        pool p({.threads = 4});
        std::vector<int> data{1, 2, 3};
        auto v = parallel_map(p, data, [](int x) { return std::to_string(x) + "!"; });

        std::vector<std::string> got;
        for (auto&& r : v) {
            REQUIRE(r.has_value());
            got.push_back(*r);
        }
        CHECK(got == std::vector<std::string>({"1!", "2!", "3!"}));
    }

    TEST_CASE("move_only_result_type") {
        pool p({.threads = 4});
        std::vector<int> data{5, 6, 7};
        auto v = parallel_map(p, data, [](int x) { return std::make_unique<int>(x); });

        int sum = 0;
        for (auto&& r : v) {
            REQUIRE(r.has_value());
            sum += **r;
        }
        CHECK(sum == 18);
    }

    TEST_CASE("empty_range_yields_nothing") {
        pool p({.threads = 2});
        std::vector<int> empty;
        auto v = parallel_map(p, empty, [](int x) { return x; });

        std::size_t n = 0;
        for (auto&& r : v) {
            static_cast<void>(r);
            ++n;
        }
        CHECK(n == std::size_t{0});
        CHECK(v.submitted() == std::size_t{0});
        CHECK(v.batch_error() == nullptr);
    }

    // 区间种类

    // 左值区间 => 闭包按指针携带元素, f 可原地改写底层容器
    TEST_CASE("lvalue_range_carries_by_pointer") {
        pool p({.threads = 4});
        std::vector<int> data(50);
        std::iota(data.begin(), data.end(), 0);

        auto v = parallel_map(p, data, [](int& x) {
            x *= 3; // 直接落到底层容器
            return x;
        });
        CHECK(v.run().has_value());

        bool tripled = true;
        for (std::size_t i = 0; i < data.size(); ++i) {
            tripled &= (data[i] == static_cast<int>(i) * 3);
        }
        CHECK(tripled);
    }

    // 生成式区间产出 prvalue => 必须按值携带, 否则闭包运行时已悬垂
    TEST_CASE("generated_range_carries_by_value") {
        pool p({.threads = 4});
        auto v = parallel_map(p, std::views::iota(0, 100), [](int x) { return x + 1; });

        long sum = 0;
        for (auto&& r : v) {
            REQUIRE(r.has_value());
            sum += *r;
        }
        CHECK(sum == 5050L); // 1..100
    }

    TEST_CASE("accepts_piped_views") {
        pool p({.threads = 4});
        auto rng = std::views::iota(1, 21) | std::views::filter([](int x) { return x % 2 == 0; });
        auto v = parallel_map(p, rng, [](int x) { return x * 10; });

        std::vector<int> got;
        for (auto&& r : v) {
            REQUIRE(r.has_value());
            got.push_back(*r);
        }
        CHECK(got == std::vector<int>({20, 40, 60, 80, 100, 120, 140, 160, 180, 200}));
    }

    // parallel_for

    TEST_CASE("parallel_for_side_effects") {
        pool p({.threads = 4});
        std::atomic<long> sum{0};
        auto v = parallel_for(p, std::views::iota(1, 1001),
                              [&sum](int x) { sum.fetch_add(x, std::memory_order_relaxed); });
        CHECK(v.run().has_value());
        CHECK(sum.load() == 500500L);
    }

    TEST_CASE("parallel_for_writes_output_by_index") {
        pool p({.threads = 4});
        constexpr std::size_t n = 500;
        std::vector<int> out(n, 0);
        auto v = parallel_for(p, std::views::iota(std::size_t{0}, n),
                              [&out](std::size_t i) { out[i] = static_cast<int>(i) * 2; });
        CHECK(v.run().has_value());

        bool ok = true;
        for (std::size_t i = 0; i < n; ++i) {
            ok &= (out[i] == static_cast<int>(i) * 2);
        }
        CHECK(ok);
    }

    TEST_CASE("parallel_for_iteration_yields_void_expected") {
        pool p({.threads = 4});
        std::vector<int> data(10, 1);
        auto v = parallel_for(p, data, [](int&) {});

        std::size_t ok = 0;
        for (auto&& r : v) {
            ok += r.has_value() ? 1u : 0u;
        }
        CHECK(ok == std::size_t{10});
    }

    // 错误通道

    TEST_CASE("per_element_error_isolation") {
        pool p({.threads = 4});
        std::vector<int> data(20);
        std::iota(data.begin(), data.end(), 0);

        auto v = parallel_map(p, data, [](int x) -> int {
            if (x % 5 == 0) {
                throw std::runtime_error("bad");
            }
            return x;
        });

        std::size_t failed = 0, ok = 0;
        std::size_t i = 0;
        for (auto&& r : v) {
            if (r) {
                ++ok;
                CHECK(*r == static_cast<int>(i)); // 成功元素的值仍与位置对应
            } else {
                ++failed;
                CHECK(r.error() != nullptr);
            }
            ++i;
        }
        CHECK(failed == std::size_t{4}); // 0,5,10,15
        CHECK(ok == std::size_t{16});
    }

    TEST_CASE("run_returns_first_error") {
        pool p({.threads = 4});
        std::vector<int> data(30);
        std::iota(data.begin(), data.end(), 0);
        auto v = parallel_map(p, data, [](int x) -> int {
            if (x == 7) {
                throw std::runtime_error("seven");
            }
            return x;
        });

        auto r = v.run();
        REQUIRE(!r.has_value());

        std::string what;
        try {
            std::rethrow_exception(r.error());
        } catch (const std::runtime_error& e) {
            what = e.what();
        } catch (...) {
        }
        CHECK(what == std::string("seven"));
    }

    TEST_CASE("run_all_success_has_no_error") {
        pool p({.threads = 4});
        std::vector<int> data(100, 1);
        auto v = parallel_map(p, data, [](int x) { return x; });
        CHECK(v.run().has_value());
    }

    // 池已关闭 => 每个元素的提交都失败, 错误经 submit_error_of 可还原
    TEST_CASE("submit_failure_on_stopped_pool") {
        pool p({.threads = 2});
        p.shutdown();
        std::vector<int> data(5, 1);
        auto v = parallel_map(p, data, [](int x) { return x; });

        std::size_t stopped = 0;
        for (auto&& r : v) {
            REQUIRE(!r.has_value());
            auto se = submit_error_of(r.error());
            if (se && *se == submit_error::stopped) {
                ++stopped;
            }
        }
        CHECK(stopped == std::size_t{5});
    }

    // 提交失败(池已关)不计入 submitted: 它统计的是真正入队的任务数
    TEST_CASE("submitted_counts_only_successful_submissions") {
        pool p({.threads = 2});
        p.shutdown();
        std::vector<int> data(5, 1);
        auto v = parallel_map(p, data, [](int x) { return x; });
        static_cast<void>(v.run());
        CHECK(v.submitted() == std::size_t{0});
    }

    // 提交期非 OOM 异常(此处: 按值搬运元素的拷贝构造抛出)必须原样透传,
    // 不得被误标为 out_of_memory
    TEST_CASE("non_oom_submission_failure_preserves_exception") {
        pool p({.threads = 2});
        struct throwy {
            throwy() = default;
            throwy(const throwy&) { throw std::runtime_error("copy boom"); }
        };
        auto rng =
            std::views::iota(0, 3) | std::views::transform([](int) { return throwy{}; });
        auto v = parallel_map(p, rng, [](const throwy&) { return 1; });

        std::size_t n = 0;
        bool saw_boom = false;
        for (auto&& r : v) {
            ++n;
            if (!r.has_value()) {
                try {
                    std::rethrow_exception(r.error());
                } catch (const std::runtime_error& e) {
                    saw_boom = (e.what() == std::string("copy boom"));
                } catch (...) {
                }
            }
        }
        CHECK(n == std::size_t{1}); // 整批失败 -> 仅末尾一个哨兵错误元素
        CHECK(saw_boom);
        REQUIRE(v.batch_error() != nullptr); // 迭代触发 launch 后整批错误可见
        CHECK(!submit_error_of(v.batch_error()).has_value()); // 不是 submit_error 类别
        CHECK(v.submitted() == std::size_t{0});               // 无一成功提交
    }

    // 生命周期

    // 析构必须阻塞至全部已提交任务完成, 闭包持有 f 与元素指针
    TEST_CASE("view_destructor_waits_pending_tasks") {
        pool p({.threads = 4});
        std::atomic<int> done{0};
        std::vector<int> data(64, 1);

        {
            auto v = parallel_map(p, data, [&done](int x) {
                std::this_thread::sleep_for(2ms);
                done.fetch_add(1, std::memory_order_relaxed);
                return x;
            });
            static_cast<void>(v.begin()); // 只提交, 不取回结果
        } // 析构在此阻塞
        CHECK(done.load() == 64);
    }

    TEST_CASE("works_with_flagged_pool") {
        basic_pool<decltype(priority), decltype(trace)> p({.threads = 4});
        std::vector<int> data(40, 3);
        auto v = parallel_map(p, data, [](int x) { return x + 1; });

        long sum = 0;
        for (auto&& r : v) {
            sum += r.value_or(0);
        }
        CHECK(sum == 160L);
    }

    TEST_CASE("nested_inside_task") {
        pool p({.threads = 4});
        auto outer = p.submit([&p] {
            std::vector<int> data(50, 2);
            auto v = parallel_map(p, data, [](int x) { return x * 5; });

            long s = 0;
            for (auto&& r : v) {
                s += r.value_or(0);
            }
            return s;
        });
        REQUIRE(outer.has_value());
        CHECK(outer->get().value_or(-1) == 500L);
    }

    TEST_CASE("partially_iterated_view_destroys_remaining_results") {
        // 只消费前几个结果就析构视图: 余下已完成任务的结果值仍须被析构
        const int base = tu::tracked::live.load();
        {
            pool p({.threads = 4});
            std::vector<int> data(64);
            std::iota(data.begin(), data.end(), 0);

            auto v = parallel_map(p, data, [](int x) { return tu::tracked{x}; });
            int seen = 0;
            for (auto&& r : v) {
                if (r) {
                    ++seen;
                }
                if (seen == 4) {
                    break; // 提前跳出, 其余 60 个结果无人认领
                }
            }
            CHECK(seen == 4);
        }
        CHECK(tu::tracked::live.load() == base);
    }
}
