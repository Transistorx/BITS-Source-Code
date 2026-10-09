#include "dual_dispense_controller.h"
#include "weight_receiver.h"
#include "safety_manager.h"
#include "job_queue.h"
#include "telemetry_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

extern void audit_check(bool condition, const char *name);
static atomic_bool finished;

static void weight_writer(void *arg)
{
    (void)arg;
    for(unsigned i=1;i<=100;i++){
        weight_msg_t msg={.weight_g=(int32_t)(1000+i),.has_weight1=true,.weight1_g=(int32_t)(1000+i)};
        dual_dispense_controller_on_weight(&msg,5000+i*10);
        vTaskDelay(1);
    }
    atomic_store(&finished,true);
    vTaskDelete(NULL);
}

static void no_actuation(const weight_msg_t *msg, uint32_t now){(void)msg;(void)now;}

void test_concurrency_run(void)
{
    audit_check(telemetry_client_note_remote_cancel(123),"cancel_before_job_records_unique_remote_id");
    audit_check(telemetry_client_remote_job_cancelled(123),"late_cancelled_job_is_refused");
    audit_check(!telemetry_client_remote_job_cancelled(124),"remote_cancel_does_not_cancel_sibling_job");
    job_queue_init();safety_manager_init();dual_dispense_controller_init();
    atomic_store(&finished,false);
    bool consistent=true;
    xTaskCreate(weight_writer,"audit_writer",2048,NULL,4,NULL);
    while(!atomic_load(&finished)){
        dual_channel_snapshot_t snapshot;
        dual_dispense_controller_snapshot(1,50000,&snapshot);
        if(snapshot.have_weight&&snapshot.weight_age_ms!=50000-(5000+(uint32_t)(snapshot.current_weight_g-1000)*10))
            consistent=false;
        vTaskDelay(1);
    }
    audit_check(consistent,"concurrent_snapshot_keeps_weight_and_acquisition_atomic");
    weight_msg_t parsed;
    const char *good="{\"type\":\"weight\",\"weight_g\":1234,\"sequence\":20,\"age_ms\":250}";
    audit_check(weight_msg_parse(good,strlen(good),&parsed)==WEIGHT_MSG_ACCEPTED&&parsed.age_ms==250,
        "sender_acquisition_age_preserved");
    const char *bad="{\"type\":\"weight\",\"weight_g\":1234,\"age_ms\":6000}";
    audit_check(weight_msg_parse(bad,strlen(bad),&parsed)==WEIGHT_MSG_REJECTED,"stale_sender_payload_rejected");
    bad="{\"type\":\"weight\",\"weight_g\":1234,\"age_ms\":1.5}";
    audit_check(weight_msg_parse(bad,strlen(bad),&parsed)==WEIGHT_MSG_REJECTED,"fractional_sender_age_rejected");
    weight_receiver_init();weight_receiver_set_actuation_handler(no_actuation);
    weight_receiver_on_message(good,strlen(good),1000);
    audit_check(weight_receiver_last_rx_ms()==750,"control_freshness_tracks_acquisition_not_receive");
    weight_receiver_on_message(good,strlen(good),2000);
    audit_check(weight_receiver_last_rx_ms()==750,"duplicate_weight_cannot_renew_freshness");
    bad="{\"type\":\"weight\",\"weight_g\":500,\"sequence\":19}";
    weight_receiver_on_message(bad,strlen(bad),2100);
    audit_check(weight_receiver_last_rx_ms()==750,"older_weight_cannot_replace_current_measurement");
    weight_receiver_on_link_lost(2200);
    bad="{\"type\":\"weight\",\"weight_g\":500,\"sequence\":1}";
    weight_receiver_on_message(bad,strlen(bad),2300);
    audit_check(weight_receiver_last_rx_ms()==2300,"reconnect_allows_new_sender_sequence_epoch");
    job_queue_init();safety_manager_init();dual_dispense_controller_init();
    audit_check(dual_dispense_controller_manual_relay(1,true,0)==ESP_OK,"manual_pump_accepts_uptime_zero_start");
    dual_dispense_controller_tick(3000000);
    dual_channel_snapshot_t snapshot;
    dual_dispense_controller_snapshot(1,3000000,&snapshot);
    audit_check(!snapshot.relay_on,"manual_uptime_zero_start_still_times_out");
    safety_manager_init();
    safety_manager_note_control_tick(5000);
    safety_manager_tick(8101);
    audit_check(safety_manager_fault()==SAFETY_CONTROL_FAILURE,"independent_watchdog_detects_missing_control_heartbeat");
    safety_manager_raise(SAFETY_EMERGENCY_STOP,8200);
    safety_manager_raise(SAFETY_WEIGHT_STALE,8300);
    audit_check(safety_manager_fault()==SAFETY_EMERGENCY_STOP,"concurrent_safety_policy_cannot_downgrade_estop");
}

