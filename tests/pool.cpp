#include "utils.hpp"
#include <concurrent/concurrent.hpp>
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <new>
#include <ranges>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <unistd.h>
#endif

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

    TEST_CASE("submit_each_maps_elements_and_batches_wake") {
        pool p({.threads = 4});
        std::vector<int> data(1000);
        for (int i = 0; i < 1000; ++i) {
            data[static_cast<std::size_t>(i)] = i;
        }

        auto r = p.submit_each(data, [](int x) { return x * 2; });
        REQUIRE(r.has_value());
        REQUIRE(r->size() == data.size());

        long long sum = 0;
        for (auto& t : *r) {
            sum += t.get().value_or(-1);
        }
        CHECK(sum == 999 * 1000); // 2 * Σ(0..999)
    }

    TEST_CASE("submit_each_supports_stop_token") {
        pool p({.threads = 2});
        const std::vector<int> data{1, 2, 3};
        auto r = p.submit_each(data, [](std::stop_token, int x) { return x + 10; });
        REQUIRE(r.has_value());
        CHECK((*r)[0].get().value_or(-1) == 11);
        CHECK((*r)[2].get().value_or(-1) == 13);
    }

    // 阶段一失败整体回滚: 用户拷贝异常原样透传, 已建节点的状态被终结, 池不悬挂
    TEST_CASE("submit_each_rolls_back_on_element_copy_throw") {
        struct explosive {
            int v;
            explicit explosive(int x) : v(x) {}
            explosive(const explosive& o) : v(o.v) {
                if (v == 3) {
                    throw std::runtime_error("copy");
                }
            }
        };
        tu::deadlock_watchdog wd(10s, "submit_each_rolls_back_on_element_copy_throw");
        pool p({.threads = 2});
        std::vector<int> in{1, 2, 3, 4};
        auto elems = in | std::views::transform([](int x) { return explosive{x}; });
        CHECK_THROWS_AS(static_cast<void>(p.submit_each(elems, [](explosive e) { return e.v; })),
                        std::runtime_error);
        p.wait();
    }

    TEST_CASE("submit_each_empty_range_no_wake") {
        pool p({.threads = 1});
        auto r = p.submit_each(std::vector<int>{}, [](int) { return 0; });
        REQUIRE(r.has_value());
        CHECK(r->empty());
    }

    TEST_CASE("submit_each_after_shutdown_returns_stopped") {
        pool p({.threads = 1});
        p.shutdown();
        auto r = p.submit_each(std::vector<int>{1, 2, 3}, [](int x) { return x; });
        REQUIRE(!r.has_value());
        CHECK(r.error() == submit_error::stopped);
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

    // 回归: 节点回收后 discard 钩子必须清除. 钩子只服务于"从未执行"的节点,
    // 而 execute 路径复用节点时不覆写该字段 -> 陈旧钩子会指向早已释放的共享
    // 状态, 关闭丢弃时 abandon 便在其上写入(ASan 实测 heap-use-after-free)
    TEST_CASE("recycled_node_drops_stale_discard_hook") {
        pool p({.threads = 1});

        // submit 型任务: 节点带 discard 钩子, 指向其共享状态
        {
            auto t = p.submit([] { return std::string("payload"); });
            REQUIRE(t.has_value());
            CHECK(t->get().value_or(std::string{}) == std::string("payload"));
        } // 句柄销毁 -> 共享状态释放; 节点回到 worker0 的空闲链

        std::atomic<bool> queued{false};
        std::atomic<bool> release{false};
        static_cast<void>(p.execute([&p, &queued, &release]() noexcept {
            // 在 worker 线程上嵌套提交 -> 取回上面那个节点
            for (int i = 0; i < 8; ++i) {
                static_cast<void>(p.execute([]() noexcept {}));
            }
            queued.store(true, std::memory_order_release);
            while (!release.load(std::memory_order_acquire)) {
                std::this_thread::yield(); // 卡住唯一 worker, 嵌套任务滞留队列
            }
        }));

        while (!queued.load(std::memory_order_acquire)) {
        }
        std::jthread killer([&p] { p.shutdown(shutdown_policy::discard); });
        std::this_thread::sleep_for(20ms); // 让 drop_all_queued 撞上那些节点
        release.store(true, std::memory_order_release);
        killer.join();
        CHECK(!p.running());
    }

    // 回归: 闭包构建失败(实参拷贝抛 bad_alloc)不得泄漏节点, 池保持可用.
    // 节点在"闭包构建成功"之后才被取出; 若先取后再构建, 每次失败即永久
    // 漏掉一个 128B 节点(计数分配器实测每次失败净增 +1), 且反复失败会抽干
    // 全局空闲节点池. ASan 构建另行覆盖"半成品节点"的误接行为
    TEST_CASE("failed_closure_construction_does_not_leak_or_disable_pool") {
        struct throwing_copy {
            int v = 0;
            explicit throwing_copy(int x) : v(x) {}
            throwing_copy(const throwing_copy&) { throw std::bad_alloc{}; }
            throwing_copy(throwing_copy&&) noexcept = default;
        };

        pool p({.threads = 2});
        throwing_copy t{7};
        for (int i = 0; i < 1000; ++i) {
            auto r = p.submit([](throwing_copy x) { return x.v; }, t); // 拷贝构造必抛
            CHECK(!r.has_value());
            CHECK(r.error() == submit_error::out_of_memory);
        }
        // 失败不得损坏后续提交路径: 池继续正常执行并完成
        {
            auto ok = p.submit([] { return 41; });
            REQUIRE(ok.has_value());
            CHECK(ok->get().value_or(0) == 41);
        }
        static_cast<void>(p.execute([]() noexcept {}));
        p.wait();
        CHECK(p.running());
    }

    TEST_CASE("shutdown_idempotent") {        pool p({.threads = 2});
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

    // 回归: shutdown(drain) 与并发提交之间有三条须守住的挂死窗口 -
    //   1. 提交被拒时裸减 pending 而不推进空闲代际, 挂在 idle_gen_ 上的
    //      wait() 等不到唤醒
    //   2. worker 以 stopping_ 为退出判据, 会在 drain 的 wait() 期间集体离场,
    //      而"已越过拒绝检查"的在途提交随后才落队 -> pending 永不归零
    //   3. worker 先检查退出条件再读 wake_gen_, 若两者之间恰好发生置位+递增,
    //      wait(g) 将永久阻塞 -> workers_.clear() 的 join 挂死
    //
    // 本用例按 1 与 2 的窗口定规模, 二者稳定复现. 路径 3 的窗口只有相邻两条
    // load 指令, 实测需上万轮才偶发一次(1200 轮检出率约 1/9), 靠堆轮数不划算 -
    // 其正确性由 worker_main 中"代际先于判据读取"的协议注释与推导保证
    TEST_CASE("shutdown_drain_race_with_submit_no_hang") {
        tu::deadlock_watchdog wd{60s, "shutdown_drain_race_with_submit_no_hang"};
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
        constexpr int rounds = 200; // 消毒器下单轮成本高一个量级, 相应缩减
#else
        constexpr int rounds = 1500;
#endif
        constexpr int producers = 8;

        int completed = 0;
        for (int r = 0; r < rounds; ++r) {
            pool p({.threads = 2});
            std::atomic<bool> go{false};
            std::vector<std::jthread> prods;
            prods.reserve(producers);
            for (int i = 0; i < producers; ++i) {
                prods.emplace_back([&p, &go] {
                    while (!go.load(std::memory_order_acquire)) {
                    }
                    for (int k = 0; k < 256; ++k) {
                        static_cast<void>(p.execute([]() noexcept {}));
                    }
                });
            }
            go.store(true, std::memory_order_release);
            std::this_thread::sleep_for(20us); // 让提交进入在途状态

            p.shutdown(shutdown_policy::drain); // 返回即证明未挂死
            prods.clear();
            if (!p.running()) {
                ++completed;
            }
        }
        CHECK(completed == rounds);
    }

    // 回归(活锁): 只要任何线程还在持续尝试提交(哪怕全部被拒),
    // shutdown(drain) 就必须仍能返回. 若被拒提交也瞬时抬高 pending_,
    // 收敛条件会被无限重置, 生产者与 shutdown 互等
    TEST_CASE("shutdown_returns_while_producers_keep_retrying") {
        tu::deadlock_watchdog wd{30s, "shutdown_returns_while_producers_keep_retrying"};
        pool p({.threads = 2});
        std::atomic<bool> stop{false};
        std::jthread prod{[&] {
            while (!stop.load(std::memory_order_acquire)) {
                static_cast<void>(p.execute([]() noexcept {})); // 被拒也继续重试
            }
        }};
        std::this_thread::sleep_for(2ms); // 让重试流稳定运转

        p.shutdown(shutdown_policy::drain); // 返回即证明活锁已消除

        stop.store(true, std::memory_order_release);
        prod.join();
        CHECK(!p.running());
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

    // 回归: 泛型 callable 对普通/可取消两条 execute 路径皆可行(须消歧).
    // 消歧规则: 无 token 也可调用的归普通重载; 必须 token 的归可取消重载
    TEST_CASE("execute_overload_disambiguation_on_generic_callable") {
        basic_pool<decltype(cancellable)> p({.threads = 1});

        // [](auto&&...) 无 token 也可调用 -> 普通重载, 返回 void 通道
        auto plain = p.execute([](auto&&...) noexcept {});
        REQUIRE(plain.has_value());

        // 首参固定 std::stop_token, 无 token 不可调用 -> 可取消重载
        std::atomic<bool> ran{false};
        auto src = p.execute([&ran](std::stop_token) noexcept { ran.store(true); });
        REQUIRE(src.has_value());
        p.wait();
        CHECK(ran.load());

        // 编译期侧证(经泛型 lambda 制造依赖上下文): 不可调用对象在
        // 重载处即被拒, 错误发生在调用点而非 submit_result_t 深处
        CHECK([]<typename PoolT>(PoolT&) constexpr {
            return !requires { std::declval<PoolT&>().submit(42); };
        }(p));
        CHECK([]<typename PoolT>(PoolT&) constexpr {
            return !requires { std::declval<PoolT&>().execute(42); };
        }(p));
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
            std::size_t worker;
        };
        std::mutex m;
        std::vector<record> events;
        auto sink = [&m, &events](trace_event e) noexcept {
            std::scoped_lock lk(m);
            events.push_back({e.id, e.phase, e.outcome, e.worker});
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
            // 归因: enqueue 发生在提交线程上(哨兵 no_worker), begin/end
            // 必来自池内 worker
            if (e.phase == task_phase::enqueue) {
                CHECK(e.worker == no_worker);
            } else {
                CHECK(e.worker < std::size_t{2});
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

    // 全局环缺省容量 1024/层, 提交超量后溢出链接管, 任何时刻都不丢任务
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

    // queue_cap 标签: 把两级容量都压到最小, 大批量提交几乎全程走溢出链,
    // 仍须一个不丢(正确性不依赖容量, 容量只影响快路径占比与内存)
    TEST_CASE("queue_cap_tiny_capacities_no_loss") {
        basic_pool<decltype(queue_cap<8, 2>)> p({.threads = 2});
        constexpr int n = 5000;
        tu::gate g;
        g.block_all(p, 2);

        std::atomic<int> done{0};
        std::jthread releaser([&g] {
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

    // 本地 deque 容量 2: worker 内嵌套提交溢出本地后落入全局, 不丢不拒
    TEST_CASE("queue_cap_local_overflow_falls_to_global") {
        basic_pool<decltype(queue_cap<1024, 2>)> p({.threads = 1});
        std::atomic<int> done{0};
        constexpr int children = 100;
        static_cast<void>(p.execute([&p, &done]() noexcept {
            // 父任务独占唯一 worker, 100 个子任务无人消费 -> 本地 2 槽必然溢出
            for (int i = 0; i < children; ++i) {
                static_cast<void>(p.execute([&done]() noexcept {
                    done.fetch_add(1, std::memory_order_relaxed);
                }));
            }
        }));
        p.wait();
        CHECK(done.load() == children);
    }

    // 自旋预算 0: worker 不自旋直接睡眠, 唤醒协议须仍然无损
    TEST_CASE("spin_budget_zero_still_processes_all") {
        pool p({.threads = 2, .spin_budget = 0us});
        constexpr int n = 2000;
        std::atomic<int> done{0};
        for (int i = 0; i < n; ++i) {
            static_cast<void>(p.execute([&done]() noexcept {
                done.fetch_add(1, std::memory_order_relaxed);
            }));
        }
        p.wait();
        CHECK(done.load() == n);
    }

    // 自旋预算可放大: 稀疏流量下仍正确(睡得更晚不改变协议)
    TEST_CASE("spin_budget_explicit_large") {
        pool p({.threads = 2, .spin_budget = 5ms});
        std::atomic<int> done{0};
        for (int i = 0; i < 100; ++i) {
            static_cast<void>(p.execute([&done]() noexcept {
                done.fetch_add(1, std::memory_order_relaxed);
            }));
        }
        p.wait();
        CHECK(done.load() == 100);
    }

    // 稀疏提交: 间隔大于自旋预算, 每轮 worker 都在熟睡, 唤醒全靠生产者的
    // 登记复查协议. 分别覆盖"全睡"(预算 0)与"有自旋者"(默认预算)两条路径
    TEST_CASE("sparse_submissions_never_lose_wakeup") {
        constexpr int n = 100;
        for (const auto budget : {0us, 64us}) {
            pool p({.threads = 2, .spin_budget = budget});
            std::atomic<int> done{0};
            for (int i = 0; i < n; ++i) {
                static_cast<void>(p.execute([&done]() noexcept {
                    done.fetch_add(1, std::memory_order_relaxed);
                }));
                if (i + 1 != n) {
                    std::this_thread::sleep_for(200us); // 长于预算: 提交前必已入睡
                }
            }
            const auto ok = p.wait_for(10s);
            CHECK(ok);
            CHECK(done.load() == n);
        }
    }

    // 批量入队须唤醒足够多的 worker: 任务们只在汇合点互相等待, 若只被一个
    // worker 认领则全员永久自旋(看门狗兜底); 串行消费同样无法越过汇合点
    TEST_CASE("submit_each_wakes_multiple_workers") {
        tu::deadlock_watchdog wd(30s, "submit_each_wakes_multiple_workers");
        pool p({.threads = 4, .spin_budget = 0us});
        constexpr int n = 4;
        std::atomic<int> arrived{0};
        std::atomic<int> done{0};
        std::vector<int> v(n);
        auto tasks = p.submit_each(v, [&](int) {
            arrived.fetch_add(1, std::memory_order_acq_rel);
            while (arrived.load(std::memory_order_acquire) < n) {
                std::this_thread::yield();
            }
            done.fetch_add(1, std::memory_order_relaxed);
            return 0;
        });
        REQUIRE(tasks.has_value());
        CHECK(p.wait_for(30s));
        CHECK(done.load() == n);
        for (auto& t : *tasks) {
            CHECK(t.get().value_or(-1) == 0);
        }
    }

    // drain 放行 worker 内嵌套提交: 闸门挡住全部 worker, 树整体在
    // stopping_ 之后展开 -> 每一次嵌套派生都走放行路径, 全部叶子须完成
    TEST_CASE("shutdown_drain_completes_nested_fork_join_tree") {
        pool p({.threads = 2});
        tu::gate g;
        g.block_all(p, 2);
        tu::deadlock_watchdog wd(30s, "shutdown_drain_completes_nested_fork_join_tree");

        std::atomic<int> leaves{0};
        struct fork {
            static void go(pool& p, int depth, std::atomic<int>& leaves) {
                if (depth == 0) {
                    leaves.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                static_cast<void>(
                    p.execute([&p, depth, &leaves]() noexcept { go(p, depth - 1, leaves); }));
                static_cast<void>(
                    p.execute([&p, depth, &leaves]() noexcept { go(p, depth - 1, leaves); }));
            }
        };
        fork::go(p, 10, leaves); // 根任务排队(未运行), 共 2^10 = 1024 叶

        std::jthread shut([&p] { p.shutdown(shutdown_policy::drain); });
        std::this_thread::sleep_for(5ms); // stopping_ 已置位而树仍被闸门挡着
        g.release();                     // 整棵树在停止状态下派生并完成
        shut.join();
        CHECK(leaves.load() == 1024);
    }

    // 同一窗口内外部提交仍须被拒: 放行只属于 worker 内的嵌套派生
    TEST_CASE("drain_rejects_external_but_allows_worker_nested") {
        pool p({.threads = 1});
        tu::gate g;
        g.block_all(p, 1);
        tu::deadlock_watchdog wd(30s, "drain_rejects_external_but_allows_worker_nested");

        std::atomic<bool> nested_ok{false};
        // root 排在闸门之后: 运行时池已处于 stopping 状态
        static_cast<void>(p.execute([&p, &nested_ok]() noexcept {
            nested_ok.store(p.execute([]() noexcept {}).has_value(),
                            std::memory_order_relaxed);
        }));

        std::jthread shut([&p] { p.shutdown(shutdown_policy::drain); });
        std::this_thread::sleep_for(5ms); // stopping_ 已置位, root 仍被闸门挡着
        CHECK(!p.execute([]() noexcept {}).has_value()); // 外部提交被拒
        g.release();
        shut.join();
        CHECK(nested_ok.load()); // worker 内嵌套提交被放行
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

    // 回归: 空闲节点缓存必须设上限. 无上限时外部线程持续提交会让每 worker 的
    // 缓存随累计任务数单调增长(节点归还进执行者的缓存, 外部生产者永远不取).
    // Linux-only 粗粒度冒烟: 百万次外部 execute 后 RSS 增量须低于宽松上界.
    // ASan 构建跳过: quarantine 把已归还内存滞留计费, RSS 不再反映真实滞留
    TEST_CASE("node_cache_bounded_rss_under_external_execute") {
#if defined(__linux__) && !defined(__SANITIZE_ADDRESS__)
        auto rss_pages = [] {
            std::FILE* f = std::fopen("/proc/self/statm", "re");
            if (!f) {
                return -1L;
            }
            long size = 0, resident = 0;
            const int got = std::fscanf(f, "%ld %ld", &size, &resident);
            std::fclose(f);
            return got == 2 ? resident : -1L;
        };
        const long page = sysconf(_SC_PAGESIZE);
        REQUIRE(page > 0);

        std::atomic<int> hits{0};
        {
            pool p({.threads = 4});
            const long before = rss_pages();
            REQUIRE(before > 0);

            constexpr int n = 1000000;
            int accepted = 0;
            for (int i = 0; i < n; ++i) {
                if (p.execute(
                        [&hits]() noexcept { hits.fetch_add(1, std::memory_order_relaxed); })) {
                    ++accepted;
                }
            }
            p.wait();
            REQUIRE(accepted == n);
            REQUIRE(hits.load() == n);

            const long after = rss_pages();
            REQUIRE(after > 0);
            const long delta_mb = (after - before) * page / (1024 * 1024);
            // 无上限缓存实测: 4 worker x 百万任务滞留约 120+ MiB(112B 节点
            // 入 malloc 128 桶); 上界 64 MiB 距满额缓存(4 x 128 KiB)与
            // 分配器噪声均有充分裕量
            CHECK(delta_mb < 64);
        }
#else
        static_cast<void>(0); // 非 Linux 或 ASan: 静默跳过
#endif
    }

    TEST_CASE("fork_join_work_first_recursive_sum") {
        pool p({.threads = 4});
        constexpr std::size_t n = 1uz << 16;
        std::atomic<long> sum{0};
        auto go = [&](auto&& self, std::size_t lo, std::size_t hi) -> void {
            if (hi - lo == 1) {
                sum.fetch_add(static_cast<long>(lo), std::memory_order_relaxed);
                return;
            }
            const std::size_t mid = lo + (hi - lo) / 2;
            (void)p.fork_join([&, lo, mid]() noexcept { self(self, lo, mid); },
                              [&, lo = mid, mid = hi]() noexcept { self(self, lo, mid); });
        };
        go(go, 0, n);
        p.wait();
        CHECK(sum.load() == n * (n - 1) / 2);
    }

    TEST_CASE("fork_join_inline_branch_runs_on_calling_thread") {
        pool p({.threads = 2});
        std::atomic<bool> submitted_ran{false};
        std::thread::id inline_id{};
        (void)p.fork_join([&]() noexcept { submitted_ran.store(true); },
                          [&] { inline_id = std::this_thread::get_id(); });
        CHECK(inline_id == std::this_thread::get_id());
        p.wait();
        CHECK(submitted_ran.load());
    }

    // 内联分支的异常按直接调用语义传播: 连 bad_alloc 也不折成提交失败
    TEST_CASE("fork_join_inline_branch_exception_propagates_unchanged") {
        pool p({.threads = 2});
        std::atomic<bool> forked{false};
        CHECK_THROWS_AS(static_cast<void>(p.fork_join(
                            [&]() noexcept { forked.store(true, std::memory_order_relaxed); },
                            [] { throw std::bad_alloc{}; })),
                        std::bad_alloc);
        p.wait();
        CHECK(forked.load());
    }

    TEST_CASE("fork_join_on_stopped_pool_returns_stopped_and_skips_inline") {
        pool p({.threads = 2});
        p.shutdown(shutdown_policy::discard);
        bool inline_ran = false;
        auto r = p.fork_join([]() noexcept {}, [&] { inline_ran = true; });
        CHECK(!r);
        CHECK(r.error() == submit_error::stopped);
        CHECK(!inline_ran);
    }
}
