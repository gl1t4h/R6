#include <cstdio>

#include "test_support.h"

void run_software_gpu_tests();
void run_bios_services_tests();
void run_gte_tests();
void run_mips_interpreter_tests();
void run_chd_image_tests();
void run_native_runtime_tests();
void run_unaligned_tests();

int main() {
    run_software_gpu_tests();
    run_bios_services_tests();
    run_gte_tests();
    run_mips_interpreter_tests();
    run_chd_image_tests();
    run_native_runtime_tests();
    run_unaligned_tests();

    const auto failures = r6::test::failure_count();
    if (failures == 0) {
        std::printf("r6-tests: all checks passed\n");
        return 0;
    }
    std::printf("r6-tests: %d check(s) failed\n", failures);
    return 1;
}
