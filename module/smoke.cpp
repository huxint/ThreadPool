// 模块冒烟测试: 证明 `import concurrent.pool;` 真能替代头文件引入
//
// 模式示例: 标准库头在前, import 在后(见 concurrent.cppm 顶部"混用须知")
// 列出的标准库头即库模板实例化所需的完整集合
#include <atomic>
#include <chrono>
#include <exception>
#include <expected>
#include <functional>
#include <memory>
#include <new>
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

    p.wait();
    if (sum.get().value_or(0) != 3 || hits != 1 || total != 100) {
        return 1;
    }
    return 0;
}
