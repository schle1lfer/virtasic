/**
 * @file bridge_netlink.c
 * @brief Raw Linux Netlink API helpers for bridge VLAN and port management.
 *
 * All three functions open a fresh NETLINK_ROUTE socket, build an RTM_SETLINK
 * message by hand, send it, read back the kernel's NLMSG_ERROR acknowledgement,
 * and then close the socket.  No libnl dependency is required.
 *
 * Netlink message layout used here:
 *
 *   [ struct nlmsghdr ]
 *   [ struct ifinfomsg ]
 *   [ struct rtattr ... ]   <-- one or more attributes / nested attributes
 *
 * Nested attributes (IFLA_AF_SPEC) are opened with nl_attr_nest_start() and
 * closed with nl_attr_nest_end(), which back-patches the rta_len field once
 * all inner attributes have been appended.
 */

#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <net/if.h>             /* if_nametoindex()                          */
#include <sys/socket.h>
#include <linux/netlink.h>      /* struct nlmsghdr, NLMSG_* macros           */
#include <linux/rtnetlink.h>    /* RTM_SETLINK, struct ifinfomsg, IFLA_*     */
#include <linux/if_bridge.h>    /* struct bridge_vlan_info, BRIDGE_FLAGS_*   */

#include "bridge_netlink.h"

/* -------------------------------------------------------------------------
 * Internal buffer size constants
 * ---------------------------------------------------------------------- */

/** Maximum size of the full Netlink request message (header + payload). */
#define NL_REQ_SIZE   256

/** Maximum size of the receive buffer for the kernel's ACK reply. */
#define NL_RECV_SIZE  512

/* -------------------------------------------------------------------------
 * Low-level Netlink helpers
 * ---------------------------------------------------------------------- */

/**
 * nl_attr_add() - Append a Netlink attribute to a message buffer.
 *
 * @param n       Pointer to the Netlink message header being built.
 * @param maxlen  Total capacity of the buffer that @p n lives in (bytes).
 * @param type    Attribute type (RTA_* or IFLA_* constant).
 * @param data    Pointer to the attribute payload, or NULL for zero length.
 * @param alen    Length of @p data in bytes.
 *
 * @return 0 on success, -ENOSPC if the attribute would overflow the buffer.
 */
static int nl_attr_add(struct nlmsghdr *n, int maxlen,
                       int type, const void *data, int alen)
{
    struct rtattr *rta;
    int rta_len = RTA_LENGTH(alen);

    /* Verify the new attribute fits within the pre-allocated buffer. */
    if (NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(rta_len) > (unsigned int)maxlen)
        return -ENOSPC;

    /* Position the new attribute right after the existing message content. */
    rta = (struct rtattr *)((char *)n + NLMSG_ALIGN(n->nlmsg_len));
    rta->rta_type = type;
    rta->rta_len  = rta_len;

    /* Copy the payload (if any) into the attribute's data region. */
    if (alen && data)
        memcpy(RTA_DATA(rta), data, alen);

    /* Advance the message length by the (aligned) attribute size. */
    n->nlmsg_len = NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(rta_len);
    return 0;
}

/**
 * nl_attr_nest_start() - Open a nested Netlink attribute.
 *
 * Appends an empty rtattr header that will later be closed (and have its
 * rta_len fixed up) by nl_attr_nest_end().  All attributes added between
 * nl_attr_nest_start() and nl_attr_nest_end() become children of this nest.
 *
 * @param n       Pointer to the Netlink message header being built.
 * @param maxlen  Total capacity of the buffer that @p n lives in (bytes).
 * @param type    Attribute type for the nest (e.g. IFLA_AF_SPEC).
 *
 * @return Pointer to the new rtattr (needed by nl_attr_nest_end), or NULL
 *         if the header would overflow the buffer.
 */
static struct rtattr *nl_attr_nest_start(struct nlmsghdr *n, int maxlen, int type)
{
    /* Record the location of the nest's rtattr header before appending it. */
    struct rtattr *nest = (struct rtattr *)((char *)n + NLMSG_ALIGN(n->nlmsg_len));

    /* Append a zero-length placeholder; children will fill in the space. */
    if (nl_attr_add(n, maxlen, type, NULL, 0) < 0)
        return NULL;

    return nest;
}

