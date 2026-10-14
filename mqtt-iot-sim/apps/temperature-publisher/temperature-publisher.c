/*
 * Copyright (c) 2026, Contiki-NG MQTT IoT Simulation
 * Temperature Sensor Publisher Application (Mote 2)
 */

#include "contiki.h"
#include "net/ipv6/uip.h"
#include "net/ipv6/uip-ds6.h"
#include "net/ipv6/uiplib.h"
#include "net/routing/routing.h"
#include "mqtt.h"
#include "sys/log.h"
#include "sys/etimer.h"

#include <stdio.h>
#include <string.h>

#define LOG_MODULE "Temp-Publisher"
#define LOG_LEVEL LOG_LEVEL_INFO

/* Configuration defaults */
#define BROKER_IP_ADDR      MQTT_CLIENT_CONF_BROKER_IP_ADDR
#define BROKER_PORT         MQTT_CLIENT_CONF_BROKER_PORT
#define PUBLISH_INTERVAL    MQTT_CLIENT_CONF_PUB_INTERVAL
#define BUFFER_SIZE         128

static const char *pub_topic = "iot-2/evt/temperature/fmt/json";
static const char *client_id = "d:iot-network:temperature-mote2";

static struct mqtt_connection conn;
static char app_buffer[BUFFER_SIZE];

/* State definitions */
typedef enum {
  STATE_INIT,
  STATE_WAIT_NET,
  STATE_CONNECTING,
  STATE_CONNECTED,
  STATE_DISCONNECTED
} app_state_t;

static app_state_t state = STATE_INIT;
static uint16_t last_mid = 0;

/* Simulated temperature generator */
static int current_temp_x10 = 250; /* 25.0 C */
static int temp_step = 5;          /* +0.5 C */

PROCESS(temperature_publisher_process, "Temperature Publisher Process");
AUTOSTART_PROCESSES(&temperature_publisher_process);

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
static void
mqtt_event(struct mqtt_connection *m, mqtt_event_t event, void *data)
{
  switch(event) {
  case MQTT_EVENT_CONNECTED:
    LOG_INFO("MQTT connection established to broker [%s]:%d\n", BROKER_IP_ADDR, BROKER_PORT);
    state = STATE_CONNECTED;
    process_poll(&temperature_publisher_process);
    break;

  case MQTT_EVENT_DISCONNECTED:
    LOG_WARN("MQTT disconnected from broker. Event: DISCONNECTED\n");
    state = STATE_DISCONNECTED;
    process_poll(&temperature_publisher_process);
    break;

  case MQTT_EVENT_CONNECTION_REFUSED_ERROR:
    LOG_ERR("MQTT connection refused by broker. Error code: %u\n", *((mqtt_event_t *)data));
    state = STATE_DISCONNECTED;
    process_poll(&temperature_publisher_process);
    break;

  case MQTT_EVENT_PUBACK:
    LOG_INFO("Broker delivery CONFIRMED: PUBACK received for message ID %u\n", last_mid);
    break;

  default:
    LOG_DBG("MQTT unhandled event: %d\n", event);
    break;
  }
}
/*---------------------------------------------------------------------------*/
static void
generate_reading(void)
{
  current_temp_x10 += temp_step;
  if(current_temp_x10 >= 320) {
    temp_step = -5;
  } else if(current_temp_x10 <= 240) {
    temp_step = 5;
  }
}
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(temperature_publisher_process, ev, data)
{
  static struct etimer periodic_timer;
  mqtt_status_t status;

  PROCESS_BEGIN();

  LOG_INFO("Mote 2: Temperature Publisher starting...\n");
  LOG_INFO("Target Broker: [%s]:%d, Topic: %s\n", BROKER_IP_ADDR, BROKER_PORT, pub_topic);

  /* Register with MQTT engine */
  mqtt_register(&conn, &temperature_publisher_process, (char *)client_id,
                mqtt_event, 64);

  state = STATE_WAIT_NET;

  /* Network polling timer */
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
            LOG_ERR("mqtt_connect() failed immediately with error %d\n", status);
          }
        } else {
          LOG_INFO("Waiting for RPL prefix and default route...\n");
        }
        etimer_set(&periodic_timer, CLOCK_SECOND * 2);
        break;

      case STATE_CONNECTING:
        LOG_INFO("Awaiting broker connection response...\n");
        etimer_set(&periodic_timer, CLOCK_SECOND * 3);
        break;

      case STATE_CONNECTED:
        generate_reading();
        {
          int val_int = current_temp_x10 / 10;
          int val_frac = current_temp_x10 % 10;

          LOG_INFO("Generated deterministic reading: %d.%01d C\n", val_int, val_frac);

          snprintf(app_buffer, sizeof(app_buffer),
                   "{\"sensor\":\"temperature\",\"value\":%d.%01d,\"unit\":\"C\"}",
                   val_int, val_frac);

          LOG_INFO("Publish attempt -> Topic: '%s', Payload: %s\n", pub_topic, app_buffer);

          status = mqtt_publish(&conn, &last_mid, (char *)pub_topic,
                                (uint8_t *)app_buffer, strlen(app_buffer),
                                MQTT_QOS_LEVEL_1, MQTT_RETAIN_OFF);

          if(status == MQTT_STATUS_OK) {
            LOG_INFO("Publish packet queued successfully (MID: %u, QoS: 1). Waiting for PUBACK...\n", last_mid);
          } else {
            LOG_ERR("Publish attempt failed with status code: %d\n", status);
          }
        }
        etimer_set(&periodic_timer, PUBLISH_INTERVAL);
        break;

      case STATE_DISCONNECTED:
        LOG_WARN("Reconnecting to broker in 5 seconds...\n");
        etimer_set(&periodic_timer, CLOCK_SECOND * 5);
        state = STATE_WAIT_NET;
        break;

      default:
        etimer_set(&periodic_timer, CLOCK_SECOND * 5);
        break;
      }
    } else if(ev == PROCESS_EVENT_POLL) {
      if(state == STATE_CONNECTED) {
        /* Kick off the first periodic publish after connection */
        etimer_set(&periodic_timer, CLOCK_SECOND * 1);
      } else if(state == STATE_DISCONNECTED) {
        etimer_set(&periodic_timer, CLOCK_SECOND * 5);
      }
    }
  }

  PROCESS_END();
}
