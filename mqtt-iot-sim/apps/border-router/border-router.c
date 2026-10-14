/*
 * Copyright (c) 2026, Contiki-NG MQTT IoT Simulation
 * Border Router Application (Mote 1)
 */

#include "contiki.h"
#include "net/ipv6/uip.h"
#include "net/ipv6/uip-ds6.h"
#include "net/routing/routing.h"
#include "sys/log.h"

#define LOG_MODULE "Border-Router"
#define LOG_LEVEL LOG_LEVEL_INFO

PROCESS(border_router_monitor_process, "Border Router Monitor");
AUTOSTART_PROCESSES(&border_router_monitor_process);

static void
print_network_status(void)
{
  int i;
  uint8_t state;
  uip_ds6_addr_t *lladdr = uip_ds6_get_link_local(-1);

  LOG_INFO("--- Border Router Network Status ---\n");
  if(lladdr != NULL) {
    LOG_INFO("Link-Local IPv6: ");
    LOG_INFO_6ADDR(&lladdr->ipaddr);
    LOG_INFO_("\n");
  }

  LOG_INFO("Global IPv6 Addresses:\n");
  for(i = 0; i < UIP_DS6_ADDR_NB; i++) {
    state = uip_ds6_if.addr_list[i].state;
    if(uip_ds6_if.addr_list[i].isused &&
       (state == ADDR_TENTATIVE || state == ADDR_PREFERRED)) {
      LOG_INFO("  - ");
      LOG_INFO_6ADDR(&uip_ds6_if.addr_list[i].ipaddr);
      LOG_INFO_(" (state: %s)\n", (state == ADDR_PREFERRED) ? "PREFERRED" : "TENTATIVE");
    }
  }

  LOG_INFO("RPL Root: %s\n", NETSTACK_ROUTING.node_is_root() ? "ACTIVE" : "STANDBY");
  LOG_INFO("------------------------------------\n");
}

PROCESS_THREAD(border_router_monitor_process, ev, data)
{
  static struct etimer status_timer;

  PROCESS_BEGIN();

  LOG_INFO("Mote 1: Border Router initializing...\n");
  LOG_INFO("Starting RPL DAG Root & SLIP border-router service...\n");

  /* Print status after startup delay and periodically thereafter */
  etimer_set(&status_timer, CLOCK_SECOND * 5);

  while(1) {
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&status_timer));
    print_network_status();
    etimer_set(&status_timer, CLOCK_SECOND * 30);
  }

  PROCESS_END();
}
