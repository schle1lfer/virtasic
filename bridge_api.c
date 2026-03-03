/**
 * @file bridge_api.c
 * @brief Bridge and dummy interface lifecycle management via the Linux
 *        Netlink ROUTE API (libnl-route-3).
 *
 * Implements bridge_create() and bridge_remove() which mirror the following
 * shell commands:
 *
 *   bridge_create(mac):
 *     ip link add Bridge up type bridge
 *     ip link set Bridge mtu 9100
 *     ip link set Bridge address <mac>
 *     bridge vlan del vid 1 dev Bridge self
 *     ip link add dummy type dummy
 *     ip link set dummy master Bridge
 *     ip link set Bridge type bridge vlan_filtering 1
 *
 *   bridge_remove():
 *     ip link del Bridge
 *     ip link del dummy
 *
 * Higher-level link operations (create, enslave, delete) use the libnl-route-3
 * rtnl_link_* API.  Bridge-specific VLAN and vlan_filtering operations require
 * kernel attributes (IFLA_BR_*, IFLA_BRIDGE_VLAN_INFO) that have no high-level
 * libnl wrapper and are sent as raw Netlink messages via nlmsg_alloc / nla_*.
 *
 * All Netlink API calls are instrumented with the NL_CALL_RET / NL_CALL_VOID
 * macros from vlan_api.h which log function name, parameters, and elapsed
 * wall-clock time to stdout.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <linux/if.h>
#include <linux/if_bridge.h>
#include <linux/if_link.h>
#include <netlink/socket.h>
#include <netlink/netlink.h>
#include <netlink/route/link.h>
#include <netlink/addr.h>
#include <netlink/msg.h>
#include <netlink/attr.h>

#include "vlan_api.h"
#include "bridge_api.h"

/** Name of the bridge interface managed by this module. */
#define BRIDGE_IFACE_NAME    "Bridge"

/** Name of the dummy member interface enslaved to the bridge. */
#define DUMMY_IFACE_NAME     "dummy"

/** MTU applied to the bridge interface (jumbo frames). */
#define BRIDGE_MTU           9100U

/** Default VLAN ID removed from the bridge self after creation. */
#define BRIDGE_DEFAULT_VID   1

/*
 * Fallback attribute constants in case the installed kernel headers predate
 * their introduction.  Values match linux/if_link.h and linux/if_bridge.h.
 */
#ifndef IFLA_BR_VLAN_FILTERING
#define IFLA_BR_VLAN_FILTERING  7
#endif

#ifndef IFLA_BRIDGE_FLAGS
#define IFLA_BRIDGE_FLAGS       1
#endif

#ifndef IFLA_BRIDGE_VLAN_INFO
#define IFLA_BRIDGE_VLAN_INFO   3
#endif

#ifndef BRIDGE_FLAGS_SELF
#define BRIDGE_FLAGS_SELF       2
#endif

/* ---------------------------------------------------------------------------
 * Internal helpers
 * --------------------------------------------------------------------------- */

/**
 * get_iface_index() - Resolve a named interface to its kernel ifindex.
 *
 * Uses ioctl(SIOCGIFINDEX) rather than a Netlink cache lookup so that it works
 * even in environments where NETLINK_ROUTE enumeration is restricted.
 *
 * @param name  Null-terminated interface name (e.g. "Bridge", "dummy").
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
 * @return      1 if the interface exists, 0 otherwise.
 */
static int check_iface_exists(const char *name)
{
    return get_iface_index(name) >= 0;
}

/**
 * bridge_do_vlan_del() - Remove a VLAN from the bridge self via raw Netlink.
 *
 * Sends RTM_DELLINK with @c ifi_family=AF_BRIDGE and an @c IFLA_AF_SPEC
 * nesting that contains @c IFLA_BRIDGE_FLAGS=BRIDGE_FLAGS_SELF and
 * @c IFLA_BRIDGE_VLAN_INFO for the given @p vid.  This is the Netlink
 * equivalent of @c "bridge vlan del vid <vid> dev Bridge self".
 *
 * @param sock     Open NETLINK_ROUTE socket.
 * @param ifindex  ifindex of the bridge interface.
 * @param vid      VLAN ID to remove (e.g. 1).
 *
 * @return  0 on success, negative libnl error code on failure.
 */
