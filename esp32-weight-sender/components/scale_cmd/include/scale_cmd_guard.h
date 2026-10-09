#ifndef SCALE_CMD_GUARD_H
#define SCALE_CMD_GUARD_H

/*
 * Production build guard. SCALE_CMD_TEST_HOOKS exposes a seam that lets the
 * post-encode ZERO/TARE path run with a fake encoder; it must never exist in
 * firmware that drives a real scale. Only the QEMU test project (which also
 * defines BITS_QEMU_TEST_PROJECT, see tests/qemu/CMakeLists.txt) may use it.
 * Dependency-free on purpose so tools/check_prod_guard.ps1 can preprocess it.
 */
#if defined(SCALE_CMD_TEST_HOOKS) && !defined(BITS_QEMU_TEST_PROJECT)
#error "SCALE_CMD_TEST_HOOKS is only allowed in the QEMU test project (tests/qemu)"
#endif

#endif /* SCALE_CMD_GUARD_H */
