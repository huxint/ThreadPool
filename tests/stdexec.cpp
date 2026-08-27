// stdexec (P2300) scheduler 集成测试: 独立目标, 经 -DWITH_STDEXEC=ON 启用
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <concurrent/concurrent.hpp>
#include <concurrent/execution.hpp>

#include <atomic>
#include <chrono>
#include <exception>
#include <optional>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <utility>

using namespace concurrent;

TEST_SUITE("concurrent.stdexec") {

    // scheduler 概念成立(结构化概念, 无需 tag)
    static_assert(stdexec::scheduler<ex::pool_scheduler<pool>>);

    TEST_CASE("scheduler_satisfies_concepts") {
        pool p({.threads = 2});
        auto sched = ex::as_scheduler(p);
        CHECK(stdexec::scheduler<decltype(sched)>);
    }

    TEST_CASE("schedule_then_sync_wait") {
        pool p({.threads = 2});
        auto sched = ex::as_scheduler(p);

        // then 链: 完成信号在池 worker 上发出, 值经 sync_wait 取回
        auto [val] = stdexec::sync_wait(sched.schedule() | stdexec::then([] {
                                             return 42;
                                         })).value();
        CHECK(val == 42);
    }

    TEST_CASE("when_all_runs_both_branches") {
        pool p({.threads = 4});
        auto sched = ex::as_scheduler(p);

        std::atomic<int> hits{0};
        auto work = [&]() -> int {
            hits.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            return 1;
        };

        auto [a, b] =
            stdexec::sync_wait(stdexec::when_all(sched.schedule() | stdexec::then(work),
                                                 sched.schedule() | stdexec::then(work)))
                .value();
        CHECK(a + b == 2);
        CHECK(hits.load() == 2);
    }

    // 两路真实并发: 各自 sleep 后有重叠窗口
    TEST_CASE("when_all_branches_overlap") {
        pool p({.threads = 2});
        auto sched = ex::as_scheduler(p);

        std::atomic<int> in_flight{0};
        std::atomic<int> peak{0};
        auto probe = [&]() -> int {
            int cur = in_flight.fetch_add(1, std::memory_order_acq_rel) + 1;
            int prev = peak.load(std::memory_order_relaxed);
            while (cur > prev &&
                   !peak.compare_exchange_weak(prev, cur, std::memory_order_relaxed)) {
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            in_flight.fetch_sub(1, std::memory_order_acq_rel);
            return 0;
        };

        auto r = stdexec::sync_wait(stdexec::when_all(sched.schedule() | stdexec::then(probe),
                                                      sched.schedule() | stdexec::then(probe)));
        REQUIRE(r.has_value());
        CHECK(peak.load() == 2);
    }

    TEST_CASE("exception_propagates_as_error") {
        pool p({.threads = 2});
        auto sched = ex::as_scheduler(p);

        // 异常经 set_error 送达; sync_wait 按 P2300 约定重抛(与本库组合子
        // 的 expected 通道不同, 文档已写明取舍)
        bool caught = false;
        try {
            static_cast<void>(stdexec::sync_wait(
                sched.schedule() | stdexec::then([]() -> int {
                    throw std::runtime_error("boom");
                })));
        } catch (const std::runtime_error& e) {
            caught = (e.what() == std::string("boom"));
        }
        CHECK(caught);
    }

    TEST_CASE("stopped_pool_completes_as_stopped") {
        pool p({.threads = 2});
        auto sched = ex::as_scheduler(p);
        p.shutdown();

        // 池已关: 提交被拒, 以 set_stopped 完成 - sync_wait 产出空 optional
        auto r = stdexec::sync_wait(sched.schedule() | stdexec::then([] { return 1; }));
        CHECK_FALSE(r.has_value());
    }

    TEST_CASE("chained_continuations_execute_on_pool") {
        pool p({.threads = 2});
        auto sched = ex::as_scheduler(p);

        // then 链的每段续延都从完成信号处接续: 整链在池上滚动
        auto [v] = stdexec::sync_wait(sched.schedule() | stdexec::then([] { return 1; })
                                      | stdexec::then([](int x) { return x * 10; })
                                      | stdexec::then([](int x) { return x + 5; }))
                       .value();
        CHECK(v == 15);
    }

    TEST_CASE("scheduler_equality_follows_pool_identity") {
        pool a({.threads = 1});
        pool b({.threads = 1});
        auto sa = ex::as_scheduler(a);
        auto sb = ex::as_scheduler(b);
        CHECK(sa == ex::as_scheduler(a));
        CHECK(sa != sb);
    }
}