static int bridge_do_vlan_del(struct nl_sock *sock, int ifindex, uint16_t vid)
{
    struct nl_msg        *msg;
    struct nlmsghdr      *hdr;
    struct ifinfomsg      ifi;
    struct nlattr        *af_spec;
    struct bridge_vlan_info vinfo;
    uint16_t              bridge_flags = BRIDGE_FLAGS_SELF;
    int                   err;

    memset(&ifi, 0, sizeof(ifi));
    ifi.ifi_family = AF_BRIDGE;
    ifi.ifi_index  = ifindex;

    memset(&vinfo, 0, sizeof(vinfo));
    vinfo.flags = 0;
    vinfo.vid   = vid;

    NL_CALL_RET(msg, nlmsg_alloc(), "nlmsg_alloc", "");
    if (!msg)
        return -NLE_NOMEM;

    hdr = nlmsg_put(msg, NL_AUTO_PID, NL_AUTO_SEQ, RTM_DELLINK,
                    sizeof(ifi), NLM_F_REQUEST | NLM_F_ACK);
    if (!hdr)
    {
        nlmsg_free(msg);
        return -NLE_NOMEM;
    }

    memcpy(nlmsg_data(hdr), &ifi, sizeof(ifi));

    af_spec = nla_nest_start(msg, IFLA_AF_SPEC);
    if (!af_spec)
    {
        nlmsg_free(msg);
        return -NLE_NOMEM;
    }

    nla_put(msg, IFLA_BRIDGE_FLAGS, sizeof(bridge_flags), &bridge_flags);
    nla_put(msg, IFLA_BRIDGE_VLAN_INFO, sizeof(vinfo), &vinfo);
    nla_nest_end(msg, af_spec);

    NL_CALL_RET(err, nl_send_auto(sock, msg),
                "nl_send_auto",
                "sock=%p, RTM_DELLINK(AF_BRIDGE, vid=%u)",
                (void *)sock, (unsigned)vid);
    if (err >= 0)
    {
        NL_CALL_RET(err, nl_recvmsgs_default(sock),
                    "nl_recvmsgs_default", "sock=%p", (void *)sock);
    }

    nlmsg_free(msg);
    return err;
}

/**
 * bridge_set_vlan_filtering() - Enable or disable VLAN filtering on a bridge.
 *
 * Sends RTM_NEWLINK with @c IFLA_LINKINFO / @c IFLA_INFO_KIND="bridge" /
 * @c IFLA_INFO_DATA / @c IFLA_BR_VLAN_FILTERING=@p enabled.  Equivalent to
 * @c "ip link set Bridge type bridge vlan_filtering <enabled>".
 *
 * @param sock     Open NETLINK_ROUTE socket.
 * @param ifindex  ifindex of the bridge interface.
 * @param enabled  1 to enable VLAN filtering, 0 to disable.
 *
 * @return  0 on success, negative libnl error code on failure.
 */
static int bridge_set_vlan_filtering(struct nl_sock *sock, int ifindex,
                                     uint8_t enabled)
{
    struct nl_msg    *msg;
    struct nlmsghdr  *hdr;
    struct ifinfomsg  ifi;
    struct nlattr    *linkinfo;
    struct nlattr    *infodata;
    int               err;

    memset(&ifi, 0, sizeof(ifi));
    ifi.ifi_family = AF_UNSPEC;
    ifi.ifi_index  = ifindex;

    NL_CALL_RET(msg, nlmsg_alloc(), "nlmsg_alloc", "");
    if (!msg)
        return -NLE_NOMEM;

    hdr = nlmsg_put(msg, NL_AUTO_PID, NL_AUTO_SEQ, RTM_NEWLINK,
                    sizeof(ifi), NLM_F_REQUEST | NLM_F_ACK);
    if (!hdr)
    {
        nlmsg_free(msg);
        return -NLE_NOMEM;
    }

    memcpy(nlmsg_data(hdr), &ifi, sizeof(ifi));

    linkinfo = nla_nest_start(msg, IFLA_LINKINFO);
    if (!linkinfo)
    {
        nlmsg_free(msg);
        return -NLE_NOMEM;
    }
    nla_put_string(msg, IFLA_INFO_KIND, "bridge");

    infodata = nla_nest_start(msg, IFLA_INFO_DATA);
    if (!infodata)
    {
        nlmsg_free(msg);
        return -NLE_NOMEM;
    }
    nla_put_u8(msg, IFLA_BR_VLAN_FILTERING, enabled);

    nla_nest_end(msg, infodata);
    nla_nest_end(msg, linkinfo);

    NL_CALL_RET(err, nl_send_auto(sock, msg),
                "nl_send_auto",
                "sock=%p, RTM_NEWLINK(IFLA_BR_VLAN_FILTERING=%u)",
                (void *)sock, (unsigned)enabled);
    if (err >= 0)
    {
        NL_CALL_RET(err, nl_recvmsgs_default(sock),
                    "nl_recvmsgs_default", "sock=%p", (void *)sock);
    }

    nlmsg_free(msg);
    return err;
}

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */

