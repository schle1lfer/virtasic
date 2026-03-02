/**
 * @file vlan_api.c
 * @brief VLAN lifecycle management via the Linux Netlink ROUTE API (libnl-route-3).
 *
 * Each VLAN is represented in the kernel as a bridge-type interface named
 * "Vlan<id>" (e.g. Vlan100 for VLAN ID 100).  Assigning an interface to a
 * VLAN enslaves it to that bridge via IFLA_MASTER; removing the assignment
 * un-enslaves it.  All existence checks use ioctl(SIOCGIFINDEX) so they work
 * without an Netlink cache, avoiding the rtnl_link_alloc_cache overhead on
 * every call.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <linux/if.h>
#include <netlink/socket.h>
#include <netlink/netlink.h>
#include <netlink/route/link.h>

#include "vlan_api.h"

/** Prefix for VLAN bridge interface names: Vlan<id> (e.g. Vlan100). */
#define VLAN_IFACE_PREFIX "Vlan"

/* ---------------------------------------------------------------------------
 * Internal helpers
 * --------------------------------------------------------------------------- */

/**
 * get_iface_index() - Resolve a named interface to its kernel ifindex.
 *
 * Uses ioctl(SIOCGIFINDEX) rather than Netlink cache lookup so that it works
 * even in environments where NETLINK_ROUTE queries are restricted.  Avoids
 * the net/if.h vs linux/if.h include-conflict by using the ioctl directly.
 *
 * @param name  Null-terminated interface name (e.g. "eth0", "Vlan100").
 * @return      Interface index (>= 1) on success, or -1 if the interface does
 *              not exist or an error occurred.
 */
static int get_iface_index(const char *name)
{
    struct ifreq ifr;
    int fd;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;

    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, name, IFNAMSIZ - 1);

    if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0)
    {
        close(fd);
        return -1;
    }

    close(fd);
    return ifr.ifr_ifindex;
}

/**
 * check_iface_exists() - Test whether a named network interface is present.
 *
 * @param name  Interface name to check.
 * @return      1 if the interface exists in the current network namespace,
 *              0 otherwise.
 */
static int check_iface_exists(const char *name)
{
    return get_iface_index(name) >= 0;
}

/**
 * vlan_bridge_name() - Build the canonical bridge name for a given VLAN ID.
 *
 * @param vlan_id  VLAN identifier (1-4094).
 * @param buf      Output buffer to receive the name string.
 * @param bufsz    Size of @p buf in bytes (should be at least IFNAMSIZ).
 */
static void vlan_bridge_name(uint16_t vlan_id, char *buf, size_t bufsz)
{
    snprintf(buf, bufsz, "%s%u", VLAN_IFACE_PREFIX, (unsigned)vlan_id);
}

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */

/**
 * create_vlan() - Create a new VLAN in the system.
 *
 * @details
 *   Represents the VLAN as a Linux bridge-type network interface named
 *   "Vlan<vlan_id>" (e.g. Vlan100).  The function first checks whether that
 *   interface already exists (ioctl-based, no Netlink cache required) and
 *   returns @c -EEXIST without touching the kernel if it does.  Otherwise an
 *   RTM_NEWLINK message with IFLA_INFO_KIND="bridge" is sent to create the
 *   interface.
 *
 * @param vlan_id  802.1Q VLAN identifier.  Valid range: 1–4094.
 *
 * @return
 *    0        – success; the bridge interface "Vlan<vlan_id>" now exists. \n
 *   -EINVAL   – @p vlan_id is outside the valid range [1..4094]. \n
 *   -EEXIST   – A VLAN with this ID already exists. \n
 *   -ENOMEM   – Failed to allocate a Netlink socket or link object. \n
 *   -EIO      – Netlink connect or RTM_NEWLINK failed (kernel error).
 */
