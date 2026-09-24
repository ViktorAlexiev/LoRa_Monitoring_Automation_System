#ifndef TEST_FRAMEWORK_H
#define TEST_FRAMEWORK_H

#include <cstdio>
#include <vector>
#include <string>
#include <functional>

struct TestCase {
  std::string name;
  std::function<void()> fn;
};
inline std::vector<TestCase>& testRegistry() {
  static std::vector<TestCase> reg;
  return reg;
}
struct TestRegistrar {
  TestRegistrar(const char *name, std::function<void()> fn) {
    testRegistry().push_back({name, fn});
  }
};
#define TEST(name) \
  static void test_##name(); \
  static TestRegistrar reg_##name(#name, test_##name); \
  static void test_##name()

inline int g_asserts = 0;
inline int g_failures = 0;
inline const char *g_currentTest = "";

#define ASSERT_TRUE(cond) do { \
  g_asserts++; \
  if (!(cond)) { \
    g_failures++; \
    printf("  [FAIL] %s:%d: ASSERT_TRUE(%s)\n", g_currentTest, __LINE__, #cond); \
  } \
} while (0)

#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))

#define ASSERT_EQ(a, b) do { \
  g_asserts++; \
  auto _a = (a); auto _b = (b); \
  if (!(_a == _b)) { \
    g_failures++; \
    printf("  [FAIL] %s:%d: ASSERT_EQ(%s, %s) -> got %ld, expected %ld\n", \
           g_currentTest, __LINE__, #a, #b, (long)_a, (long)_b); \
  } \
} while (0)

#define ASSERT_MEM_EQ(a, b, len) do { \
  g_asserts++; \
  if (memcmp((a), (b), (len)) != 0) { \
    g_failures++; \
    printf("  [FAIL] %s:%d: ASSERT_MEM_EQ(%s, %s, %d)\n", g_currentTest, __LINE__, #a, #b, (int)(len)); \
  } \
} while (0)

#define ASSERT_MEM_NE(a, b, len) do { \
  g_asserts++; \
  if (memcmp((a), (b), (len)) == 0) { \
    g_failures++; \
    printf("  [FAIL] %s:%d: ASSERT_MEM_NE(%s, %s, %d) - unexpectedly equal\n", g_currentTest, __LINE__, #a, #b, (int)(len)); \
  } \
} while (0)

inline int runAllTests() {
  int failedBefore;
  for (auto &tc : testRegistry()) {
    g_currentTest = tc.name.c_str();
    failedBefore = g_failures;
    tc.fn();
    if (g_failures == failedBefore) {
      printf("[PASS] %s\n", tc.name.c_str());
    } else {
      printf("[FAIL] %s\n", tc.name.c_str());
    }
  }
  printf("\n%d test cases, %d assertions, %d failed assertions\n",
         (int)testRegistry().size(), g_asserts, g_failures);
  return g_failures == 0 ? 0 : 1;
}

#endif
