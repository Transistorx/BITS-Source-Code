#ifndef WEB_API_H
#define WEB_API_H

#include "esp_err.h"
#include <stdbool.h>

/*
 * Operator surface for the relay controller.
 *
 * The API submits jobs to the QUEUE and issues controller commands — it never
 * touches a relay GPIO. That is the point: every actuation still goes through
 * the dispense state machine and the safety manager, so a request from the web
 * cannot bypass a fault latch or leave a valve open.
 *
 *   GET  /                  control page
 *   GET  /api/status        full state as JSON (poll target)
 *   POST /api/job           target_g=N  -> queue a job
 *   POST /api/cancel        id=N        -> cancel a queued job (all if no id)
 *   POST /api/pause         hold the running job with both valves closed
 *   POST /api/resume        resume from WAIT_FOR_SCALE
 *   POST /api/estop         emergency stop: valves closed, queue cancelled
 *   POST /api/clear         clear a latched fault (releases an e-stop)
 *   POST /api/sim           source=real|sim|auto, sim=static|dynamic|progression
 *                           -> forwarded UPSTREAM to the weight sender
 *
 * GET works as well as POST for every command, so the whole surface is
 * reachable from a browser address bar and from curl without a body.
 */
esp_err_t web_api_start(void);
bool web_api_is_started(void);

#endif /* WEB_API_H */