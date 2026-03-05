/**
 * @file bridge_netlink.h
 * @brief Raw Linux Netlink API helpers for bridge VLAN and port management.
 *
 * These functions issue RTM_SETLINK messages directly via a raw NETLINK_ROUTE
 * socket — no libnl dependency.  Each one corresponds to a single iproute2 /
 * bridge-utils shell command and performs exactly that kernel operation.
 */

#ifndef BRIDGE_NETLINK_H
#define BRIDGE_NETLINK_H

#include <stdint.h>

/**
 * @brief Add a VLAN to the bridge device itself ("self").
 *
 * Equivalent to: bridge vlan add dev <bridge_name> vid <vlan_id> self
 *
 * @param[in] bridge_name  Name of the bridge interface (e.g. "Bridge").
 * @param[in] vlan_id      802.1Q VLAN identifier to add (1–4094).
 *
 * @return 0 on success, negative errno on failure.
 */
int bridge_vlan_add_self(const char *bridge_name, uint16_t vlan_id);

/**
 * @brief Attach a network interface to a bridge (set its master).
 *
 * Equivalent to: ip link set <interface_name> master <bridge_name>
 *
 * @param[in] interface_name  Name of the interface to enslave (e.g. "eth1").
 * @param[in] bridge_name     Name of the bridge master (e.g. "Bridge").
 *
 * @return 0 on success, negative errno on failure.
 */
int ip_link_set_master(const char *interface_name, const char *bridge_name);

/**
 * @brief Add a VLAN to a bridge member port interface.
 *
 * Equivalent to: bridge vlan add dev <interface_name> vid <vlan_id>
 *
 * @param[in] interface_name  Name of the bridge port (e.g. "eth1").
 * @param[in] vlan_id         802.1Q VLAN identifier to add (1–4094).
 *
 * @return 0 on success, negative errno on failure.
 */
int bridge_vlan_add_port(const char *interface_name, uint16_t vlan_id);

#endif /* BRIDGE_NETLINK_H */