/**
 * nl_attr_nest_end() - Close a nested Netlink attribute.
 *
 * Back-patches the rta_len of the nest header opened by nl_attr_nest_start()
 * to cover all the child attributes that were appended since then.
 *
 * @param n     Pointer to the Netlink message header being built.
 * @param nest  The pointer returned by the matching nl_attr_nest_start() call.
 */
static void nl_attr_nest_end(struct nlmsghdr *n, struct rtattr *nest)
{
    /* rta_len spans from the nest header's start to the current message end. */
    nest->rta_len = (uint16_t)((char *)n + n->nlmsg_len - (char *)nest);
}

/**
 * nl_socket_open() - Create and bind a raw NETLINK_ROUTE socket.
 *
 * @return File descriptor on success, negative errno on failure.
 */
static int nl_socket_open(void)
{
    struct sockaddr_nl sa;
    int fd;

    fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0)
        return -errno;

    /* Bind to the kernel's Netlink address (nl_pid = 0 → auto-assign). */
    memset(&sa, 0, sizeof(sa));
    sa.nl_family = AF_NETLINK;

    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        int err = -errno;
        close(fd);
        return err;
    }

    return fd;
}

/**
 * nl_send_recv() - Send a Netlink message and return the kernel's error code.
 *
 * Sends the message pointed to by @p n, then reads back the NLMSG_ERROR
 * acknowledgement and extracts the embedded error value.
 *
 * @param fd  Open NETLINK_ROUTE socket file descriptor.
 * @param n   Fully-formed Netlink message to send.
 *
 * @return 0 on success, negative errno on failure (kernel or send/recv error).
 */
