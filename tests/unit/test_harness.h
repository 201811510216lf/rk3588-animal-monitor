#pragma once

#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

// 极简单元测试框架：零外部依赖，每个测试可执行文件自带 main()。
// 用法见 tests/unit/test_bounded_queue.cpp。
namespace am_test
{

struct TestCase
{
    const char* name;
    void (*fn)();
};

inline std::vector<TestCase>& Registry()
{
    static std::vector<TestCase> registry;
    return registry;
}

struct Registrar
{
    explicit Registrar(TestCase test_case) { Registry().push_back(test_case); }
};

inline int RunAll()
{
    int failed = 0;
    for (const TestCase& test_case : Registry())
    {
        try
        {
            test_case.fn();
            std::printf("[ OK ] %s\n", test_case.name);
        }
        catch (const std::exception& e)
        {
            ++failed;
            std::printf("[FAIL] %s: %s\n", test_case.name, e.what());
        }
        catch (...)
        {
            ++failed;
            std::printf("[FAIL] %s: unknown exception\n", test_case.name);
        }
    }
    const int total = static_cast<int>(Registry().size());
    std::printf("%d/%d tests passed\n", total - failed, total);
    return failed == 0 ? 0 : 1;
}

}  // namespace am_test

#define AM_TEST(name)                                      \
    static void                       am_test_fn_##name(); \
    static const ::am_test::Registrar am_test_reg_##name{  \
        ::am_test::TestCase{#name, &am_test_fn_##name}};   \
    static void am_test_fn_##name()

#define AM_EXPECT(cond)                                                               \
    do                                                                                \
    {                                                                                 \
        if (!(cond))                                                                  \
        {                                                                             \
            throw std::runtime_error(std::string("expect failed: ") + #cond +         \
                                     " at " __FILE__ ":" + std::to_string(__LINE__)); \
        }                                                                             \
    }                                                                                 \
    while (0)

#define AM_EXPECT_EQ(a, b)                                                                      \
    do                                                                                          \
    {                                                                                           \
        const auto am_va = (a);                                                                 \
        const auto am_vb = (b);                                                                 \
        if (!(am_va == am_vb))                                                                  \
        {                                                                                       \
            throw std::runtime_error(std::string("expect_eq failed: ") + #a +                   \
                                     " == " #b " at " __FILE__ ":" + std::to_string(__LINE__)); \
        }                                                                                       \
    }                                                                                           \
    while (0)