int create_vlan(uint16_t vlan_id)
{
    char vlan_name[IFNAMSIZ];
    struct nl_sock *sock = NULL;
    struct rtnl_link *link = NULL;
    int err;
    int _nl_err;

    if (vlan_id < 1 || vlan_id > 4094)
    {
        fprintf(stderr, "create_vlan: VLAN ID %u is outside valid range [1..4094]\n",
                (unsigned)vlan_id);
        return -EINVAL;
    }

    vlan_bridge_name(vlan_id, vlan_name, sizeof(vlan_name));

    if (check_iface_exists(vlan_name))
    {
        fprintf(stderr, "create_vlan: VLAN %u (%s) already exists\n",
                (unsigned)vlan_id, vlan_name);
        return -EEXIST;
    }

    NL_CALL_RET(sock, nl_socket_alloc(),
                "nl_socket_alloc", "");
    if (!sock)
    {
        fprintf(stderr, "create_vlan: failed to allocate Netlink socket\n");
        return -ENOMEM;
    }

    NL_CALL_RET(_nl_err, nl_connect(sock, NETLINK_ROUTE),
                "nl_connect", "sock=%p, protocol=NETLINK_ROUTE", (void *)sock);
    if (_nl_err < 0)
    {
        fprintf(stderr, "create_vlan: nl_connect failed: %s\n", nl_geterror(_nl_err));
        NL_CALL_VOID(nl_socket_free(sock),
                     "nl_socket_free", "sock=%p", (void *)sock);
        return -EIO;
    }

    NL_CALL_RET(link, rtnl_link_alloc(),
                "rtnl_link_alloc", "");
    if (!link)
    {
        fprintf(stderr, "create_vlan: failed to allocate link object\n");
        NL_CALL_VOID(nl_socket_free(sock),
                     "nl_socket_free", "sock=%p", (void *)sock);
        return -ENOMEM;
    }

    NL_CALL_VOID(rtnl_link_set_name(link, vlan_name),
                 "rtnl_link_set_name", "link=%p, name=\"%s\"",
                 (void *)link, vlan_name);

    NL_CALL_RET(_nl_err, rtnl_link_set_type(link, "bridge"),
                "rtnl_link_set_type", "link=%p, type=\"bridge\"",
                (void *)link);
    if (_nl_err < 0)
    {
        fprintf(stderr, "create_vlan: rtnl_link_set_type(bridge) failed: %s\n",
                nl_geterror(_nl_err));
        NL_CALL_VOID(rtnl_link_put(link),
                     "rtnl_link_put", "link=%p", (void *)link);
        NL_CALL_VOID(nl_socket_free(sock),
                     "nl_socket_free", "sock=%p", (void *)sock);
        return -EIO;
    }

    NL_CALL_RET(err, rtnl_link_add(sock, link, NLM_F_CREATE | NLM_F_EXCL),
                "rtnl_link_add",
                "sock=%p, link=%p, flags=NLM_F_CREATE|NLM_F_EXCL",
                (void *)sock, (void *)link);
    if (err < 0)
    {
        fprintf(stderr, "create_vlan: RTM_NEWLINK failed for %s: %s\n",
                vlan_name, nl_geterror(err));
        NL_CALL_VOID(rtnl_link_put(link),
                     "rtnl_link_put", "link=%p", (void *)link);
        NL_CALL_VOID(nl_socket_free(sock),
                     "nl_socket_free", "sock=%p", (void *)sock);
        return -EIO;
    }

    printf("VLAN %u created: bridge interface %s\n", (unsigned)vlan_id, vlan_name);

    NL_CALL_VOID(rtnl_link_put(link),
                 "rtnl_link_put", "link=%p", (void *)link);
    NL_CALL_VOID(nl_socket_free(sock),
                 "nl_socket_free", "sock=%p", (void *)sock);
    return 0;
}

