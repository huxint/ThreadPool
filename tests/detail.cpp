#include <concurrent/detail/chase_lev.hpp>
#include <concurrent/detail/mpmc_ring.hpp>
#include <concurrent/detail/node_cache.hpp>
#include <concurrent/detail/sbo_function.hpp>
#include <doctest/doctest.h>

#include <array>
#include <atomic>
#include <thread>
#include <vector>

using namespace concurrent::detail;

TEST_SUITE("concurrent.detail") {

    // 校验回收到的指针集合恰好覆盖 storage 的每个元素一次
    bool exactly_once(const std::vector<int*>& got, const std::vector<int>& storage,
                      std::size_t expected_total) {
        if (got.size() != expected_total) {
            return false;
        }
        std::vector<char> seen(storage.size(), 0);
        for (int* p : got) {
            const auto i = static_cast<std::size_t>(p - storage.data());
            if (i >= storage.size() || seen[i]) {
                return false;
            }
            seen[i] = 1;
        }
        return true;
    }

    TEST_CASE("chase_lev_owner_lifo_and_empty") {
        chase_lev_deque<int*, 8> dq;
        std::array<int, 3> a{1, 2, 3};

        CHECK(dq.pop() == nullptr);
        CHECK(dq.steal() == nullptr);

        CHECK(dq.push(&a[0]));
        CHECK(dq.push(&a[1]));
        CHECK(dq.push(&a[2]));
        CHECK(dq.size_approx() == std::size_t{3});

        CHECK(dq.pop() == &a[2]); // 所有者端 LIFO
        CHECK(dq.pop() == &a[1]);
        CHECK(dq.pop() == &a[0]);
        CHECK(dq.pop() == nullptr);
    }

    TEST_CASE("chase_lev_stealer_fifo_and_empty") {
        chase_lev_deque<int*, 8> dq;
        std::array<int, 3> a{1, 2, 3};
        CHECK(dq.push(&a[0]));
        CHECK(dq.push(&a[1]));
        CHECK(dq.push(&a[2]));

        CHECK(dq.steal() == &a[0]); // 窃取端 FIFO
        CHECK(dq.steal() == &a[1]);
        CHECK(dq.steal() == &a[2]);
        CHECK(dq.steal() == nullptr);
    }

    TEST_CASE("chase_lev_full_rejects_push") {
        chase_lev_deque<int*, 2> dq;
        std::array<int, 3> a{1, 2, 3};
        CHECK(dq.push(&a[0]));
        CHECK(dq.push(&a[1]));
        CHECK(!dq.push(&a[2])); // 满则拒绝, 不覆盖
    }

    // 回归: steal 必须"先读槽、后 CAS"; 若顺序反了, 环形缓冲绕回时所有者的
    // push 会与窃取者的槽访问相撞, 表现为任务丢失或重复取出
    TEST_CASE("chase_lev_concurrent_no_loss_no_dup") {
        constexpr std::size_t total = 200000;
        constexpr int stealers = 3;

        chase_lev_deque<int*, 64> dq; // 刻意取小容量, 逼出高频绕回
        std::vector<int> storage(total);
        std::atomic<bool> done{false};

        std::vector<std::vector<int*>> harvest(stealers + 1);
        std::vector<std::jthread> ts;
        for (int s = 0; s < stealers; ++s) {
            ts.emplace_back([&, s] {
                auto& out = harvest[static_cast<std::size_t>(s) + 1];
                while (true) {
                    if (int* p = dq.steal()) {
                        out.push_back(p);
                        continue;
                    }
                    if (done.load(std::memory_order_acquire)) {
                        if (int* p = dq.steal()) { // done 之后再补一轮, 避免漏拿
                            out.push_back(p);
                            continue;
                        }
                        break;
                    }
                }
            });
        }

        auto& mine = harvest[0];
        for (std::size_t pushed = 0; pushed < total;) {
            if (dq.push(&storage[pushed])) {
                ++pushed;
                if ((pushed & 7u) == 0) { // 穿插所有者端 pop, 制造 pop/steal 竞争
                    if (int* p = dq.pop()) {
                        mine.push_back(p);
                    }
                }
            } else if (int* p = dq.pop()) { // 满则自消化
                mine.push_back(p);
            }
        }
        done.store(true, std::memory_order_release);
        while (int* p = dq.pop()) {
            mine.push_back(p);
        }
        ts.clear(); // join

        std::vector<int*> all;
        for (auto& v : harvest) {
            all.insert(all.end(), v.begin(), v.end());
        }
        CHECK(exactly_once(all, storage, total));
    }

    TEST_CASE("mpmc_ring_fifo_full_and_empty") {
        mpmc_ring<int*, 4> ring;
        std::array<int, 5> a{1, 2, 3, 4, 5};

        CHECK(ring.try_pop() == nullptr);
        for (int i = 0; i < 4; ++i) {
            CHECK(ring.try_push(&a[static_cast<std::size_t>(i)]));
        }
        CHECK(!ring.try_push(&a[4])); // 满

        for (int i = 0; i < 4; ++i) {
            CHECK(ring.try_pop() == &a[static_cast<std::size_t>(i)]); // FIFO
        }
        CHECK(ring.try_pop() == nullptr);
    }

    TEST_CASE("mpmc_ring_concurrent_no_loss_no_dup") {
        constexpr std::size_t per_producer = 40000;
        constexpr int producers = 4;
        constexpr int consumers = 4;
        constexpr std::size_t total = per_producer * producers;

        mpmc_ring<int*, 512> ring;
        std::vector<int> storage(total);
        std::atomic<std::size_t> produced{0};
        std::atomic<std::size_t> consumed{0};

        std::vector<std::vector<int*>> harvest(consumers);
        std::vector<std::jthread> ps;
        for (int t = 0; t < producers; ++t) {
            ps.emplace_back([&, t] {
                const std::size_t base = static_cast<std::size_t>(t) * per_producer;
                for (std::size_t i = 0; i < per_producer; ++i) {
                    while (!ring.try_push(&storage[base + i])) {
                        std::this_thread::yield();
                    }
                    produced.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        std::vector<std::jthread> cs;
        for (int t = 0; t < consumers; ++t) {
            cs.emplace_back([&, t] {
                auto& out = harvest[static_cast<std::size_t>(t)];
                while (consumed.load(std::memory_order_acquire) < total) {
                    if (int* p = ring.try_pop()) {
                        out.push_back(p);
                        consumed.fetch_add(1, std::memory_order_release);
                    } else {
                        std::this_thread::yield();
                    }
                }
            });
        }
        ps.clear();
        cs.clear();

        CHECK(produced.load() == total);
        std::vector<int*> all;
        for (auto& v : harvest) {
            all.insert(all.end(), v.begin(), v.end());
        }
        CHECK(exactly_once(all, storage, total));
    }

    TEST_CASE("sbo_function_inplace_heap_and_move") {
        int witness = 0;
        sbo_function<64> small{[&witness] { witness = 1; }}; // 捕获一个引用 => 就地存储
        CHECK(static_cast<bool>(small));
        small();
        CHECK(witness == 1);

        // 超过 SBO 容量 => 走堆分配路径
        std::array<char, 256> bulk{};
        bulk[0] = 7;
        sbo_function<64> big{[&witness, bulk] { witness = bulk[0]; }};
        big();
        CHECK(witness == 7);

        // 移动后源置空, 目标可调用
        sbo_function<64> moved = std::move(big);
        CHECK(!static_cast<bool>(big));
        CHECK(static_cast<bool>(moved));
        witness = 0;
        moved();
        CHECK(witness == 7);

        moved.reset();
        CHECK(!static_cast<bool>(moved));

        sbo_function<64> empty;
        CHECK(!static_cast<bool>(empty));
    }

    // 移动赋值必须析构旧的可调用体, 否则生命周期计数泄漏
    TEST_CASE("sbo_function_move_assign_destroys_old") {
        struct tracker {
            std::atomic<int>* live;
            explicit tracker(std::atomic<int>* c) : live(c) { live->fetch_add(1); }
            tracker(const tracker& o) : live(o.live) { live->fetch_add(1); }
            tracker(tracker&& o) noexcept : live(o.live) { live->fetch_add(1); }
            ~tracker() { live->fetch_sub(1); }
            void operator()() const noexcept {}
        };

        std::atomic<int> live{0};
        {
            sbo_function<64> a{tracker{&live}};
            sbo_function<64> b{tracker{&live}};
            CHECK(live.load() == 2);
            a = std::move(b); // 旧的 a 必须被析构
            CHECK(live.load() == 1);
        }
        CHECK(live.load() == 0);
    }

    struct stub_node {
        stub_node* next_free = nullptr;
    };

    TEST_CASE("node_cache_caps_retention_and_reports_full") {
        node_cache<stub_node, 4> cache;
        stub_node a, b, c, d, e;

        CHECK(cache.pop() == nullptr); // 空
        CHECK(cache.size_approx() == std::size_t{0});

        CHECK(cache.push(&a));
        CHECK(cache.push(&b));
        CHECK(cache.push(&c));
        CHECK(cache.push(&d));
        CHECK(cache.size_approx() == std::size_t{4});

        CHECK(!cache.push(&e)); // 超限拒绝: 调用方负责销毁
        CHECK(cache.size_approx() == std::size_t{4});
    }

    TEST_CASE("node_cache_pop_drains_and_decrements") {
        node_cache<stub_node, 4> cache;
        stub_node a, b;

        CHECK(cache.push(&a));
        CHECK(cache.push(&b));

        stub_node* first = cache.pop();
        stub_node* second = cache.pop();
        CHECK(cache.pop() == nullptr); // 排空
        CHECK(cache.size_approx() == std::size_t{0});
        CHECK(((first == &a && second == &b) || (first == &b && second == &a)));
        CHECK(first != second);

        // 排空后可重新收容
        CHECK(cache.push(first));
        CHECK(cache.size_approx() == std::size_t{1});
    }

    // 回归: 无上限的空闲链在"外部生产者持续提交"下内存单调增长.
    // 并发压栈一轮, 验证计数与弹出总量一致(无丢失)
    TEST_CASE("node_cache_concurrent_push_accounting") {
        constexpr std::size_t cap = 64;
        node_cache<stub_node, cap> cache;
        std::vector<stub_node> nodes(cap * 4);
        std::atomic<std::size_t> rejected{0};

        std::vector<std::jthread> ts;
        for (int t = 0; t < 4; ++t) {
            ts.emplace_back([&, t] {
                const std::size_t base = static_cast<std::size_t>(t) * cap;
                for (std::size_t i = 0; i < cap; ++i) {
                    if (!cache.push(&nodes[base + i])) {
                        rejected.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }
        ts.clear();

        // 弹出总量 + 被拒量 == 总压栈量: 计数不丢
        std::size_t drained = 0;
        while (cache.pop()) {
            ++drained;
        }
        CHECK(drained + rejected.load() == cap * 4);
    }
}
