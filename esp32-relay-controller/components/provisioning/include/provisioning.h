#ifndef PROVISIONING_H
#define PROVISIONING_H

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

/* --------------------------------------------------------------------------
 * provisioning_build_ap_name
 *
 * Builds the provisioning SoftAP name "<prefix>-XXXX", where XXXX is the last
 * four hexadecimal characters of the unique device id, upper-cased (e.g.
 * device_id "bits-a4cf12ab34cd", prefix "BITS-Relay" -> "BITS-Relay-34CD").
 *
 * PURE, no hardware and no state, so the QEMU suite can pin the derivation.
 *
 * The suffix is scanned from the END of the id so it stays stable if the
 * "bits-" style prefix ever changes length. When the id carries fewer than
 * four hex characters the suffix is left-padded with '0'; a NULL/empty id
 * yields "0000" rather than an empty AP name (an unnamed SoftAP is unusable
 * and would be indistinguishable from another device's).
 *
 * @param prefix    Fixed role prefix, e.g. "BITS-Scale" / "BITS-Relay"
 * @param device_id Persisted unique id (NVS_KEY_DEVICE_ID); may be NULL
 * @param out       Destination buffer
 * @param cap       Destination capacity; always NUL-terminated
 * -------------------------------------------------------------------------- */
void provisioning_build_ap_name(const char *prefix, const char *device_id,
                                char *out, size_t cap);

/* --------------------------------------------------------------------------
 * provisioning_run
 *
 * Starts a WiFi SoftAP, a DNS captive-portal redirect server, and an HTTP
 * credential form. Blocks until the user submits credentials or the timeout
 * expires, then calls esp_restart() in both cases.
 *
 * This function does NOT return under normal operation.
 * It only returns (with an error code) if initialisation fails before the
 * servers can start.
 *
 * User flow:
 *   1. Phone/laptop sees open AP named <ap_ssid> (the device_id, e.g.
 *      "bits-a4cf12ab34cd")
 *   2. Connecting triggers the captive-portal popup automatically
 *   3. The page shows the device id + MAC; user picks the SSID from a dropdown
 *      (the stored SSID/password is pre-selected/filled if in range) and submits
 *   4. Credentials written to the primary NVS slot → device restarts → normal boot
 *
 * @param ap_ssid    Name to broadcast for the provisioning SoftAP
 * @param timeout_ms Maximum wait (ms) before restarting without saving
 * -------------------------------------------------------------------------- */
esp_err_t provisioning_run(const char *ap_ssid, uint32_t timeout_ms);

#endif /* PROVISIONING_H */
