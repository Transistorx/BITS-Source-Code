cmake_minimum_required(VERSION 3.16)

# Relative on purpose: ESP-IDF's split_paths_by_spaces.py rejects an absolute
# EXTRA_COMPONENT_DIRS value that contains spaces, and this workspace lives
# under "BITS Source Code". IDF resolves this against the project source dir.
set(EXTRA_COMPONENT_DIRS "../../components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# TEST-ONLY seam: exposes scale_cmd_test_allow_post_encode(). Never set in production.
idf_build_set_property(COMPILE_OPTIONS "-DSCALE_CMD_TEST_HOOKS=1" APPEND)
# Marks this as the test project; scale_cmd_guard.h #errors on TEST_HOOKS without it.
idf_build_set_property(COMPILE_OPTIONS "-DBITS_QEMU_TEST_PROJECT=1" APPEND)
project(weight_sender_tests)