static int nl_send_recv(int fd, struct nlmsghdr *n)
{
    char buf[NL_RECV_SIZE];
    struct nlmsghdr *resp;
    ssize_t len;

    /* Transmit the request message. */
    if (send(fd, n, n->nlmsg_len, 0) < 0)
        return -errno;

    /* Block until the kernel sends back an acknowledgement. */
    len = recv(fd, buf, sizeof(buf), 0);
    if (len < 0)
        return -errno;

    resp = (struct nlmsghdr *)buf;

    /* Validate the received message length before dereferencing. */
    if (!NLMSG_OK(resp, (unsigned int)len))
        return -EINVAL;

    /* The kernel always responds with NLMSG_ERROR (error == 0 means success). */
    if (resp->nlmsg_type == NLMSG_ERROR) {
        struct nlmsgerr *err_msg = (struct nlmsgerr *)NLMSG_DATA(resp);
        return err_msg->error;  /* 0 on success, negative errno on error */
    }

    return 0;
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

/**
 * bridge_vlan_add_self() - Add a VLAN to the bridge device itself ("self").
 *
 * @details
 *   Sends an RTM_SETLINK message with @c ifi_family=AF_BRIDGE targeting the
 *   bridge interface.  Inside @c IFLA_AF_SPEC the attribute
 *   @c IFLA_BRIDGE_FLAGS is set to @c BRIDGE_FLAGS_SELF, which directs the
 *   kernel to install the VLAN entry on the bridge master's own filtering
 *   database rather than on a port.  A @c IFLA_BRIDGE_VLAN_INFO attribute
 *   carries the VLAN ID.
 *
 *   Equivalent to:
 *   @code
 *   bridge vlan add dev <bridge_name> vid <vlan_id> self
 *   @endcode
 *
 * @param[in] bridge_name  Name of the bridge interface (e.g. "Bridge").
 * @param[in] vlan_id      802.1Q VLAN identifier to add (1–4094).
 *
 * @return
 *    0        – success. \n
 *   -ENODEV   – @p bridge_name does not exist or cannot be resolved. \n
 *   -ENOSPC   – internal buffer overflow while building the Netlink message. \n
 *   -errno    – socket, send, or recv system call failed. \n
 *   negative  – kernel rejected the request (e.g. -EPERM, -EINVAL).
 */
int bridge_vlan_add_self(const char *bridge_name, uint16_t vlan_id)
{
    /* Fixed-size buffer that holds the entire Netlink message. */
    struct {
        struct nlmsghdr  nlh;
        struct ifinfomsg ifm;
        char             buf[NL_REQ_SIZE];
    } req;

    struct bridge_vlan_info vinfo;
    struct rtattr *afspec;
    uint16_t bridge_flags;
    unsigned int ifindex;
    int fd, ret;

    /* Resolve the bridge name to its kernel interface index. */
    ifindex = if_nametoindex(bridge_name);
    if (!ifindex)
        return -ENODEV;

    /* Zero and fill the Netlink message header. */
    memset(&req, 0, sizeof(req));
    req.nlh.nlmsg_len   = NLMSG_LENGTH(sizeof(req.ifm));
    req.nlh.nlmsg_type  = RTM_SETLINK;
    req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    req.nlh.nlmsg_seq   = 1;

    /* Target the bridge interface itself using the AF_BRIDGE address family. */
    req.ifm.ifi_family = AF_BRIDGE;
    req.ifm.ifi_index  = (int)ifindex;

    /* Open IFLA_AF_SPEC — the container for bridge-specific attributes. */
    afspec = nl_attr_nest_start(&req.nlh, sizeof(req), IFLA_AF_SPEC);
    if (!afspec)
        return -ENOSPC;

    /* IFLA_BRIDGE_FLAGS = BRIDGE_FLAGS_SELF tells the kernel this VLAN
     * operation targets the bridge device itself, not one of its ports. */
    bridge_flags = BRIDGE_FLAGS_SELF;
    ret = nl_attr_add(&req.nlh, sizeof(req),
                      IFLA_BRIDGE_FLAGS, &bridge_flags, sizeof(bridge_flags));
    if (ret < 0)
        return ret;

    /* IFLA_BRIDGE_VLAN_INFO carries the VLAN ID to install. */
    memset(&vinfo, 0, sizeof(vinfo));
    vinfo.vid = vlan_id;
    ret = nl_attr_add(&req.nlh, sizeof(req),
                      IFLA_BRIDGE_VLAN_INFO, &vinfo, sizeof(vinfo));
    if (ret < 0)
        return ret;

    /* Close IFLA_AF_SPEC by patching its rta_len to cover all children. */
    nl_attr_nest_end(&req.nlh, afspec);

    /* Open socket, dispatch request, wait for ACK, then close. */
    fd = nl_socket_open();
    if (fd < 0)
        return fd;

    ret = nl_send_recv(fd, &req.nlh);
    close(fd);
    return ret;
}

/**
 * ip_link_set_master() - Attach a network interface to a bridge (set master).
 *
 * @details
 *   Sends an RTM_SETLINK message with @c ifi_family=AF_UNSPEC targeting
 *   @p interface_name.  The @c IFLA_MASTER attribute is set to the ifindex of
 *   @p bridge_name, which enslaves the interface to that bridge.
 *
 *   Equivalent to:
 *   @code
 *   ip link set <interface_name> master <bridge_name>
 *   @endcode
 *
 * @param[in] interface_name  Name of the interface to enslave (e.g. "eth1").
 * @param[in] bridge_name     Name of the bridge master (e.g. "Bridge").
 *
 * @return
 *    0        – success; @p interface_name is now a port of @p bridge_name. \n
 *   -ENODEV   – either interface name could not be resolved. \n
 *   -ENOSPC   – internal buffer overflow while building the Netlink message. \n
 *   -errno    – socket, send, or recv system call failed. \n
 *   negative  – kernel rejected the request (e.g. -EPERM, -EBUSY).
 */
int ip_link_set_master(const char *interface_name, const char *bridge_name)
{
    /* Fixed-size buffer that holds the entire Netlink message. */
    struct {
        struct nlmsghdr  nlh;
        struct ifinfomsg ifm;
        char             buf[NL_REQ_SIZE];
    } req;

    unsigned int slave_ifindex, master_ifindex;
    int fd, ret;

    /* Resolve both interface names to their kernel indices. */
    slave_ifindex = if_nametoindex(interface_name);
    if (!slave_ifindex)
        return -ENODEV;

    master_ifindex = if_nametoindex(bridge_name);
    if (!master_ifindex)
        return -ENODEV;

    /* Zero and fill the Netlink message header. */
    memset(&req, 0, sizeof(req));
    req.nlh.nlmsg_len   = NLMSG_LENGTH(sizeof(req.ifm));
    req.nlh.nlmsg_type  = RTM_SETLINK;
    req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    req.nlh.nlmsg_seq   = 1;

    /* Target the slave interface; AF_UNSPEC is correct for IFLA_MASTER. */
    req.ifm.ifi_family = AF_UNSPEC;
    req.ifm.ifi_index  = (int)slave_ifindex;

    /* IFLA_MASTER holds the ifindex of the bridge to attach to.
     * Setting this to a non-zero value enslaves the interface. */
    ret = nl_attr_add(&req.nlh, sizeof(req),
                      IFLA_MASTER, &master_ifindex, sizeof(master_ifindex));
    if (ret < 0)
        return ret;

    /* Open socket, dispatch request, wait for ACK, then close. */
    fd = nl_socket_open();
    if (fd < 0)
        return fd;

    ret = nl_send_recv(fd, &req.nlh);
    close(fd);
    return ret;
}

/**
 * bridge_vlan_add_port() - Add a VLAN to a bridge member port interface.
 *
 * @details
 *   Sends an RTM_SETLINK message with @c ifi_family=AF_BRIDGE targeting
 *   @p interface_name.  Inside @c IFLA_AF_SPEC a @c IFLA_BRIDGE_VLAN_INFO
 *   attribute carries the VLAN ID.  Unlike bridge_vlan_add_self() no
 *   @c BRIDGE_FLAGS_SELF flag is sent, so the kernel applies the VLAN entry
 *   to the port rather than the bridge master.
 *
 *   The interface must already be enslaved to a bridge (via ip_link_set_master()
 *   or equivalent) before calling this function.
 *
 *   Equivalent to:
 *   @code
 *   bridge vlan add dev <interface_name> vid <vlan_id>
 *   @endcode
 *
 * @param[in] interface_name  Name of the bridge port (e.g. "eth1").
 * @param[in] vlan_id         802.1Q VLAN identifier to add (1–4094).
 *
 * @return
 *    0        – success. \n
 *   -ENODEV   – @p interface_name does not exist or cannot be resolved. \n
 *   -ENOSPC   – internal buffer overflow while building the Netlink message. \n
 *   -errno    – socket, send, or recv system call failed. \n
 *   negative  – kernel rejected the request (e.g. -EPERM, -EINVAL).
 */
int bridge_vlan_add_port(const char *interface_name, uint16_t vlan_id)
{
    /* Fixed-size buffer that holds the entire Netlink message. */
    struct {
        struct nlmsghdr  nlh;
        struct ifinfomsg ifm;
        char             buf[NL_REQ_SIZE];
    } req;

    struct bridge_vlan_info vinfo;
    struct rtattr *afspec;
    unsigned int ifindex;
    int fd, ret;

    /* Resolve the port interface name to its kernel interface index. */
    ifindex = if_nametoindex(interface_name);
    if (!ifindex)
        return -ENODEV;

    /* Zero and fill the Netlink message header. */
    memset(&req, 0, sizeof(req));
    req.nlh.nlmsg_len   = NLMSG_LENGTH(sizeof(req.ifm));
    req.nlh.nlmsg_type  = RTM_SETLINK;
    req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    req.nlh.nlmsg_seq   = 1;

    /* Target the port interface using the AF_BRIDGE address family. */
    req.ifm.ifi_family = AF_BRIDGE;
    req.ifm.ifi_index  = (int)ifindex;

    /* Open IFLA_AF_SPEC — the container for bridge-specific attributes. */
    afspec = nl_attr_nest_start(&req.nlh, sizeof(req), IFLA_AF_SPEC);
    if (!afspec)
        return -ENOSPC;

    /* IFLA_BRIDGE_VLAN_INFO carries the VLAN ID for the port's filter table.
     * No BRIDGE_FLAGS_SELF here — this targets the port, not the bridge. */
    memset(&vinfo, 0, sizeof(vinfo));
    vinfo.vid = vlan_id;
    ret = nl_attr_add(&req.nlh, sizeof(req),
                      IFLA_BRIDGE_VLAN_INFO, &vinfo, sizeof(vinfo));
    if (ret < 0)
        return ret;

    /* Close IFLA_AF_SPEC by patching its rta_len to cover all children. */
    nl_attr_nest_end(&req.nlh, afspec);

    /* Open socket, dispatch request, wait for ACK, then close. */
    fd = nl_socket_open();
    if (fd < 0)
        return fd;

    ret = nl_send_recv(fd, &req.nlh);
    close(fd);
    return ret;
}
