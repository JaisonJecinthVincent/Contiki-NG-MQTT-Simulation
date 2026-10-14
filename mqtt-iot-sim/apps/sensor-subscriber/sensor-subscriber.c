/*
 * Copyright (c) 2026, Contiki-NG MQTT IoT Simulation
 * Sensor Event Subscriber Application (Mote 4)
 */

#include "contiki.h"
#include "net/ipv6/uip.h"
#include "net/ipv6/uip-ds6.h"
#include "net/ipv6/uiplib.h"
#include "net/routing/routing.h"
#include "mqtt.h"
#include "sys/log.h"
#include "sys/etimer.h"
#include "jsonparse.h"
#include "json.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#define LOG_MODULE "Sensor-Sub"
#define LOG_LEVEL LOG_LEVEL_INFO

#define BROKER_IP_ADDR      MQTT_CLIENT_CONF_BROKER_IP_ADDR
#define BROKER_PORT         MQTT_CLIENT_CONF_BROKER_PORT
#define BUFFER_SIZE         256

static const char *sub_topic = "iot-2/evt/+/fmt/json";
static const char *client_id = "d:iot-network:sensor-sub-mote4";

static struct mqtt_connection conn;
static char app_buffer[BUFFER_SIZE];

typedef enum {
  STATE_INIT,
  STATE_WAIT_NET,
  STATE_CONNECTING,
  STATE_CONNECTED,
  STATE_SUBSCRIBED,
  STATE_DISCONNECTED
} app_state_t;

static app_state_t state = STATE_INIT;
static uint16_t sub_mid = 0;

PROCESS(sensor_subscriber_process, "Sensor Subscriber Process");
AUTOSTART_PROCESSES(&sensor_subscriber_process);