int bridge_create(const char *mac)
{
    struct nl_sock  *sock    = NULL;
    struct rtnl_link *link   = NULL;
    struct rtnl_link *orig   = NULL;
    struct rtnl_link *change = NULL;
    struct nl_addr   *addr   = NULL;
    int               bridge_ifindex;
    int               dummy_ifindex;
    int               err;
    int               _nl_err;

    /* -- Input validation --------------------------------------------------- */

    if (!mac)
    {
        fprintf(stderr, "bridge_create: mac is NULL\n");
        return -EINVAL;
    }

    if (check_iface_exists(BRIDGE_IFACE_NAME))
    {
        fprintf(stderr, "bridge_create: interface '%s' already exists\n",
                BRIDGE_IFACE_NAME);
        return -EEXIST;
    }

    if (check_iface_exists(DUMMY_IFACE_NAME))
    {
        fprintf(stderr, "bridge_create: interface '%s' already exists\n",
                DUMMY_IFACE_NAME);
        return -EEXIST;
    }

    /* Validate and parse the MAC address string before touching the kernel. */
    NL_CALL_RET(_nl_err, nl_addr_parse(mac, AF_LLC, &addr),
                "nl_addr_parse", "mac=\"%s\", family=AF_LLC", mac);
    if (_nl_err < 0)
    {
        fprintf(stderr, "bridge_create: invalid MAC address '%s': %s\n",
                mac, nl_geterror(_nl_err));
        return -EINVAL;
    }

    /* -- Netlink socket ----------------------------------------------------- */

    NL_CALL_RET(sock, nl_socket_alloc(), "nl_socket_alloc", "");
    if (!sock)
    {
        fprintf(stderr, "bridge_create: failed to allocate Netlink socket\n");
        nl_addr_put(addr);
        return -ENOMEM;
    }

    NL_CALL_RET(_nl_err, nl_connect(sock, NETLINK_ROUTE),
                "nl_connect", "sock=%p, protocol=NETLINK_ROUTE", (void *)sock);
    if (_nl_err < 0)
    {
        fprintf(stderr, "bridge_create: nl_connect failed: %s\n",
                nl_geterror(_nl_err));
        nl_addr_put(addr);
        NL_CALL_VOID(nl_socket_free(sock),
                     "nl_socket_free", "sock=%p", (void *)sock);
        return -EIO;
    }

    /* -- Step 1: ip link add Bridge up type bridge -------------------------- */

    NL_CALL_RET(link, rtnl_link_alloc(), "rtnl_link_alloc", "");
    if (!link)
    {
        fprintf(stderr, "bridge_create: failed to allocate link for Bridge\n");
        nl_addr_put(addr);
        NL_CALL_VOID(nl_socket_free(sock),
                     "nl_socket_free", "sock=%p", (void *)sock);
        return -ENOMEM;
    }

    NL_CALL_VOID(rtnl_link_set_name(link, BRIDGE_IFACE_NAME),
                 "rtnl_link_set_name", "link=%p, name=\"%s\"",
                 (void *)link, BRIDGE_IFACE_NAME);

    NL_CALL_RET(_nl_err, rtnl_link_set_type(link, "bridge"),
                "rtnl_link_set_type", "link=%p, type=\"bridge\"", (void *)link);
    if (_nl_err < 0)
    {
        fprintf(stderr, "bridge_create: rtnl_link_set_type(bridge) failed: %s\n",
                nl_geterror(_nl_err));
        NL_CALL_VOID(rtnl_link_put(link), "rtnl_link_put", "link=%p", (void *)link);
        nl_addr_put(addr);
        NL_CALL_VOID(nl_socket_free(sock),
                     "nl_socket_free", "sock=%p", (void *)sock);
        return -EIO;
    }

    NL_CALL_VOID(rtnl_link_set_flags(link, IFF_UP),
                 "rtnl_link_set_flags", "link=%p, flags=IFF_UP", (void *)link);

    NL_CALL_RET(err, rtnl_link_add(sock, link, NLM_F_CREATE | NLM_F_EXCL),
                "rtnl_link_add",
                "sock=%p, link=%p, flags=NLM_F_CREATE|NLM_F_EXCL",
                (void *)sock, (void *)link);
    NL_CALL_VOID(rtnl_link_put(link), "rtnl_link_put", "link=%p", (void *)link);
    link = NULL;

    if (err < 0)
    {
        fprintf(stderr, "bridge_create: RTM_NEWLINK for %s failed: %s\n",
                BRIDGE_IFACE_NAME, nl_geterror(err));
        nl_addr_put(addr);
        NL_CALL_VOID(nl_socket_free(sock),
                     "nl_socket_free", "sock=%p", (void *)sock);
        return -EIO;
    }

    printf("bridge_create: %s created (type bridge, IFF_UP)\n", BRIDGE_IFACE_NAME);

    bridge_ifindex = get_iface_index(BRIDGE_IFACE_NAME);
    if (bridge_ifindex < 0)
    {
        fprintf(stderr, "bridge_create: cannot resolve ifindex for %s\n",
                BRIDGE_IFACE_NAME);
        nl_addr_put(addr);
        NL_CALL_VOID(nl_socket_free(sock),
                     "nl_socket_free", "sock=%p", (void *)sock);
        return -EIO;
    }

    /* Prepare orig link (identifies Bridge by ifindex for rtnl_link_change). */
    NL_CALL_RET(orig, rtnl_link_alloc(), "rtnl_link_alloc", "");
    if (!orig)
    {
        fprintf(stderr, "bridge_create: failed to allocate orig link\n");
        nl_addr_put(addr);
        NL_CALL_VOID(nl_socket_free(sock),
                     "nl_socket_free", "sock=%p", (void *)sock);
        return -ENOMEM;
    }
    NL_CALL_VOID(rtnl_link_set_ifindex(orig, bridge_ifindex),
                 "rtnl_link_set_ifindex", "link=%p, ifindex=%d",
                 (void *)orig, bridge_ifindex);

    /* -- Step 2: ip link set Bridge mtu 9100 -------------------------------- */

    NL_CALL_RET(change, rtnl_link_alloc(), "rtnl_link_alloc", "");
    if (!change)
    {
        fprintf(stderr, "bridge_create: failed to allocate change link (mtu)\n");
        NL_CALL_VOID(rtnl_link_put(orig), "rtnl_link_put", "link=%p", (void *)orig);
        nl_addr_put(addr);
        NL_CALL_VOID(nl_socket_free(sock),
                     "nl_socket_free", "sock=%p", (void *)sock);
        return -ENOMEM;
    }

    NL_CALL_VOID(rtnl_link_set_mtu(change, BRIDGE_MTU),
                 "rtnl_link_set_mtu", "link=%p, mtu=%u",
                 (void *)change, BRIDGE_MTU);

    NL_CALL_RET(err, rtnl_link_change(sock, orig, change, 0),
                "rtnl_link_change",
                "sock=%p, orig=%p, change=%p (mtu=%u), flags=0",
                (void *)sock, (void *)orig, (void *)change, BRIDGE_MTU);
    NL_CALL_VOID(rtnl_link_put(change), "rtnl_link_put", "link=%p", (void *)change);
    change = NULL;

    if (err < 0)
    {
        fprintf(stderr, "bridge_create: RTM_SETLINK mtu failed: %s\n",
                nl_geterror(err));
        NL_CALL_VOID(rtnl_link_put(orig), "rtnl_link_put", "link=%p", (void *)orig);
        nl_addr_put(addr);
        NL_CALL_VOID(nl_socket_free(sock),
                     "nl_socket_free", "sock=%p", (void *)sock);
        return -EIO;
    }

    printf("bridge_create: %s MTU set to %u\n", BRIDGE_IFACE_NAME, BRIDGE_MTU);

    /* -- Step 3: ip link set Bridge address <mac> --------------------------- */

    NL_CALL_RET(change, rtnl_link_alloc(), "rtnl_link_alloc", "");
    if (!change)
    {
        fprintf(stderr, "bridge_create: failed to allocate change link (addr)\n");
        NL_CALL_VOID(rtnl_link_put(orig), "rtnl_link_put", "link=%p", (void *)orig);
        nl_addr_put(addr);
        NL_CALL_VOID(nl_socket_free(sock),
                     "nl_socket_free", "sock=%p", (void *)sock);
        return -ENOMEM;
    }

    /* rtnl_link_set_addr() takes a reference; addr is released after the call. */
    NL_CALL_VOID(rtnl_link_set_addr(change, addr),
                 "rtnl_link_set_addr", "link=%p, addr=\"%s\"",
                 (void *)change, mac);
    nl_addr_put(addr);
    addr = NULL;

    NL_CALL_RET(err, rtnl_link_change(sock, orig, change, 0),
                "rtnl_link_change",
                "sock=%p, orig=%p, change=%p (addr=\"%s\"), flags=0",
                (void *)sock, (void *)orig, (void *)change, mac);
    NL_CALL_VOID(rtnl_link_put(change), "rtnl_link_put", "link=%p", (void *)change);
    change = NULL;

    if (err < 0)
    {
        fprintf(stderr, "bridge_create: RTM_SETLINK address failed: %s\n",
                nl_geterror(err));
        NL_CALL_VOID(rtnl_link_put(orig), "rtnl_link_put", "link=%p", (void *)orig);
        NL_CALL_VOID(nl_socket_free(sock),
                     "nl_socket_free", "sock=%p", (void *)sock);
        return -EIO;
    }

    printf("bridge_create: %s address set to %s\n", BRIDGE_IFACE_NAME, mac);

    /* orig is no longer needed for Bridge-level changes; free it now. */
    NL_CALL_VOID(rtnl_link_put(orig), "rtnl_link_put", "link=%p", (void *)orig);
    orig = NULL;

    /* -- Step 4: bridge vlan del vid 1 dev Bridge self ---------------------- */

    err = bridge_do_vlan_del(sock, bridge_ifindex, BRIDGE_DEFAULT_VID);
    if (err < 0)
    {
        fprintf(stderr, "bridge_create: bridge vlan del vid %d failed: %s\n",
                BRIDGE_DEFAULT_VID, nl_geterror(err));
        NL_CALL_VOID(nl_socket_free(sock),
                     "nl_socket_free", "sock=%p", (void *)sock);
        return -EIO;
    }

    printf("bridge_create: VLAN %d removed from %s self\n",
           BRIDGE_DEFAULT_VID, BRIDGE_IFACE_NAME);

    /* -- Step 5: ip link add dummy type dummy ------------------------------- */

    NL_CALL_RET(link, rtnl_link_alloc(), "rtnl_link_alloc", "");
    if (!link)
    {
        fprintf(stderr, "bridge_create: failed to allocate link for dummy\n");
        NL_CALL_VOID(nl_socket_free(sock),
                     "nl_socket_free", "sock=%p", (void *)sock);
        return -ENOMEM;
    }

    NL_CALL_VOID(rtnl_link_set_name(link, DUMMY_IFACE_NAME),
                 "rtnl_link_set_name", "link=%p, name=\"%s\"",
                 (void *)link, DUMMY_IFACE_NAME);

    NL_CALL_RET(_nl_err, rtnl_link_set_type(link, "dummy"),
                "rtnl_link_set_type", "link=%p, type=\"dummy\"", (void *)link);
    if (_nl_err < 0)
    {
        fprintf(stderr, "bridge_create: rtnl_link_set_type(dummy) failed: %s\n",
                nl_geterror(_nl_err));
        NL_CALL_VOID(rtnl_link_put(link), "rtnl_link_put", "link=%p", (void *)link);
        NL_CALL_VOID(nl_socket_free(sock),
                     "nl_socket_free", "sock=%p", (void *)sock);
        return -EIO;
    }

    NL_CALL_RET(err, rtnl_link_add(sock, link, NLM_F_CREATE | NLM_F_EXCL),
                "rtnl_link_add",
                "sock=%p, link=%p (dummy), flags=NLM_F_CREATE|NLM_F_EXCL",
                (void *)sock, (void *)link);
    NL_CALL_VOID(rtnl_link_put(link), "rtnl_link_put", "link=%p", (void *)link);
    link = NULL;

    if (err < 0)
    {
        fprintf(stderr, "bridge_create: RTM_NEWLINK for %s failed: %s\n",
                DUMMY_IFACE_NAME, nl_geterror(err));
        NL_CALL_VOID(nl_socket_free(sock),
                     "nl_socket_free", "sock=%p", (void *)sock);
        return -EIO;
    }

    printf("bridge_create: %s interface created (type dummy)\n", DUMMY_IFACE_NAME);

    dummy_ifindex = get_iface_index(DUMMY_IFACE_NAME);
    if (dummy_ifindex < 0)
    {
        fprintf(stderr, "bridge_create: cannot resolve ifindex for %s\n",
                DUMMY_IFACE_NAME);
        NL_CALL_VOID(nl_socket_free(sock),
                     "nl_socket_free", "sock=%p", (void *)sock);
        return -EIO;
    }

    /* -- Step 6: ip link set dummy master Bridge ---------------------------- */

    NL_CALL_RET(orig, rtnl_link_alloc(), "rtnl_link_alloc", "");
    if (!orig)
    {
        fprintf(stderr, "bridge_create: failed to allocate orig link (dummy)\n");
        NL_CALL_VOID(nl_socket_free(sock),
                     "nl_socket_free", "sock=%p", (void *)sock);
        return -ENOMEM;
    }
    NL_CALL_VOID(rtnl_link_set_ifindex(orig, dummy_ifindex),
                 "rtnl_link_set_ifindex", "link=%p, ifindex=%d",
                 (void *)orig, dummy_ifindex);

    NL_CALL_RET(change, rtnl_link_alloc(), "rtnl_link_alloc", "");
    if (!change)
    {
        fprintf(stderr, "bridge_create: failed to allocate change link (master)\n");
        NL_CALL_VOID(rtnl_link_put(orig), "rtnl_link_put", "link=%p", (void *)orig);
        NL_CALL_VOID(nl_socket_free(sock),
                     "nl_socket_free", "sock=%p", (void *)sock);
        return -ENOMEM;
    }

    NL_CALL_VOID(rtnl_link_set_master(change, bridge_ifindex),
                 "rtnl_link_set_master", "link=%p, master_ifindex=%d",
                 (void *)change, bridge_ifindex);

    NL_CALL_RET(err, rtnl_link_change(sock, orig, change, 0),
                "rtnl_link_change",
                "sock=%p, orig=%p, change=%p (master=%d), flags=0",
                (void *)sock, (void *)orig, (void *)change, bridge_ifindex);
    NL_CALL_VOID(rtnl_link_put(change), "rtnl_link_put", "link=%p", (void *)change);
    NL_CALL_VOID(rtnl_link_put(orig),   "rtnl_link_put", "link=%p", (void *)orig);
    change = NULL;
    orig   = NULL;

    if (err < 0)
    {
        fprintf(stderr, "bridge_create: RTM_SETLINK (IFLA_MASTER) failed: %s\n",
                nl_geterror(err));
        NL_CALL_VOID(nl_socket_free(sock),
                     "nl_socket_free", "sock=%p", (void *)sock);
        return -EIO;
    }

    printf("bridge_create: %s enslaved to %s\n", DUMMY_IFACE_NAME, BRIDGE_IFACE_NAME);

    /* -- Step 7: ip link set Bridge type bridge vlan_filtering 1 ------------ */

    err = bridge_set_vlan_filtering(sock, bridge_ifindex, 1);
    if (err < 0)
    {
        fprintf(stderr, "bridge_create: vlan_filtering=1 failed: %s\n",
                nl_geterror(err));
        NL_CALL_VOID(nl_socket_free(sock),
                     "nl_socket_free", "sock=%p", (void *)sock);
        return -EIO;
    }

    printf("bridge_create: VLAN filtering enabled on %s\n", BRIDGE_IFACE_NAME);

    NL_CALL_VOID(nl_socket_free(sock), "nl_socket_free", "sock=%p", (void *)sock);
    return 0;
}

