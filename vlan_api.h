/**
 * @file vlan_api.h
 * @brief Public API and shared Netlink logging macros for VLAN lifecycle
 *        management via the Linux Netlink ROUTE interface.
 *
 * Functions declared here create, delete, and assign 802.1Q VLANs to network
 * interfaces using the libnl-route-3 library.  Each VLAN is represented in the
 * kernel as a Linux bridge interface named "Vlan<id>" (e.g. Vlan100).
 */

#ifndef VLAN_API_H
#define VLAN_API_H

#include <stdint.h>
#include <stdio.h>
#include <time.h>

/* ---------------------------------------------------------------------------
 * Netlink API call logging helpers
 *
 * NL_CALL_RET(var, call, fn_name, params_fmt, ...)
 *   Executes `call`, assigns the result to `var`, then prints the function
 *   name, formatted parameter description, and elapsed wall-clock time (ms).
 *
 * NL_CALL_VOID(call, fn_name, params_fmt, ...)
 *   Same as NL_CALL_RET but for calls whose return value is not captured.
 * --------------------------------------------------------------------------- */
#define NL_CALL_RET(var, call, fn_name, params_fmt, ...)                    \
    do {                                                                     \
        struct timespec _nl_t0, _nl_t1;                                     \
        clock_gettime(CLOCK_MONOTONIC, &_nl_t0);                            \
        (var) = (call);                                                      \
        clock_gettime(CLOCK_MONOTONIC, &_nl_t1);                            \
        double _nl_ms = (_nl_t1.tv_sec  - _nl_t0.tv_sec)  * 1000.0         \
                      + (_nl_t1.tv_nsec - _nl_t0.tv_nsec) / 1.0e6;         \
        printf("[NETLINK] %s(" params_fmt ") => %.3f ms\n",                 \
               fn_name, ##__VA_ARGS__, _nl_ms);                             \
    } while (0)

#define NL_CALL_VOID(call, fn_name, params_fmt, ...)                        \
    do {                                                                     \
        struct timespec _nl_t0, _nl_t1;                                     \
        clock_gettime(CLOCK_MONOTONIC, &_nl_t0);                            \
        (call);                                                              \
        clock_gettime(CLOCK_MONOTONIC, &_nl_t1);                            \
        double _nl_ms = (_nl_t1.tv_sec  - _nl_t0.tv_sec)  * 1000.0         \
                      + (_nl_t1.tv_nsec - _nl_t0.tv_nsec) / 1.0e6;         \
        printf("[NETLINK] %s(" params_fmt ") => %.3f ms\n",                 \
               fn_name, ##__VA_ARGS__, _nl_ms);                             \
    } while (0)

/* ---------------------------------------------------------------------------
 * VLAN API
 * --------------------------------------------------------------------------- */

int create_vlan(uint16_t vlan_id);
int delete_vlan(uint16_t vlan_id);
int add_vlan_assignment(uint16_t vlan_id, const char *iface);
int remove_vlan_assignment(uint16_t vlan_id, const char *iface);

#endif /* VLAN_API_H */