/*---------------------------------------------------------------------------*/
static bool
have_connectivity(void)
{
  if(uip_ds6_get_global(ADDR_PREFERRED) == NULL ||
     uip_ds6_defrt_choose() == NULL) {
    return false;
  }
  return true;
}
/*---------------------------------------------------------------------------*/
static bool
validate_json_payload(const char *payload, int len, char *sensor_out, size_t max_len)
{
  struct jsonparse_state js;
  int type;
  bool has_sensor = false;
  bool has_value = false;
  bool has_unit = false;

  jsonparse_setup(&js, payload, len);

  while((type = jsonparse_next(&js)) != 0) {
    if(type == JSON_TYPE_PAIR_NAME) {
      if(jsonparse_strcmp_value(&js, "sensor") == 0) {
        jsonparse_next(&js);
        jsonparse_next(&js);
        jsonparse_copy_value(&js, sensor_out, max_len);
        has_sensor = true;
      } else if(jsonparse_strcmp_value(&js, "value") == 0) {
        jsonparse_next(&js);
        jsonparse_next(&js);
        has_value = true;
      } else if(jsonparse_strcmp_value(&js, "unit") == 0) {
        jsonparse_next(&js);
        jsonparse_next(&js);
        has_unit = true;
      }
    }
  }

  return has_sensor && has_value && has_unit;
}
/*---------------------------------------------------------------------------*/
static void
mqtt_event(struct mqtt_connection *m, mqtt_event_t event, void *data)
{
  switch(event) {
  case MQTT_EVENT_CONNECTED:
    LOG_INFO("MQTT connection established to broker [%s]:%d\n", BROKER_IP_ADDR, BROKER_PORT);
    state = STATE_CONNECTED;
    process_poll(&sensor_subscriber_process);
    break;

  case MQTT_EVENT_DISCONNECTED:
    LOG_WARN("MQTT disconnected from broker. Event: DISCONNECTED\n");
    state = STATE_DISCONNECTED;
    process_poll(&sensor_subscriber_process);
    break;

  case MQTT_EVENT_CONNECTION_REFUSED_ERROR:
    LOG_ERR("MQTT connection refused by broker. Error code: %u\n", *((mqtt_event_t *)data));
    state = STATE_DISCONNECTED;
    process_poll(&sensor_subscriber_process);
    break;

  case MQTT_EVENT_SUBACK: {
    LOG_INFO("Subscription CONFIRMED: Subscribed to wildcard filter '%s'\n", sub_topic);
    state = STATE_SUBSCRIBED;
    break;
  }

  case MQTT_EVENT_PUBLISH: {
    struct mqtt_message *msg = (struct mqtt_message *)data;
    int copy_len;
    char sensor_name[32] = {0};
    bool valid;

    LOG_INFO("--- Received MQTT Event ---\n");
    LOG_INFO("Topic: '%s'\n", msg->topic);

    copy_len = (msg->payload_chunk_length < (BUFFER_SIZE - 1)) ?
                msg->payload_chunk_length : (BUFFER_SIZE - 1);
    memcpy(app_buffer, msg->payload_chunk, copy_len);
    app_buffer[copy_len] = '\0';

    LOG_INFO("Payload: %s\n", app_buffer);

    valid = validate_json_payload(app_buffer, copy_len, sensor_name, sizeof(sensor_name));
    if(valid) {
      LOG_INFO("JSON Validation: PASSED (sensor: '%s')\n", sensor_name);
      if(strstr(msg->topic, "temperature") != NULL) {
        LOG_INFO("Wildcard Verification: TEMPERATURE event matched wildcard 'iot-2/evt/+/fmt/json'\n");
      } else if(strstr(msg->topic, "humidity") != NULL) {
        LOG_INFO("Wildcard Verification: HUMIDITY event matched wildcard 'iot-2/evt/+/fmt/json'\n");
      }
    } else {
      LOG_WARN("JSON Validation: FAILED or unexpected structure\n");
    }
    LOG_INFO("---------------------------\n");
    break;
  }

  default:
    LOG_DBG("MQTT unhandled event: %d\n", event);
    break;
  }
}
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(sensor_subscriber_process, ev, data)
{
  static struct etimer periodic_timer;
  mqtt_status_t status;

  PROCESS_BEGIN();

  LOG_INFO("Mote 4: Sensor-Event Subscriber starting...\n");
  LOG_INFO("Target Broker: [%s]:%d, Subscription Filter: %s\n", BROKER_IP_ADDR, BROKER_PORT, sub_topic);

  mqtt_register(&conn, &sensor_subscriber_process, (char *)client_id,
                mqtt_event, 64);

  state = STATE_WAIT_NET;
  etimer_set(&periodic_timer, CLOCK_SECOND * 2);

  while(1) {
    PROCESS_WAIT_EVENT();

    if(ev == PROCESS_EVENT_TIMER && etimer_expired(&periodic_timer)) {
      switch(state) {
      case STATE_WAIT_NET:
        if(have_connectivity()) {
          LOG_INFO("Network reachable. Connecting to broker [%s]:%d...\n", BROKER_IP_ADDR, BROKER_PORT);
          status = mqtt_connect(&conn, (char *)BROKER_IP_ADDR, BROKER_PORT,
                                60, 1);
          if(status == MQTT_STATUS_OK) {
            state = STATE_CONNECTING;
            LOG_INFO("TCP SYN sent to broker. Waiting for CONNACK...\n");
          } else {
            LOG_ERR("mqtt_connect() failed with error %d\n", status);
          }
        } else {
          LOG_INFO("Waiting for RPL network connectivity...\n");
        }
        etimer_set(&periodic_timer, CLOCK_SECOND * 2);
        break;

      case STATE_CONNECTED:
        LOG_INFO("Subscribing to filter: %s (QoS: 1)...\n", sub_topic);
        status = mqtt_subscribe(&conn, &sub_mid, (char *)sub_topic, MQTT_QOS_LEVEL_1);
        if(status != MQTT_STATUS_OK) {
          LOG_ERR("mqtt_subscribe() failed with code %d\n", status);
        }
        etimer_set(&periodic_timer, CLOCK_SECOND * 5);
        break;

      case STATE_SUBSCRIBED:
        /* Keep connection active */
        etimer_set(&periodic_timer, CLOCK_SECOND * 10);
        break;

      case STATE_DISCONNECTED:
        LOG_WARN("Reconnecting to broker in 5 seconds...\n");
        state = STATE_WAIT_NET;
        etimer_set(&periodic_timer, CLOCK_SECOND * 5);
        break;

      default:
        etimer_set(&periodic_timer, CLOCK_SECOND * 5);
        break;
      }
    } else if(ev == PROCESS_EVENT_POLL) {
      if(state == STATE_CONNECTED) {
        etimer_set(&periodic_timer, CLOCK_SECOND * 1);
      } else if(state == STATE_DISCONNECTED) {
        etimer_set(&periodic_timer, CLOCK_SECOND * 5);
      }
    }
  }

  PROCESS_END();
}
