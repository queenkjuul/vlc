#include "cxx_test_helper.hpp"

#include <cstdlib>

TEST( AssertTrueShouldFail ) {
  const bool val = false;
  ASSERT_TRUE(val);
}
TEST( AssertTrueShouldPass ) {
  ASSERT_TRUE(true);
}

TEST( AssertFalseShouldFail ) {
  const bool val = true;
  ASSERT_FALSE(val);
}
TEST( AssertFalseShouldPass ) {
  ASSERT_FALSE(false);
}

TEST( AssertGTShouldFail ) {
  const int lhs = 1;
  const int rhs = 2;
  ASSERT_GT(lhs, rhs);
}
TEST( AssertGTShouldPass ) {
  ASSERT_GT(42, 3);
}

int main() { return test::TestPool::run_all_tests(test::Logger()); }