/**
 * delete_vlan() - Delete an existing VLAN from the system.
 *
 * @details
 *   Verifies that the bridge interface "Vlan<vlan_id>" exists (ioctl-based
 *   check) and then sends an RTM_DELLINK Netlink message to remove it.  Any
 *   interfaces still enslaved to this bridge are automatically detached by the
 *   kernel before the bridge is destroyed.
 *
 * @param vlan_id  802.1Q VLAN identifier.  Valid range: 1–4094.
 *
 * @return
 *    0        – success; the bridge interface "Vlan<vlan_id>" has been removed. \n
 *   -EINVAL   – @p vlan_id is outside the valid range [1..4094]. \n
 *   -ENOENT   – The VLAN does not exist. \n
 *   -ENOMEM   – Failed to allocate a Netlink socket or link object. \n
 *   -EIO      – Netlink connect or RTM_DELLINK failed (kernel error).
 */
int delete_vlan(uint16_t vlan_id)
{
    char vlan_name[IFNAMSIZ];
    int vlan_ifindex;
    struct nl_sock *sock = NULL;
    struct rtnl_link *link = NULL;
    int err;
    int _nl_err;

    if (vlan_id < 1 || vlan_id > 4094)
    {
        fprintf(stderr, "delete_vlan: VLAN ID %u is outside valid range [1..4094]\n",
                (unsigned)vlan_id);
        return -EINVAL;
    }

    vlan_bridge_name(vlan_id, vlan_name, sizeof(vlan_name));

    vlan_ifindex = get_iface_index(vlan_name);
    if (vlan_ifindex < 0)
    {
        fprintf(stderr, "delete_vlan: VLAN %u (%s) does not exist\n",
                (unsigned)vlan_id, vlan_name);
        return -ENOENT;
    }

    NL_CALL_RET(sock, nl_socket_alloc(),
                "nl_socket_alloc", "");
    if (!sock)
    {
        fprintf(stderr, "delete_vlan: failed to allocate Netlink socket\n");
        return -ENOMEM;
    }

    NL_CALL_RET(_nl_err, nl_connect(sock, NETLINK_ROUTE),
                "nl_connect", "sock=%p, protocol=NETLINK_ROUTE", (void *)sock);
    if (_nl_err < 0)
    {
        fprintf(stderr, "delete_vlan: nl_connect failed: %s\n", nl_geterror(_nl_err));
        NL_CALL_VOID(nl_socket_free(sock),
                     "nl_socket_free", "sock=%p", (void *)sock);
        return -EIO;
    }

    /* Minimal link object – identifies the interface by ifindex only. */
    NL_CALL_RET(link, rtnl_link_alloc(),
                "rtnl_link_alloc", "");
    if (!link)
    {
        fprintf(stderr, "delete_vlan: failed to allocate link object\n");
        NL_CALL_VOID(nl_socket_free(sock),
                     "nl_socket_free", "sock=%p", (void *)sock);
        return -ENOMEM;
    }

    NL_CALL_VOID(rtnl_link_set_ifindex(link, vlan_ifindex),
                 "rtnl_link_set_ifindex", "link=%p, ifindex=%d",
                 (void *)link, vlan_ifindex);

    NL_CALL_RET(err, rtnl_link_delete(sock, link),
                "rtnl_link_delete", "sock=%p, link=%p",
                (void *)sock, (void *)link);
    if (err < 0)
    {
        fprintf(stderr, "delete_vlan: RTM_DELLINK failed for %s: %s\n",
                vlan_name, nl_geterror(err));
        NL_CALL_VOID(rtnl_link_put(link),
                     "rtnl_link_put", "link=%p", (void *)link);
        NL_CALL_VOID(nl_socket_free(sock),
                     "nl_socket_free", "sock=%p", (void *)sock);
        return -EIO;
    }

    printf("VLAN %u deleted: bridge interface %s removed\n",
           (unsigned)vlan_id, vlan_name);

    NL_CALL_VOID(rtnl_link_put(link),
                 "rtnl_link_put", "link=%p", (void *)link);
    NL_CALL_VOID(nl_socket_free(sock),
                 "nl_socket_free", "sock=%p", (void *)sock);
    return 0;
}

