/**
 * @file bridge_api.h
 * @brief Public API for bridge and dummy interface lifecycle management
 *        via the Linux Netlink ROUTE interface.
 *
 * A single bridge interface named "Bridge" is created and configured with a
 * jumbo-frame MTU, a caller-supplied MAC address, VLAN filtering enabled, and
 * default VLAN 1 removed from the bridge self.  A dummy interface named
 * "dummy" is created and enslaved to the bridge as its sole member port.
 *
 * Equivalent shell commands performed by bridge_create():
 * @code
 *   ip link add Bridge up type bridge
 *   ip link set Bridge mtu 9100
 *   ip link set Bridge address <mac>
 *   bridge vlan del vid 1 dev Bridge self
 *   ip link add dummy type dummy
 *   ip link set dummy master Bridge
 *   ip link set Bridge type bridge vlan_filtering 1
 * @endcode
 *
 * Equivalent shell commands performed by bridge_remove():
 * @code
 *   ip link del Bridge
 *   ip link del dummy
 * @endcode
 */

#ifndef BRIDGE_API_H
#define BRIDGE_API_H

/**
 * bridge_create() - Create a managed bridge with a dummy member interface.
 *
 * @details
 *   Performs the following sequence of Netlink ROUTE operations in order:
 *   -# Create a bridge interface named @c "Bridge" and bring it up
 *      (RTM_NEWLINK with @c type=bridge and @c IFF_UP).
 *   -# Set the bridge MTU to 9100 bytes (RTM_SETLINK).
 *   -# Set the bridge MAC address to @p mac (RTM_SETLINK).
 *   -# Remove the default VLAN 1 entry from the bridge self
 *      (RTM_DELLINK with @c AF_BRIDGE and @c IFLA_BRIDGE_VLAN_INFO).
 *   -# Create a dummy interface named @c "dummy"
 *      (RTM_NEWLINK with @c type=dummy).
 *   -# Enslave the dummy interface to the bridge
 *      (RTM_SETLINK with @c IFLA_MASTER).
 *   -# Enable VLAN filtering on the bridge
 *      (RTM_NEWLINK with @c IFLA_BR_VLAN_FILTERING=1).
 *
 *   If either @c "Bridge" or @c "dummy" already exists in the network
 *   namespace, the function returns @c -EEXIST without performing any
 *   operations.  If any step fails, subsequent steps are skipped and a
 *   negative error code is returned; the caller should invoke
 *   bridge_remove() to clean up any partially-created state.
 *
 * @param[in] mac  Ethernet MAC address in colon-separated hexadecimal notation
 *                 (e.g. @c "aa:bb:cc:dd:ee:ff").  Must not be @c NULL.
 *
 * @return
 *    0        – All steps completed successfully. \n
 *   -EINVAL   – @p mac is @c NULL or is not a valid MAC address string. \n
 *   -EEXIST   – The @c "Bridge" or @c "dummy" interface already exists. \n
 *   -ENOMEM   – Failed to allocate a Netlink socket, message, or link object. \n
 *   -EIO      – A Netlink operation failed (kernel rejected the request);
 *               a descriptive message is printed to @c stderr.
 */
int bridge_create(const char *mac);

/**
 * bridge_remove() - Delete the Bridge and dummy interfaces.
 *
 * @details
 *   Sends RTM_DELLINK Netlink messages to remove the @c "Bridge" and
 *   @c "dummy" interfaces created by bridge_create().  The function resolves
 *   each interface by name before attempting deletion; if an interface is
 *   absent a warning is printed to @c stderr and the step is skipped without
 *   returning an error.  The bridge is deleted first: the kernel automatically
 *   detaches enslaved ports when the bridge master is removed, so the dummy
 *   interface reverts to a standalone interface before being deleted.
 *
 * @return
 *    0        – Both interfaces were deleted (or were already absent). \n
 *   -ENOMEM   – Failed to allocate a Netlink socket or link object. \n
 *   -EIO      – A Netlink RTM_DELLINK operation failed (kernel error);
 *               a descriptive message is printed to @c stderr.
 */
int bridge_remove(void);

#endif /* BRIDGE_API_H */
