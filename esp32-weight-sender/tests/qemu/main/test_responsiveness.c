#include "test_harness.h"
#include "test_diag_channels.h"
#include "test_weight_mqtt.h"
#include "mock_transport.h"
#include "golden_frames.h"
#include "cas_ci2001_parser.h"
#include "scale_reader.h"
#include "scale_events.h"
#include "scale_manager.h"
#include "weight_source.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/task.h"
#include <string.h>

static const scale_protocol_t *protocols[] = {&cas_ci2001_protocol};

void test_responsiveness_run(void)
{
    static mock_transport_t mock;
    mock_transport_init(&mock);
    scale_link_config_t link = {.uart_port=2,.rx_gpio=33,.tx_gpio=32,.baud_rate=9600};
    test_check(scale_transport_open(&mock.base,&link)==ESP_OK,"rate_mock_open");
    QueueHandle_t queue=xQueueCreate(32,sizeof(scale_event_t));
    scale_reader_config_t cfg={.channel_id=2,.transport=&mock.base,.protocols=protocols,
        .num_protocols=1,.event_queue=queue};
    test_check(scale_reader_start(&cfg)==ESP_OK,"rate_reader_start");
    vTaskDelay(pdMS_TO_TICKS(30));
    scale_event_t event;
    while(xQueueReceive(queue,&event,0)==pdTRUE){}
    uint32_t count=0;
    int64_t begin=esp_timer_get_time();
    TickType_t wake=xTaskGetTickCount();
    for(unsigned i=0;i<60;i++){
        mock_transport_push_bytes(&mock,k_golden_frames[0].wire,GOLDEN_WIRE_SIZE);
        vTaskDelayUntil(&wake,pdMS_TO_TICKS(50));
        while(xQueueReceive(queue,&event,0)==pdTRUE)if(event.type==SCALE_EVENT_READING)count++;
    }
    scale_reader_diagnostics_t d;
    scale_reader_get_diagnostics(2,&d);
    test_check(d.frames_received==60&&d.valid_frames==60,"rate_20hz_all_frames_parsed");
    test_check(count==59,"rate_20hz_confirmation_then_every_frame_delivered");
    test_check(d.application_queue_drops==0,"rate_20hz_no_queue_loss");
    test_check(mock.rx_len==0,"rate_20hz_continuous_rx_drain");
    ESP_LOGI("TEST","AUDIT_MEASUREMENT frames=60 duration_us=%lld parsed=%lu readings=%lu parser_us_min=%lu avg=%llu max=%lu",
        (long long)(esp_timer_get_time()-begin),(unsigned long)d.valid_frames,(unsigned long)count,
        (unsigned long)d.parser_min_us,(unsigned long long)(d.parser_total_us/d.parser_calls),(unsigned long)d.parser_max_us);
    for(unsigned i=0;i<60;i++)mock_transport_push_bytes(&mock,k_golden_frames[0].wire,GOLDEN_WIRE_SIZE);
    mock_transport_push_event(&mock,SCALE_XPORT_EV_BUFFER_FULL);
    vTaskDelay(pdMS_TO_TICKS(200));
    scale_reader_get_diagnostics(2,&d);
    /* BUFFER_FULL means the driver dropped bytes after the ones just drained,
     * so the frame in progress is discarded to the next terminator: 60 frames
     * pushed, the one spanning the 1024-byte recovery drain is sacrificed. */
    test_check(d.valid_frames==119,"burst_every_valid_frame_parsed_despite_consumer_backlog");
    test_check(d.resync_events>=1,"burst_buffer_full_resyncs_to_terminator");
    test_check(d.application_queue_drops>0,"burst_queue_overload_counted");
    test_check(d.application_queue_high_water==32,"burst_queue_high_water_measured");
    test_check(mock.flush_calls==0,"buffer_full_recovery_retains_valid_frames");
    while(xQueueReceive(queue,&event,0)==pdTRUE){}
    mock_transport_push_bytes(&mock,k_golden_frames[0].wire,GOLDEN_WIRE_SIZE);
    vTaskDelay(pdMS_TO_TICKS(50));
    bool recovered=false;
    while(xQueueReceive(queue,&event,0)==pdTRUE)if(event.type==SCALE_EVENT_READING)recovered=true;
    test_check(recovered,"burst_queue_recovers_without_uart_sleep");

    /* Start manager only after existing no-manager/no-scale assertions. */
    QueueHandle_t manager=xQueueCreate(10,sizeof(scale_event_t));
    test_check(scale_manager_start(manager)==ESP_OK,"timestamp_manager_start");
    memset(&event,0,sizeof(event));
    event.channel_id=1;event.type=SCALE_EVENT_READING;
    event.reading=(scale_reading_t){.valid=true,.has_gross=true,.gross_is_physical=true,
        .gross=1.234,.has_unit=true,.timestamp_ms=weight_source_now_ms()-2000};
    strcpy(event.reading.unit,"kg");
    xQueueSend(manager,&event,0);
    vTaskDelay(pdMS_TO_TICKS(30));
    scale_channel_status_t status;
    scale_manager_get_channel_status(1,&status);
    test_check(status.gross_update_ms==event.reading.timestamp_ms,"timestamp_acquisition_survives_manager_queue");
    weight_sample_t sample;
    test_check(weight_source_get(&sample)==WEIGHT_SOURCE_RESULT_REAL&&sample.age_ms>=2000,"timestamp_source_keeps_true_age");
    uint32_t seq=sample.sequence,stamp=weight_source_last_valid_cas_ms();
    weight_source_get(&sample);
    test_check(sample.sequence==seq,"repeated_source_poll_does_not_advance_cas_sequence");
    test_check(weight_source_last_valid_cas_ms()==stamp,"repeated_source_poll_does_not_renew_freshness");
    event.reading.timestamp_ms=weight_source_now_ms();
    xQueueSend(manager,&event,0);vTaskDelay(pdMS_TO_TICKS(30));
    weight_source_get(&sample);
    test_check(sample.sequence==seq+1,"new_identical_weight_frame_advances_cas_sequence");
    event.reading.timestamp_ms=weight_source_now_ms()-4000;
    xQueueSend(manager,&event,0);vTaskDelay(pdMS_TO_TICKS(30));
    test_check(weight_source_get(&sample)==WEIGHT_SOURCE_RESULT_NONE,"queued_stale_weight_never_becomes_fresh_or_zero");

    /* Per-channel diagnostics, CH1 selection and cross-channel isolation. */
    test_diag_channels_run(manager);
    test_weight_mqtt_run(manager);
}
