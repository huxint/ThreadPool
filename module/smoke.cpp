// 模块冒烟测试: 证明 `import concurrent.pool;` 真能替代头文件引入
//
// 模式示例: 标准库头在前, import 在后(见 concurrent.cppm 顶部"混用须知")
// 列出的标准库头即库模板实例化所需的完整集合
#include <atomic>
#include <chrono>
#include <concepts>
#include <exception>
#include <expected>
#include <functional>
#include <generator>
#include <memory>
#include <new>
#include <numeric>
#include <optional>
#include <ranges>
#include <stop_token>
#include <tuple>
#include <typeinfo>
#include <utility>
#include <variant>
#include <vector>

import concurrent.pool;

int check(bool ok) {
    return ok ? 0 : 1;
}

int main() {
    concurrent::basic_pool<decltype(concurrent::priority)> p({.threads = 2});

    // submit + 结果通道
    auto t = p.submit([](int x) { return x * 2; }, 21);
    if (auto rc = check(t.has_value()); rc != 0) {
        return rc;
    }
    if (auto rc = check(t->get().value_or(0) == 42); rc != 0) {
        return rc;
    }

    // fire-and-forget(noexcept 强制)
    int hits = 0;
    if (auto e = p.execute([&hits]() noexcept { ++hits; }); !e) {
        return 1;
    }

    // 组合子: when_all + map
    auto a = p.submit([] { return 1; });
    auto b = p.submit([] { return 2; });
    if (!a || !b) {
        return 1;
    }
    auto sum = concurrent::when_all(std::move(*a), std::move(*b)).map([](auto&& tup) {
        return std::get<0>(tup) + std::get<1>(tup);
    });

    // 惰性批量: 原生数组区间, begin/end 迭代按序取回
    int data[4] = {1, 2, 3, 4};
    auto view = concurrent::parallel_map(p, data, [](int x) noexcept { return x * 10; });
    int total = 0;
    for (auto it = view.begin(); it != view.end(); ++it) {
        if (!*it) {
            return 1;
        }
        total += **it;
    }

    // 分块入口与视图类型经模块可达: parallel_view 模板名可命名(任意合法实例化),
    // chunked 求和按块取回
    int data2[6] = {1, 2, 3, 4, 5, 6};
    auto cv = concurrent::parallel_map_chunked(
        p, data2, [](auto&& c) { return std::accumulate(c.begin(), c.end(), 0); }, 3);
    using pv_proof =
        concurrent::parallel_view<decltype(p), decltype(std::views::all(data2)), int (*)(int)>;
    static_assert(
        std::same_as<typename pv_proof::value_type, std::expected<int, std::exception_ptr>>);
    int ctotal = 0;
    for (auto&& r : cv) {
        if (!r) {
            return 1;
        }
        ctotal += *r;
    }

    p.wait();
    if (sum.get().value_or(0) != 3 || hits != 1 || total != 100) {
        return 1;
    }
    if (ctotal != 21) { // 1..6 之和
        return 1;
    }
    return 0;
}