int bridge_remove(void)
{
    struct nl_sock   *sock = NULL;
    struct rtnl_link *link = NULL;
    int               ifindex;
    int               err;
    int               _nl_err;

    NL_CALL_RET(sock, nl_socket_alloc(), "nl_socket_alloc", "");
    if (!sock)
    {
        fprintf(stderr, "bridge_remove: failed to allocate Netlink socket\n");
        return -ENOMEM;
    }

    NL_CALL_RET(_nl_err, nl_connect(sock, NETLINK_ROUTE),
                "nl_connect", "sock=%p, protocol=NETLINK_ROUTE", (void *)sock);
    if (_nl_err < 0)
    {
        fprintf(stderr, "bridge_remove: nl_connect failed: %s\n",
                nl_geterror(_nl_err));
        NL_CALL_VOID(nl_socket_free(sock),
                     "nl_socket_free", "sock=%p", (void *)sock);
        return -EIO;
    }

    /* -- Step 1: ip link del Bridge ----------------------------------------- */

    ifindex = get_iface_index(BRIDGE_IFACE_NAME);
    if (ifindex < 0)
    {
        fprintf(stderr, "bridge_remove: '%s' not found, skipping\n",
                BRIDGE_IFACE_NAME);
    }
    else
    {
        NL_CALL_RET(link, rtnl_link_alloc(), "rtnl_link_alloc", "");
        if (!link)
        {
            fprintf(stderr, "bridge_remove: failed to allocate link (Bridge)\n");
            NL_CALL_VOID(nl_socket_free(sock),
                         "nl_socket_free", "sock=%p", (void *)sock);
            return -ENOMEM;
        }

        NL_CALL_VOID(rtnl_link_set_ifindex(link, ifindex),
                     "rtnl_link_set_ifindex", "link=%p, ifindex=%d",
                     (void *)link, ifindex);

        NL_CALL_RET(err, rtnl_link_delete(sock, link),
                    "rtnl_link_delete", "sock=%p, link=%p (%s)",
                    (void *)sock, (void *)link, BRIDGE_IFACE_NAME);
        NL_CALL_VOID(rtnl_link_put(link), "rtnl_link_put", "link=%p", (void *)link);
        link = NULL;

        if (err < 0)
        {
            fprintf(stderr, "bridge_remove: RTM_DELLINK for %s failed: %s\n",
                    BRIDGE_IFACE_NAME, nl_geterror(err));
            NL_CALL_VOID(nl_socket_free(sock),
                         "nl_socket_free", "sock=%p", (void *)sock);
            return -EIO;
        }

        printf("bridge_remove: %s deleted\n", BRIDGE_IFACE_NAME);
    }

    /* -- Step 2: ip link del dummy ------------------------------------------ */

    ifindex = get_iface_index(DUMMY_IFACE_NAME);
    if (ifindex < 0)
    {
        fprintf(stderr, "bridge_remove: '%s' not found, skipping\n",
                DUMMY_IFACE_NAME);
    }
    else
    {
        NL_CALL_RET(link, rtnl_link_alloc(), "rtnl_link_alloc", "");
        if (!link)
        {
            fprintf(stderr, "bridge_remove: failed to allocate link (dummy)\n");
            NL_CALL_VOID(nl_socket_free(sock),
                         "nl_socket_free", "sock=%p", (void *)sock);
            return -ENOMEM;
        }

        NL_CALL_VOID(rtnl_link_set_ifindex(link, ifindex),
                     "rtnl_link_set_ifindex", "link=%p, ifindex=%d",
                     (void *)link, ifindex);

        NL_CALL_RET(err, rtnl_link_delete(sock, link),
                    "rtnl_link_delete", "sock=%p, link=%p (%s)",
                    (void *)sock, (void *)link, DUMMY_IFACE_NAME);
        NL_CALL_VOID(rtnl_link_put(link), "rtnl_link_put", "link=%p", (void *)link);
        link = NULL;

        if (err < 0)
        {
            fprintf(stderr, "bridge_remove: RTM_DELLINK for %s failed: %s\n",
                    DUMMY_IFACE_NAME, nl_geterror(err));
            NL_CALL_VOID(nl_socket_free(sock),
                         "nl_socket_free", "sock=%p", (void *)sock);
            return -EIO;
        }

        printf("bridge_remove: %s deleted\n", DUMMY_IFACE_NAME);
    }

    NL_CALL_VOID(nl_socket_free(sock), "nl_socket_free", "sock=%p", (void *)sock);
    return 0;
}