/**
 * add_vlan_assignment() - Assign a network interface to a VLAN.
 *
 * @details
 *   Enslaves @p iface to the bridge interface "Vlan<vlan_id>" by setting its
 *   IFLA_MASTER attribute via RTM_SETLINK.  Before sending the Netlink message
 *   the function checks that:
 *     - The VLAN bridge "Vlan<vlan_id>" exists (ioctl-based).
 *     - The interface @p iface exists (ioctl-based).
 *
 *   The IFLA_MASTER update is issued as a delta (RTM_SETLINK) so no link cache
 *   population is required.
 *
 * @param vlan_id  802.1Q VLAN identifier.  Valid range: 1–4094.
 * @param iface    Name of the network interface to add (e.g. "eth0").
 *
 * @return
 *    0        – success; @p iface is now a member of VLAN @p vlan_id. \n
 *   -EINVAL   – @p vlan_id is out of range, or @p iface is NULL. \n
 *   -ENOENT   – The VLAN or the interface does not exist. \n
 *   -ENOMEM   – Failed to allocate a Netlink socket or link object. \n
 *   -EIO      – Netlink connect or RTM_SETLINK failed (kernel error).
 */
int add_vlan_assignment(uint16_t vlan_id, const char *iface)
{
    char vlan_name[IFNAMSIZ];
    int vlan_ifindex;
    int iface_ifindex;
    struct nl_sock *sock = NULL;
    struct rtnl_link *orig   = NULL;
    struct rtnl_link *change = NULL;
    int err;
    int _nl_err;

    if (vlan_id < 1 || vlan_id > 4094)
    {
        fprintf(stderr, "add_vlan_assignment: VLAN ID %u is outside valid range [1..4094]\n",
                (unsigned)vlan_id);
        return -EINVAL;
    }

    if (!iface)
    {
        fprintf(stderr, "add_vlan_assignment: iface is NULL\n");
        return -EINVAL;
    }

    vlan_bridge_name(vlan_id, vlan_name, sizeof(vlan_name));

    vlan_ifindex = get_iface_index(vlan_name);
    if (vlan_ifindex < 0)
    {
        fprintf(stderr, "add_vlan_assignment: VLAN %u (%s) does not exist\n",
                (unsigned)vlan_id, vlan_name);
        return -ENOENT;
    }

    iface_ifindex = get_iface_index(iface);
    if (iface_ifindex < 0)
    {
        fprintf(stderr, "add_vlan_assignment: interface '%s' does not exist\n", iface);
        return -ENOENT;
    }

    NL_CALL_RET(sock, nl_socket_alloc(),
                "nl_socket_alloc", "");
    if (!sock)
    {
        fprintf(stderr, "add_vlan_assignment: failed to allocate Netlink socket\n");
        return -ENOMEM;
    }

    NL_CALL_RET(_nl_err, nl_connect(sock, NETLINK_ROUTE),
                "nl_connect", "sock=%p, protocol=NETLINK_ROUTE", (void *)sock);
    if (_nl_err < 0)
    {
        fprintf(stderr, "add_vlan_assignment: nl_connect failed: %s\n",
                nl_geterror(_nl_err));
        NL_CALL_VOID(nl_socket_free(sock),
                     "nl_socket_free", "sock=%p", (void *)sock);
        return -EIO;
    }

    /* orig – identifies the interface to modify (by ifindex). */
    NL_CALL_RET(orig, rtnl_link_alloc(),
                "rtnl_link_alloc", "");
    if (!orig)
    {
        fprintf(stderr, "add_vlan_assignment: failed to allocate orig link\n");
        NL_CALL_VOID(nl_socket_free(sock),
                     "nl_socket_free", "sock=%p", (void *)sock);
        return -ENOMEM;
    }

    NL_CALL_VOID(rtnl_link_set_ifindex(orig, iface_ifindex),
                 "rtnl_link_set_ifindex", "link=%p, ifindex=%d",
                 (void *)orig, iface_ifindex);

    /* change – the desired delta: set IFLA_MASTER to the bridge's ifindex. */
    NL_CALL_RET(change, rtnl_link_alloc(),
                "rtnl_link_alloc", "");
    if (!change)
    {
        fprintf(stderr, "add_vlan_assignment: failed to allocate change link\n");
        NL_CALL_VOID(rtnl_link_put(orig),
                     "rtnl_link_put", "link=%p", (void *)orig);
        NL_CALL_VOID(nl_socket_free(sock),
                     "nl_socket_free", "sock=%p", (void *)sock);
        return -ENOMEM;
    }

    NL_CALL_VOID(rtnl_link_set_master(change, vlan_ifindex),
                 "rtnl_link_set_master", "link=%p, master_ifindex=%d",
                 (void *)change, vlan_ifindex);

    NL_CALL_RET(err, rtnl_link_change(sock, orig, change, 0),
                "rtnl_link_change",
                "sock=%p, orig=%p, change=%p, flags=0",
                (void *)sock, (void *)orig, (void *)change);
    if (err < 0)
    {
        fprintf(stderr, "add_vlan_assignment: RTM_SETLINK failed (%s -> %s): %s\n",
                iface, vlan_name, nl_geterror(err));
        NL_CALL_VOID(rtnl_link_put(change),
                     "rtnl_link_put", "link=%p", (void *)change);
        NL_CALL_VOID(rtnl_link_put(orig),
                     "rtnl_link_put", "link=%p", (void *)orig);
        NL_CALL_VOID(nl_socket_free(sock),
                     "nl_socket_free", "sock=%p", (void *)sock);
        return -EIO;
    }

    printf("Interface %s assigned to VLAN %u (%s)\n",
           iface, (unsigned)vlan_id, vlan_name);

    NL_CALL_VOID(rtnl_link_put(change),
                 "rtnl_link_put", "link=%p", (void *)change);
    NL_CALL_VOID(rtnl_link_put(orig),
                 "rtnl_link_put", "link=%p", (void *)orig);
    NL_CALL_VOID(nl_socket_free(sock),
                 "nl_socket_free", "sock=%p", (void *)sock);
    return 0;
}

