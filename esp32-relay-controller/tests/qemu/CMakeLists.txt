cmake_minimum_required(VERSION 3.16)

# Relative on purpose: ESP-IDF's split_paths_by_spaces.py rejects an absolute
# EXTRA_COMPONENT_DIRS value that contains spaces, and this workspace lives
# under "BITS Source Code". IDF resolves this against the project source dir.
set(EXTRA_COMPONENT_DIRS "../../components")

# Staged-dispense Kconfig defaults. These normally come from the production
# project's Kconfig; the QEMU test project has none, so we provide them here
# (matching the #ifndef fallbacks in dual_dispense_controller.c).
add_compile_definitions(
    CONFIG_WEIGHT_DEMO_COARSE_THRESHOLD_G=500
    CONFIG_WEIGHT_DEMO_FINE_THRESHOLD_G=100
    CONFIG_WEIGHT_DEMO_MICRO_THRESHOLD_G=30
    CONFIG_WEIGHT_DEMO_COARSE_MIN_ON_MS=80
    CONFIG_WEIGHT_DEMO_FINE_MIN_ON_MS=30
    CONFIG_WEIGHT_DEMO_MICRO_MIN_ON_MS=15
    CONFIG_WEIGHT_DEMO_SETTLE_TIME_MS=1500
    CONFIG_WEIGHT_DEMO_INFLIGHT_COMP_G=15
)

# TEST-ONLY relay seam in dual_dispense_controller.c (PRE-04 write-failure
# injection). Defined here and nowhere in the production project.
add_compile_definitions(BITS_QEMU_TEST_HOOKS=1)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(relay_controller_tests)