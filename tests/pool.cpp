#include "utils.hpp"
#include <concurrent/concurrent.hpp>
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace concurrent;
using namespace std::chrono_literals;

TEST_SUITE("concurrent.pool") {

    // 提交语义

    TEST_CASE("submit_forwards_args_and_returns_value") {
        pool p({.threads = 4});
        auto a = p.submit([] { return 42; });
        auto b = p.submit([](int x, int y) { return x * y; }, 6, 7);
        auto c = p.submit([](std::string s) { return s + "!"; }, std::string("hi"));

        REQUIRE(a.has_value());
        REQUIRE(b.has_value());
        REQUIRE(c.has_value());
        CHECK(a->get().value_or(-1) == 42);
        CHECK(b->get().value_or(-1) == 42);
        CHECK(c->get().value_or(std::string{}) == std::string("hi!"));
    }

    TEST_CASE("submit_void_task") {
        pool p({.threads = 2});
        std::atomic<int> hits{0};
        auto t = p.submit([&hits] { hits.fetch_add(1, std::memory_order_relaxed); });
        REQUIRE(t.has_value());
        CHECK(t->get().has_value());
        CHECK(hits.load() == 1);
    }

    TEST_CASE("submit_result_consumed_once") {
        pool p({.threads = 2});
        auto t = p.submit([] { return std::string("once"); });
        REQUIRE(t.has_value());
        CHECK(t->get().value_or(std::string{}) == std::string("once"));
        CHECK(!t->get().has_value()); // 二次取值落入错误通道
    }

    TEST_CASE("submit_accepts_move_only_callable") {
        pool p({.threads = 2});
        auto payload = std::make_unique<int>(41);
        auto t = p.submit([v = std::move(payload)] { return *v + 1; });
        REQUIRE(t.has_value());
        CHECK(t->get().value_or(-1) == 42);
    }

    TEST_CASE("execute_fire_and_forget") {
        pool p({.threads = 4});
        std::atomic<int> total{0};
        constexpr int n = 500;
        int accepted = 0;
        for (int i = 0; i < n; ++i) {
            if (p.execute([&total]() noexcept { total.fetch_add(1, std::memory_order_relaxed); })) {
                ++accepted;
            }
        }
        p.wait();
        CHECK(accepted == n);
        CHECK(total.load() == n);
    }

    TEST_CASE("default_ctor_uses_hardware_concurrency") {
        pool p;
        CHECK(p.thread_count() >= 1);
        CHECK(p.running());
    }

    // 异常与错误通道

    TEST_CASE("task_exception_flows_to_error_channel") {
        pool p({.threads = 2});
        auto t = p.submit([]() -> int { throw std::runtime_error("boom"); });
        REQUIRE(t.has_value());

        auto r = t->get();
        REQUIRE(!r.has_value());
        CHECK(r.error() != nullptr);
        CHECK(!is_cancelled(r.error())); // 是失败, 不是取消

        std::string what;
        try {
            std::rethrow_exception(r.error());
        } catch (const std::runtime_error& e) {
            what = e.what();
        } catch (...) {
        }
        CHECK(what == std::string("boom"));
    }

    TEST_CASE("failing_task_does_not_break_pool") {
        pool p({.threads = 2});
        for (int i = 0; i < 20; ++i) {
            auto bad = p.submit([]() -> int { throw std::runtime_error("x"); });
            REQUIRE(bad.has_value());
            CHECK(!bad->get().has_value());
        }
        auto good = p.submit([] { return 9; });
        REQUIRE(good.has_value());
        CHECK(good->get().value_or(-1) == 9);
    }

    TEST_CASE("submit_to_stopped_pool_returns_stopped") {
        pool p({.threads = 2});
        p.shutdown();
        CHECK(!p.running());

        auto t = p.submit([] { return 1; });
        REQUIRE(!t.has_value());
        CHECK(t.error() == submit_error::stopped);

        auto e = p.execute([]() noexcept {});
        REQUIRE(!e.has_value());
        CHECK(e.error() == submit_error::stopped);
    }

    TEST_CASE("try_create_nothrow_entry") {
        auto p = pool::try_create({.threads = 2});
        REQUIRE(p.has_value());
        auto t = (*p)->submit([] { return 5; });
        REQUIRE(t.has_value());
        CHECK(t->get().value_or(-1) == 5);
    }

    // 生命周期

    TEST_CASE("destructor_defaults_drain_completes_all") {
        std::atomic<int> ran{0};
        {
            pool p({.threads = 4});
            for (int i = 0; i < 2000; ++i) {
                static_cast<void>(
                    p.execute([&ran]() noexcept { ran.fetch_add(1, std::memory_order_relaxed); }));
            }
        } // 析构 -> shutdown(drain)
        CHECK(ran.load() == 2000);
    }

    TEST_CASE("shutdown_drain_explicit") {
        std::atomic<int> ran{0};
        pool p({.threads = 3});
        for (int i = 0; i < 1000; ++i) {
            static_cast<void>(
                p.execute([&ran]() noexcept { ran.fetch_add(1, std::memory_order_relaxed); }));
        }
        p.shutdown(shutdown_policy::drain);
        CHECK(ran.load() == 1000);
    }

    TEST_CASE("shutdown_discard_drops_queued") {
        std::atomic<int> ran{0};
        pool p({.threads = 1});
        tu::gate g;
        g.block_all(p, 1);

        for (int i = 0; i < 500; ++i) {
            static_cast<void>(
                p.execute([&ran]() noexcept { ran.fetch_add(1, std::memory_order_relaxed); }));
        }

        // discard 不等待排空, 但 join worker 前须先放行闸门
        std::jthread releaser([&g] {
            std::this_thread::sleep_for(60ms);
            g.release();
        });
        p.shutdown(shutdown_policy::discard);
        CHECK(ran.load() < 500);
    }

    TEST_CASE("shutdown_discard_finalizes_queued_task_state") {
        // 被丢弃的 submit 任务: 结果通道必须以 operation_cancelled 终结并
        // 发布完成, 否则持有句柄的一方 get() 将永久阻塞. 唯一 worker 被闸门
        // 卡住 -> 任务必然处于排队态; 丢弃先于放行发生, 结局确定
        pool p({.threads = 1});
        tu::gate g;
        g.block_all(p, 1);

        auto submitted = p.submit([] { return 7; });
        REQUIRE(submitted.has_value());

        std::jthread dropper([&] { p.shutdown(shutdown_policy::discard); });
        auto r = submitted->get(); // 阻塞至丢弃终结发布完成
        g.release();               // 放行占位任务, 收敛循环方可在 pending 归零后结束
        dropper.join();

        REQUIRE(!r.has_value());
        CHECK(is_cancelled(r.error()));
    }

    TEST_CASE("shutdown_idempotent") {
        pool p({.threads = 2});
        static_cast<void>(p.execute([]() noexcept {}));
        p.shutdown();
        p.shutdown(shutdown_policy::discard);
        p.shutdown(shutdown_policy::drain);
        CHECK(!p.running());
    }

    // 回归: submit 在"读停止标志 -> 入队"之间存在窗口; 若 shutdown 的排空恰在
    // 两者之间完成, 节点会滞留队列且 pending 永不归零, 之后一切 wait 都将悬挂
    TEST_CASE("shutdown_discard_race_with_submit_no_hang") {
        constexpr int rounds = 8;
        for (int r = 0; r < rounds; ++r) {
            pool p({.threads = 4});
            std::atomic<bool> stop_submitter{false};

            std::jthread submitter([&] {
                while (!stop_submitter.load(std::memory_order_acquire)) {
                    static_cast<void>(p.execute([]() noexcept {}));
                }
            });

            std::this_thread::sleep_for(20ms);
            p.shutdown(shutdown_policy::discard);
            stop_submitter.store(true, std::memory_order_release);
            submitter.join();

            CHECK(p.wait_for(5s)); // 滞留节点会让此处的超时必然失败
        }
    }

    TEST_CASE("wait_for_timeout_and_completion") {
        pool p({.threads = 1});
        tu::gate g;
        g.block_all(p, 1);

        CHECK(!p.wait_for(20ms)); // 闸门未开 -> 超时
        g.release();
        CHECK(p.wait_for(5s));
    }

    TEST_CASE("wait_until_timeout_and_completion") {
        pool p({.threads = 1});
        tu::gate g;
        g.block_all(p, 1);

        CHECK(!p.wait_until(std::chrono::steady_clock::now() + 20ms));
        g.release();
        CHECK(p.wait_until(std::chrono::steady_clock::now() + 5s));
    }

    TEST_CASE("wait_returns_immediately_on_idle_pool") {
        pool p({.threads = 2});
        p.wait();
        CHECK(p.wait_for(0ms));
    }

    // 优先级

    TEST_CASE("priority_single_worker_high_first") {
        basic_pool<decltype(priority)> p({.threads = 1});
        tu::gate g;
        g.block_all(p, 1);

        std::mutex m;
        std::vector<int> order;
        auto record = [&m, &order](int tag) {
            std::scoped_lock lk(m);
            order.push_back(tag);
        };

        static_cast<void>(p.execute(task_priority::low, [&record]() noexcept { record(0); }));
        static_cast<void>(p.execute(task_priority::normal, [&record]() noexcept { record(1); }));
        static_cast<void>(p.execute(task_priority::high, [&record]() noexcept { record(2); }));

        g.release();
        p.wait();

        REQUIRE(order.size() == 3);
        CHECK(order == std::vector<int>({2, 1, 0})); // high -> normal -> low
    }

    TEST_CASE("priority_submit_accepts_level") {
        basic_pool<decltype(priority)> p({.threads = 2});
        auto t = p.submit(task_priority::high, [](int v) { return v + 1; }, 10);
        REQUIRE(t.has_value());
        CHECK(t->get().value_or(-1) == 11);
    }

    // 取消

    TEST_CASE("cancel_while_queued_skips_body") {
        pool p({.threads = 1});
        tu::gate g;
        g.block_all(p, 1);

        std::atomic<int> body_ran{0};
        auto t = p.submit([&body_ran](std::stop_token) {
            body_ran.fetch_add(1, std::memory_order_relaxed);
            return 7;
        });
        REQUIRE(t.has_value());
        t->request_stop(); // 尚在排队
        g.release();

        auto r = t->get();
        REQUIRE(!r.has_value());
        CHECK(is_cancelled(r.error()));
        CHECK(body_ran.load() == 0); // 任务体从未执行
    }

    TEST_CASE("cooperative_cancel_while_running") {
        pool p({.threads = 2});
        auto t = p.submit([](std::stop_token tok) {
            int spins = 0;
            while (!tok.stop_requested()) {
                ++spins;
                std::this_thread::sleep_for(1ms);
            }
            return spins;
        });
        REQUIRE(t.has_value());
        std::this_thread::sleep_for(20ms);
        t->request_stop();
        CHECK(t->get().has_value()); // 协作退出属正常完成
    }

    TEST_CASE("cancellable_execute_returns_stop_source") {
        basic_pool<decltype(cancellable)> p({.threads = 2});
        std::atomic<bool> observed_stop{false};
        auto src = p.execute([&observed_stop](std::stop_token tok) noexcept {
            while (!tok.stop_requested()) {
                std::this_thread::sleep_for(1ms);
            }
            observed_stop.store(true, std::memory_order_release);
        });
        REQUIRE(src.has_value());
        std::this_thread::sleep_for(20ms);
        src->request_stop();
        p.wait();
        CHECK(observed_stop.load());
    }

    TEST_CASE("uncancelled_token_task_completes_normally") {
        pool p({.threads = 2});
        auto t = p.submit([](std::stop_token tok) { return tok.stop_requested() ? -1 : 123; });
        REQUIRE(t.has_value());
        CHECK(t->get().value_or(-1) == 123);
    }

    // trace 钩子

    TEST_CASE("trace_three_phases_and_worker_attribution") {
        struct record {
            std::uint64_t id;
            task_phase phase;
            task_outcome outcome;
        };
        std::mutex m;
        std::vector<record> events;
        auto sink = [&m, &events](trace_event e) noexcept {
            std::scoped_lock lk(m);
            events.push_back({e.id, e.phase, e.outcome});
        };

        trace_hooks hooks;
        hooks.on_enqueue = sink;
        hooks.on_begin = sink;
        hooks.on_end = sink;

        {
            basic_pool<decltype(trace)> p({.threads = 2, .hooks = std::move(hooks)});
            auto t = p.submit([] { return 1; });
            REQUIRE(t.has_value());
            CHECK(t->get().value_or(-1) == 1);
            p.wait();
        }

        std::scoped_lock lk(m);
        int enq = 0, beg = 0, end = 0;
        for (const auto& e : events) {
            enq += e.phase == task_phase::enqueue;
            beg += e.phase == task_phase::begin;
            end += e.phase == task_phase::end;
            if (e.phase == task_phase::end) {
                CHECK(e.outcome == task_outcome::completed);
            }
        }
        CHECK(enq == 1);
        CHECK(beg == 1);
        CHECK(end == 1);
    }

    TEST_CASE("trace_reports_cancelled_outcome") {
        std::mutex m;
        std::vector<task_outcome> ends;
        trace_hooks hooks;
        hooks.on_end = [&m, &ends](trace_event e) noexcept {
            std::scoped_lock lk(m);
            ends.push_back(e.outcome);
        };

        basic_pool<decltype(trace)> p({.threads = 1, .hooks = std::move(hooks)});
        tu::gate g;
        g.block_all(p, 1);

        auto t = p.submit([](std::stop_token) { return 1; });
        REQUIRE(t.has_value());
        t->request_stop();
        g.release();
        static_cast<void>(t->get());
        p.wait();

        std::scoped_lock lk(m);
        bool saw_cancelled = false;
        for (auto o : ends) {
            saw_cancelled |= (o == task_outcome::cancelled);
        }
        CHECK(saw_cancelled);
    }

    TEST_CASE("trace_reports_failed_outcome") {
        std::mutex m;
        std::vector<task_outcome> ends;
        trace_hooks hooks;
        hooks.on_end = [&m, &ends](trace_event e) noexcept {
            std::scoped_lock lk(m);
            ends.push_back(e.outcome);
        };

        basic_pool<decltype(trace)> p({.threads = 2, .hooks = std::move(hooks)});
        auto t = p.submit([]() -> int { throw std::runtime_error("x"); });
        REQUIRE(t.has_value());
        static_cast<void>(t->get());
        p.wait();

        std::scoped_lock lk(m);
        bool saw_failed = false;
        for (auto o : ends) {
            saw_failed |= (o == task_outcome::failed);
        }
        CHECK(saw_failed);
    }

    // 特性标签组合

    TEST_CASE("worker_cap_static_storage_clamps_threads") {
        basic_pool<decltype(worker_cap<4>)> p({.threads = 4});
        CHECK(p.thread_count() == std::size_t{4});

        std::atomic<int> n{0};
        for (int i = 0; i < 100; ++i) {
            static_cast<void>(
                p.execute([&n]() noexcept { n.fetch_add(1, std::memory_order_relaxed); }));
        }
        p.wait();
        CHECK(n.load() == 100);
    }

    TEST_CASE("all_flags_combined") {
        basic_pool<decltype(priority), decltype(cancellable), decltype(trace),
                   decltype(worker_cap<8>)>
            p({.threads = 3});
        CHECK(p.thread_count() == std::size_t{3});

        auto t = p.submit(task_priority::high, [] { return 77; });
        REQUIRE(t.has_value());
        CHECK(t->get().value_or(-1) == 77);
    }

    // 工作窃取与压力

    TEST_CASE("nested_submit_from_worker") {
        pool p({.threads = 4});
        std::atomic<int> total{0};
        constexpr int outer = 64;
        constexpr int inner = 64;

        for (int i = 0; i < outer; ++i) {
            static_cast<void>(p.execute([&p, &total]() noexcept {
                for (int j = 0; j < inner; ++j) {
                    static_cast<void>(p.execute(
                        [&total]() noexcept { total.fetch_add(1, std::memory_order_relaxed); }));
                }
            }));
        }
        p.wait();
        CHECK(total.load() == outer * inner);
    }

    TEST_CASE("deep_recursive_nested_submit") {
        pool p({.threads = 4});
        std::atomic<int> leaves{0};

        struct fork {
            static void go(pool& p, int depth, std::atomic<int>& leaves) noexcept {
                if (depth == 0) {
                    leaves.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                for (int i = 0; i < 2; ++i) {
                    static_cast<void>(
                        p.execute([&p, depth, &leaves]() noexcept { go(p, depth - 1, leaves); }));
                }
            }
        };
        fork::go(p, 10, leaves); // 2^10 个叶子
        p.wait();
        CHECK(leaves.load() == 1024);
    }

    TEST_CASE("multi_producer_concurrent_submit") {
        pool p({.threads = 4});
        std::atomic<int> total{0};
        constexpr int producers = 4;
        constexpr int each = 5000;

        std::vector<std::jthread> ts;
        for (int t = 0; t < producers; ++t) {
            ts.emplace_back([&p, &total] {
                for (int i = 0; i < each; ++i) {
                    static_cast<void>(p.execute(
                        [&total]() noexcept { total.fetch_add(1, std::memory_order_relaxed); }));
                }
            });
        }
        ts.clear();
        p.wait();
        CHECK(total.load() == producers * each);
    }

    // 全局环容量为 GLOBAL_CAP/层, 提交超量后溢出链接管, 任何时刻都不丢任务
    TEST_CASE("global_queue_overflow_no_loss") {
        constexpr int n = 20000;
        pool p({.threads = 2});
        tu::gate g;
        g.block_all(p, 2);

        std::atomic<int> done{0};
        std::jthread releaser([&g] { // 生产者可能背压, 由旁路线程放行闸门
            std::this_thread::sleep_for(80ms);
            g.release();
        });

        int accepted = 0;
        for (int i = 0; i < n; ++i) {
            if (p.execute([&done]() noexcept { done.fetch_add(1, std::memory_order_relaxed); })) {
                ++accepted;
            }
        }
        p.wait();
        CHECK(accepted == n);
        CHECK(done.load() == n);
    }

    TEST_CASE("single_worker_preserves_fifo_order") {
        pool p({.threads = 1});
        std::vector<int> order;
        for (int i = 0; i < 100; ++i) {
            static_cast<void>(
                p.execute([&order, i]() noexcept { order.push_back(i); })); // 单 worker 免锁
        }
        p.wait();

        REQUIRE(order.size() == 100);
        bool sorted = true;
        for (int i = 0; i < 100; ++i) {
            sorted &= (order[static_cast<std::size_t>(i)] == i);
        }
        CHECK(sorted);
    }
}
