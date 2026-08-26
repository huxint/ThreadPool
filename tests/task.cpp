#include "utils.hpp"
#include <concurrent/concurrent.hpp>
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>

using namespace concurrent;
using namespace std::chrono_literals;

TEST_SUITE("concurrent.task") {

    // map

    TEST_CASE("map_transforms_success_value") {
        pool p({.threads = 4});
        auto t = p.submit([] { return 21; });
        REQUIRE(t.has_value());
        auto m = t->map([](int v) { return v * 2; });
        CHECK(m.get().value_or(-1) == 42);
    }

    TEST_CASE("map_changes_type") {
        pool p({.threads = 2});
        auto t = p.submit([] { return 7; });
        REQUIRE(t.has_value());
        auto m = t->map([](int v) { return std::to_string(v) + "!"; });
        CHECK(m.get().value_or(std::string{}) == std::string("7!"));
    }

    TEST_CASE("map_chains") {
        pool p({.threads = 2});
        auto t = p.submit([] { return 2; });
        REQUIRE(t.has_value());
        auto m = t->map([](int v) { return v + 3; }).map([](int v) { return v * 10; });
        CHECK(m.get().value_or(-1) == 50);
    }

    TEST_CASE("map_skips_failed_upstream") {
        pool p({.threads = 2});
        auto t = p.submit([]() -> int { throw std::runtime_error("upstream"); });
        REQUIRE(t.has_value());

        std::atomic<int> called{0};
        auto m = t->map([&called](int v) {
            called.fetch_add(1, std::memory_order_relaxed);
            return v;
        });
        CHECK(!m.get().has_value());
        CHECK(called.load() == 0); // 上游失败 => 变换体不执行
    }

    TEST_CASE("map_own_throw_into_error_channel") {
        pool p({.threads = 2});
        auto t = p.submit([] { return 1; });
        REQUIRE(t.has_value());
        auto m = t->map([](int) -> int { throw std::runtime_error("in map"); });
        CHECK(!m.get().has_value());
    }

    TEST_CASE("map_on_completed_task_runs_inline") {
        pool p({.threads = 2});
        auto t = p.submit([] { return 5; });
        REQUIRE(t.has_value());
        t->wait();                                    // 先等它跑完
        auto m = t->map([](int v) { return v * v; }); // 再附着 => 走内联路径
        CHECK(m.get().value_or(-1) == 25);
    }

    TEST_CASE("map_on_void_task_invokes_without_args") {
        pool p({.threads = 2});
        auto t = p.submit([] {});
        REQUIRE(t.has_value());
        auto m = t->map([] { return 8; }); // void 上游 => 变换体无参
        CHECK(m.get().value_or(-1) == 8);
    }

    TEST_CASE("map_skips_cancelled_upstream") {
        pool p({.threads = 1});
        tu::gate g;
        g.block_all(p, 1);

        auto t = p.submit([](std::stop_token) { return 1; });
        REQUIRE(t.has_value());
        t->request_stop();
        auto m = t->map([](int v) { return v + 1; });
        g.release();

        auto r = m.get();
        REQUIRE(!r.has_value());
        CHECK(is_cancelled(r.error())); // 取消语义沿链路传播
    }

    // and_then

    TEST_CASE("and_then_binds_next_task") {
        pool p({.threads = 4});
        auto t = p.submit([] { return 5; });
        REQUIRE(t.has_value());
        auto chained = t->and_then([&p](int v) {
            auto inner = p.submit([v] { return v * 10; });
            return inner ? std::move(*inner) : task<int>{};
        });
        CHECK(chained.get().value_or(-1) == 50);
    }

    TEST_CASE("and_then_inner_failure_propagates") {
        pool p({.threads = 4});
        auto t = p.submit([] { return 5; });
        REQUIRE(t.has_value());
        auto chained = t->and_then([&p](int) {
            auto inner = p.submit([]() -> int { throw std::runtime_error("inner"); });
            return inner ? std::move(*inner) : task<int>{};
        });
        CHECK(!chained.get().has_value());
    }

    TEST_CASE("and_then_skips_failed_upstream") {
        pool p({.threads = 2});
        auto t = p.submit([]() -> int { throw std::runtime_error("up"); });
        REQUIRE(t.has_value());

        std::atomic<int> called{0};
        auto chained = t->and_then([&p, &called](int v) {
            called.fetch_add(1, std::memory_order_relaxed);
            auto inner = p.submit([v] { return v; });
            return inner ? std::move(*inner) : task<int>{};
        });
        CHECK(!chained.get().has_value());
        CHECK(called.load() == 0);
    }

    TEST_CASE("and_then_fails_on_invalid_inner_task") {
        pool p({.threads = 2});
        auto t = p.submit([] { return 1; });
        REQUIRE(t.has_value());
        auto chained = t->and_then([](int) { return task<int>{}; }); // 故意给无效任务
        CHECK(!chained.get().has_value());
    }

    // inspect

    TEST_CASE("inspect_observes_without_changing_result") {
        pool p({.threads = 2});
        auto t = p.submit([] { return 3; });
        REQUIRE(t.has_value());

        std::atomic<int> seen{-1};
        auto ins = t->inspect([&seen](int& v) { seen.store(v, std::memory_order_release); });
        CHECK(ins.get().value_or(-1) == 3);
        CHECK(seen.load() == 3);
    }

    TEST_CASE("inspect_preserves_upstream_error") {
        pool p({.threads = 2});
        auto t = p.submit([]() -> int { throw std::runtime_error("e"); });
        REQUIRE(t.has_value());

        std::atomic<int> called{0};
        auto ins = t->inspect([&called](int&) { called.fetch_add(1, std::memory_order_relaxed); });
        CHECK(!ins.get().has_value());
        CHECK(called.load() == 0);
    }

    // when_all

    TEST_CASE("when_all_success_joins_tuple") {
        pool p({.threads = 4});
        auto a = p.submit([] { return 1; });
        auto b = p.submit([] { return std::string("two"); });
        auto c = p.submit([] { return 3.5; });
        REQUIRE(a.has_value());
        REQUIRE(b.has_value());
        REQUIRE(c.has_value());

        auto all = when_all(std::move(*a), std::move(*b), std::move(*c));
        auto r = all.get();
        REQUIRE(r.has_value());
        CHECK(std::get<0>(*r) == 1);
        CHECK(std::get<1>(*r) == std::string("two"));
        CHECK(std::get<2>(*r) == 3.5);
    }

    TEST_CASE("when_all_any_failure_fails_all") {
        pool p({.threads = 4});
        auto a = p.submit([] { return 1; });
        auto b = p.submit([]() -> int { throw std::runtime_error("bad"); });
        REQUIRE(a.has_value());
        REQUIRE(b.has_value());
        auto all = when_all(std::move(*a), std::move(*b));
        CHECK(!all.get().has_value());
    }

    TEST_CASE("when_all_empty_succeeds_immediately") {
        auto e = when_all();
        auto r = e.get();
        REQUIRE(r.has_value());
        CHECK(std::tuple_size_v<std::decay_t<decltype(*r)>> == std::size_t{0});
    }

    TEST_CASE("when_all_single_task") {
        pool p({.threads = 2});
        auto a = p.submit([] { return 9; });
        REQUIRE(a.has_value());
        auto all = when_all(std::move(*a));
        auto r = all.get();
        REQUIRE(r.has_value());
        CHECK(std::get<0>(*r) == 9);
    }

    TEST_CASE("when_all_composes_with_map") {
        pool p({.threads = 4});
        auto a = p.submit([] { return 100; });
        auto b = p.submit([] { return 200; });
        REQUIRE(a.has_value());
        REQUIRE(b.has_value());

        auto sum = when_all(std::move(*a), std::move(*b)).map([](auto&& tup) {
            return std::get<0>(tup) + std::get<1>(tup);
        });
        CHECK(sum.get().value_or(-1) == 300);
    }

    // 回归: 含无效任务时绝不可组装未填充的槽位
    TEST_CASE("when_all_invalid_task_fails_safely") {
        pool p({.threads = 2});
        auto a = p.submit([] { return 1; });
        REQUIRE(a.has_value());

        task<int> invalid;
        auto all = when_all(std::move(*a), std::move(invalid));
        CHECK(!all.get().has_value()); // 必须失败, 且不得读未初始化内存
    }

    TEST_CASE("when_all_move_only_results") {
        pool p({.threads = 4});
        auto a = p.submit([] { return std::make_unique<int>(11); });
        auto b = p.submit([] { return std::make_unique<int>(22); });
        REQUIRE(a.has_value());
        REQUIRE(b.has_value());

        auto all = when_all(std::move(*a), std::move(*b));
        auto r = all.get();
        REQUIRE(r.has_value());
        CHECK(*std::get<0>(*r) == 11);
        CHECK(*std::get<1>(*r) == 22);
    }

    // 无效任务守卫

    TEST_CASE("invalid_task_get_wait_and_request_stop_are_safe") {
        task<int> t;
        CHECK(!t.valid());
        CHECK(!t.get().has_value());
        t.wait();         // 不得崩溃
        t.request_stop(); // 不得崩溃
    }

    // 回归: 组合子必须在空共享状态上短路, 而非解引用空指针
    TEST_CASE("combinators_shortcircuit_on_invalid_task") {
        task<int> t;
        auto m = t.map([](int v) { return v; });
        CHECK(!m.valid());
        CHECK(!m.get().has_value());

        task<int> t2;
        auto i = t2.inspect([](int&) {});
        CHECK(!i.valid());
        CHECK(!i.get().has_value());

        task<int> t3;
        auto a = t3.and_then([](int) { return task<int>{}; });
        CHECK(!a.valid());
        CHECK(!a.get().has_value());
    }

    // 错误辨识器

    TEST_CASE("submit_error_of_recovers_submit_phase_failure") {
        pool p({.threads = 2});
        p.shutdown();
        auto t = p.submit([] { return 1; });
        REQUIRE(!t.has_value());

        auto as_ptr = std::make_exception_ptr(t.error());
        auto back = submit_error_of(as_ptr);
        REQUIRE(back.has_value());
        CHECK(*back == submit_error::stopped);
    }

    TEST_CASE("error_classifiers_reject_other_errors") {
        auto other = std::make_exception_ptr(std::runtime_error("x"));
        CHECK(!submit_error_of(other).has_value());
        CHECK(!is_cancelled(other));
        CHECK(!submit_error_of(nullptr).has_value());
        CHECK(!is_cancelled(nullptr));
    }

    // 结果所有权与规模

    TEST_CASE("move_only_result_through_result_channel") {
        pool p({.threads = 2});
        auto t = p.submit([] { return std::make_unique<std::string>("moved"); });
        REQUIRE(t.has_value());
        auto r = t->get();
        REQUIRE(r.has_value());
        CHECK(**r == std::string("moved"));
    }

    TEST_CASE("many_tasks_attach_combinators") {
        pool p({.threads = 4});
        constexpr int n = 500;
        std::vector<task<int>> chained;
        chained.reserve(n);

        for (int i = 0; i < n; ++i) {
            auto t = p.submit([i] { return i; });
            REQUIRE(t.has_value());
            chained.push_back(t->map([](int v) { return v * 2; }));
        }
        long sum = 0;
        for (auto& c : chained) {
            sum += c.get().value_or(0);
        }
        CHECK(sum == static_cast<long>(n) * (n - 1)); // 2 * n(n-1)/2
    }
}
