#pragma once
#include <stdbool.h>
#include <stdint.h>
struct ble_npl_event { void (*fn)(struct ble_npl_event *); void *arg; bool queued; };
struct ble_npl_eventq { struct ble_npl_event *items[32]; unsigned count; };
struct ble_npl_callout { struct ble_npl_event event; bool active; uint32_t ticks; };
struct ble_npl_eventq *nimble_port_get_dflt_eventq(void);
void ble_npl_event_init(struct ble_npl_event *, void (*)(struct ble_npl_event *), void *);
void ble_npl_eventq_put(struct ble_npl_eventq *, struct ble_npl_event *);
int ble_npl_callout_init(struct ble_npl_callout *, struct ble_npl_eventq *, void (*)(struct ble_npl_event *), void *);
int ble_npl_callout_reset(struct ble_npl_callout *, uint32_t);
void ble_npl_callout_stop(struct ble_npl_callout *);
void ble_npl_callout_deinit(struct ble_npl_callout *);
void ble_npl_event_deinit(struct ble_npl_event *);
static inline uint32_t ble_npl_time_ms_to_ticks32(uint32_t ms) { return ms; }
void nimble_test_drain(void);