/**
 * remove_vlan_assignment() - Remove a network interface from a VLAN.
 *
 * @details
 *   Un-enslaves @p iface from the bridge that represents VLAN @p vlan_id by
 *   clearing its IFLA_MASTER attribute (setting it to 0) via RTM_SETLINK.
 *   Before sending the Netlink message the function checks that:
 *     - The VLAN bridge "Vlan<vlan_id>" exists (ioctl-based).
 *     - The interface @p iface exists (ioctl-based).
 *
 * @param vlan_id  802.1Q VLAN identifier.  Valid range: 1–4094.
 * @param iface    Name of the network interface to remove (e.g. "eth0").
 *
 * @return
 *    0        – success; @p iface is no longer a member of VLAN @p vlan_id. \n
 *   -EINVAL   – @p vlan_id is out of range, or @p iface is NULL. \n
 *   -ENOENT   – The VLAN or the interface does not exist. \n
 *   -ENOMEM   – Failed to allocate a Netlink socket or link object. \n
 *   -EIO      – Netlink connect or RTM_SETLINK failed (kernel error).
 */
int remove_vlan_assignment(uint16_t vlan_id, const char *iface)
{
    char vlan_name[IFNAMSIZ];
    int iface_ifindex;
    struct nl_sock *sock = NULL;
    struct rtnl_link *orig   = NULL;
    struct rtnl_link *change = NULL;
    int err;
    int _nl_err;

    if (vlan_id < 1 || vlan_id > 4094)
    {
        fprintf(stderr, "remove_vlan_assignment: VLAN ID %u is outside valid range [1..4094]\n",
                (unsigned)vlan_id);
        return -EINVAL;
    }

    if (!iface)
    {
        fprintf(stderr, "remove_vlan_assignment: iface is NULL\n");
        return -EINVAL;
    }

    vlan_bridge_name(vlan_id, vlan_name, sizeof(vlan_name));

    if (!check_iface_exists(vlan_name))
    {
        fprintf(stderr, "remove_vlan_assignment: VLAN %u (%s) does not exist\n",
                (unsigned)vlan_id, vlan_name);
        return -ENOENT;
    }

    iface_ifindex = get_iface_index(iface);
    if (iface_ifindex < 0)
    {
        fprintf(stderr, "remove_vlan_assignment: interface '%s' does not exist\n", iface);
        return -ENOENT;
    }

    NL_CALL_RET(sock, nl_socket_alloc(),
                "nl_socket_alloc", "");
    if (!sock)
    {
        fprintf(stderr, "remove_vlan_assignment: failed to allocate Netlink socket\n");
        return -ENOMEM;
    }

    NL_CALL_RET(_nl_err, nl_connect(sock, NETLINK_ROUTE),
                "nl_connect", "sock=%p, protocol=NETLINK_ROUTE", (void *)sock);
    if (_nl_err < 0)
    {
        fprintf(stderr, "remove_vlan_assignment: nl_connect failed: %s\n",
                nl_geterror(_nl_err));
        NL_CALL_VOID(nl_socket_free(sock),
                     "nl_socket_free", "sock=%p", (void *)sock);
        return -EIO;
    }

    /* orig – identifies the interface to modify (by ifindex). */
    NL_CALL_RET(orig, rtnl_link_alloc(),
                "rtnl_link_alloc", "");
    if (!orig)
    {
        fprintf(stderr, "remove_vlan_assignment: failed to allocate orig link\n");
        NL_CALL_VOID(nl_socket_free(sock),
                     "nl_socket_free", "sock=%p", (void *)sock);
        return -ENOMEM;
    }

    NL_CALL_VOID(rtnl_link_set_ifindex(orig, iface_ifindex),
                 "rtnl_link_set_ifindex", "link=%p, ifindex=%d",
                 (void *)orig, iface_ifindex);

    /* change – the desired delta: IFLA_MASTER = 0 un-enslaves the interface. */
    NL_CALL_RET(change, rtnl_link_alloc(),
                "rtnl_link_alloc", "");
    if (!change)
    {
        fprintf(stderr, "remove_vlan_assignment: failed to allocate change link\n");
        NL_CALL_VOID(rtnl_link_put(orig),
                     "rtnl_link_put", "link=%p", (void *)orig);
        NL_CALL_VOID(nl_socket_free(sock),
                     "nl_socket_free", "sock=%p", (void *)sock);
        return -ENOMEM;
    }

    NL_CALL_VOID(rtnl_link_set_master(change, 0),
                 "rtnl_link_set_master", "link=%p, master_ifindex=0 (unset)",
                 (void *)change);

    NL_CALL_RET(err, rtnl_link_change(sock, orig, change, 0),
                "rtnl_link_change",
                "sock=%p, orig=%p, change=%p, flags=0",
                (void *)sock, (void *)orig, (void *)change);
    if (err < 0)
    {
        fprintf(stderr, "remove_vlan_assignment: RTM_SETLINK failed for %s: %s\n",
                iface, nl_geterror(err));
        NL_CALL_VOID(rtnl_link_put(change),
                     "rtnl_link_put", "link=%p", (void *)change);
        NL_CALL_VOID(rtnl_link_put(orig),
                     "rtnl_link_put", "link=%p", (void *)orig);
        NL_CALL_VOID(nl_socket_free(sock),
                     "nl_socket_free", "sock=%p", (void *)sock);
        return -EIO;
    }

    printf("Interface %s removed from VLAN %u (%s)\n",
           iface, (unsigned)vlan_id, vlan_name);

    NL_CALL_VOID(rtnl_link_put(change),
                 "rtnl_link_put", "link=%p", (void *)change);
    NL_CALL_VOID(rtnl_link_put(orig),
                 "rtnl_link_put", "link=%p", (void *)orig);
    NL_CALL_VOID(nl_socket_free(sock),
                 "nl_socket_free", "sock=%p", (void *)sock);
    return 0;
}
