#include "scale_transport.h"

/*
 * Null-safe convenience wrappers over the transport vtable.
 *
 * These exist so scale_reader never has to check `ops` and `ops->field` on
 * every call, and so a partially configured transport degrades to "no data"
 * rather than dereferencing NULL. Failure is always inert: return 0 bytes,
 * report 0 pending events, ignore flushes. That matches the reader's existing
 * contract — a silent link publishes nothing and the stale-weight failsafe
 * fires; it never invents a weight.
 */

static bool transport_usable(const scale_transport_t *t)
{
    return t != NULL && t->ops != NULL;
}

esp_err_t scale_transport_open(scale_transport_t *t, const scale_link_config_t *cfg)
{
    if (!transport_usable(t) || cfg == NULL || t->ops->open == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return t->ops->open(t, cfg);
}

bool scale_transport_wait_event(scale_transport_t *t, scale_xport_event_t *out,
                                uint32_t timeout_ms)
{
    if (!transport_usable(t) || out == NULL || t->ops->wait_event == NULL) {
        return false;
    }
    return t->ops->wait_event(t, out, timeout_ms);
}

int scale_transport_read(scale_transport_t *t, uint8_t *buf, size_t max_len,
                         uint32_t timeout_ms)
{
    if (!transport_usable(t) || buf == NULL || max_len == 0U ||
        t->ops->read == NULL) {
        return 0;
    }
    return t->ops->read(t, buf, max_len, timeout_ms);
}

esp_err_t scale_transport_flush_input(scale_transport_t *t)
{
    if (!transport_usable(t) || t->ops->flush_input == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return t->ops->flush_input(t);
}

esp_err_t scale_transport_get_buffered_len(scale_transport_t *t, size_t *out_len)
{
    if (out_len == NULL) return ESP_ERR_INVALID_ARG;
    *out_len = 0U;
    if (!transport_usable(t) || t->ops->get_buffered_len == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return t->ops->get_buffered_len(t, out_len);
}

size_t scale_transport_pending_events(scale_transport_t *t)
{
    if (!transport_usable(t) || t->ops->pending_events == NULL) {
        return 0U;
    }
    return t->ops->pending_events(t);
}

void scale_transport_close(scale_transport_t *t)
{
    if (!transport_usable(t) || t->ops->close == NULL) return;
    t->ops->close(t);
}

const char *scale_transport_name(const scale_transport_t *t)
{
    if (!transport_usable(t) || t->ops->name == NULL) return "none";
    return t->ops->name;
}
