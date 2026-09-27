// headless self-test: drives strata::context with scripted input (no window, no gpu) and checks the behaviour of the
// ui logic. the checks live in selftest_<area>.cpp; strata_sandbox --selftest

#include "selftest.hpp"
#include "selftest_common.hpp"

void run_text_tests();
void run_dock_tests();
void run_widgets_tests();
void run_windows_tests();
void run_misc_tests();
void run_rows_tests();
void run_app_tests();
void run_new_tests();
void run_robust_tests();

int run_selftest()
{
    run_text_tests();
    run_dock_tests();
    run_widgets_tests();
    run_windows_tests();
    run_misc_tests();
    run_rows_tests();
    run_app_tests();
    run_new_tests();
    run_robust_tests();
    std::fprintf(stderr, "selftest: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
