#ifndef TEST_BOARD_PINMAP_H
#define TEST_BOARD_PINMAP_H

/*
 * Board pin-map contract tests.
 *
 * Lock the one authoritative scale pin mapping: UART ownership, pad reuse,
 * console-UART protection, EMAC collisions, the MAX13487E AutoDirection
 * contract (no ESP32 DE/RE pad), and the GPIO15 strap-pad / single-scale
 * channel selection.
 */

#ifdef __cplusplus
extern "C" {
#endif

void test_board_pinmap_run(void);

#ifdef __cplusplus
}
#endif

#endif /* TEST_BOARD_PINMAP_H */
