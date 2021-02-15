#include "cxx_test_helper.hpp"

#include <cstdlib>

int main() { return test::TestPool::run_all( test::Logger() ) ? EXIT_SUCCESS : EXIT_FAILURE; }
