#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <time.h>
#include <linux/netlink.h>      /* keep only one copy */
#include <linux/if.h>
#include <linux/if_ether.h>
#include <linux/if_vlan.h>
#include <linux/if_link.h>      /* IFLA_LINKINFO, IFLA_INFO_KIND, IFLA_INFO_DATA */
#include <netinet/in.h>
#include <netlink/attr.h>       /* nla_nest_start / nla_nest_end */
#include <netlink/socket.h>
#include <netlink/netlink.h>
#include <netlink/route/link.h>
#include <netlink/route/link/vlan.h>
#include <sys/socket.h>
#include <sys/types.h>

#define PORT 8888
#define BUFFER_SIZE 1024

/*
 * ---------------------------------------------------------------------------
 * Netlink API call logging helpers
 *
 * NL_CALL_RET(var, call, fn_name, params_fmt, ...)
 *   Executes `call`, assigns the result to `var`, then prints the function
 *   name, formatted parameter description, and elapsed wall-clock time (ms).
 *
 * NL_CALL_VOID(call, fn_name, params_fmt, ...)
 *   Same as NL_CALL_RET but for calls whose return value is not used.
 * ---------------------------------------------------------------------------
 */
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

/* Forward declarations */
int get_words(const char* str, char*** words, int* cnt);
int cmd_show_interfaces();
int cmd_rename_interfaces(char* prefix, char* new_prefix);
int cmd_show_vlan();
int cmd_set_vlan_on_interface(char* iface, char* type, char* ver);
int cmd_set_vlan(char* ver, char* id);

/*
 * Tokenise `str` (space-separated words) into a freshly-allocated array of
 * C-strings.  The array pointer is written to *words; the caller must free
 * each element and then free the array itself.
 *
 * Bug fixes applied:
 *   - parameter changed to char*** so the caller's pointer is updated
 *   - sentinel check uses '\0' (null terminator), not '0' (digit zero)
 *   - word null-termination uses '\0', not '0'
 *   - old *words is freed before re-allocating
 */
int get_words(const char* str, char*** words, int* cnt)
{
    int length = 0;
    int in_word = 0;
    int word_index = 0;

    if (!str)
    {
        printf("str is NULL\n");
        return -1;
    }

    if (!cnt)
    {
        printf("cnt is NULL\n");
        return -2;
    }

    *cnt = 0;

    /* Free any previous array the caller may have passed in */
    if (*words)
    {
        printf("words is not NULL\n");
        free(*words);
        *words = NULL;
    }

    length = strlen(str);
    *words = malloc(length * sizeof(char*));

    if (*words == NULL)
    {
        perror("Failed to allocate memory for words");
        return -3;
    }

    char* word = malloc(length + 1);

    if (word == NULL)
    {
        perror("Failed to allocate memory for word");
        free(*words);
        *words = NULL;
        return -4;
    }

    word_index = 0;
    in_word = 0;
    for (int i = 0; i <= length; i++)
    {
        /* Bug fix: was 'str[i] != '0'' — must check for '\0', not digit '0' */
        if (str[i] != ' ' && str[i] != '\0')
        {
            word[word_index++] = str[i];
            in_word = 1;
        }
        else
        {
            if (in_word)
            {
                /* Bug fix: was 'word[word_index] = '0'' — must be '\0' */
                word[word_index] = '\0';
                (*words)[*cnt] = malloc((word_index + 1) * sizeof(char));
                if ((*words)[*cnt] == NULL)
                {
                    perror("Failed to allocate memory for a word");
                    for (int j = 0; j < *cnt; j++)
                    {
                        free((*words)[j]);
                    }
                    free(*words);
                    *words = NULL;
                    free(word);
                    return -5;
                }
                strcpy((*words)[*cnt], word);
                (*cnt)++;
                word_index = 0;
            }
            in_word = 0;
        }
    }

    free(word);

    return 0;
}

/*
 * Helper: return the interface index for the named interface, or -1 on error.
 * Uses ioctl(SIOCGIFINDEX) to avoid the net/if.h <-> linux/if.h conflict.
 */
static int get_iface_index(const char *iface_name)
{
    struct ifreq ifr;
    int fd;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        perror("socket (get_iface_index)");
        return -1;
    }

    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, iface_name, IFNAMSIZ - 1);

    if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0)
    {
        perror("ioctl SIOCGIFINDEX");
        close(fd);
        return -1;
    }

    close(fd);
    return ifr.ifr_ifindex;
}

/*
 * Create a VLAN sub-interface <iface_name>.<vlan_id> via Netlink RTM_NEWLINK.
 *
 * Bug fixes applied:
 *   - ifi_family: AF_PACKET -> AF_UNSPEC
 *   - Added IFLA_LINK (parent interface index) which is mandatory
 *   - IFLA_VLAN_ID is now correctly nested inside IFLA_LINKINFO / IFLA_INFO_DATA
 *   - VLAN ID attribute uses u16 (not u32)
 *   - NLM_F_REQUEST flag added
 *   - nlh NULL-check added
 *   - All error paths return -1 instead of calling exit()
 */
int create_vlan(const char *iface_name, int vlan_id)
{
    struct nl_sock *sock;
    struct nl_msg *msg;
    struct nlmsghdr *nlh;
    struct ifinfomsg *ifi;
    struct nlattr *linkinfo, *data;
    char vlan_ifname[IFNAMSIZ];
    int parent_idx;
    int _nl_err;
    const char *_nl_errmsg;

    parent_idx = get_iface_index(iface_name);
    if (parent_idx < 0)
    {
        fprintf(stderr, "Failed to get index for interface %s\n", iface_name);
        return -1;
    }

    NL_CALL_RET(sock, nl_socket_alloc(),
                "nl_socket_alloc", "");
    if (!sock)
    {
        perror("nl_socket_alloc");
        return -1;
    }

    NL_CALL_RET(_nl_err, nl_connect(sock, NETLINK_ROUTE),
                "nl_connect", "sock=%p, protocol=NETLINK_ROUTE", (void *)sock);
    if (_nl_err < 0)
    {
        perror("nl_connect");
        NL_CALL_VOID(nl_socket_free(sock),
                     "nl_socket_free", "sock=%p", (void *)sock);
        return -1;
    }

    snprintf(vlan_ifname, sizeof(vlan_ifname), "%s.%d", iface_name, vlan_id);

    NL_CALL_RET(msg, nlmsg_alloc(),
                "nlmsg_alloc", "");
    if (!msg)
    {
        perror("nlmsg_alloc");
        NL_CALL_VOID(nl_socket_free(sock),
                     "nl_socket_free", "sock=%p", (void *)sock);
        return -1;
    }

    NL_CALL_RET(nlh,
                nlmsg_put(msg, NL_AUTO_PORT, NL_AUTO_SEQ, RTM_NEWLINK,
                          sizeof(struct ifinfomsg),
                          NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL),
                "nlmsg_put",
                "msg=%p, port=NL_AUTO_PORT, seq=NL_AUTO_SEQ, type=RTM_NEWLINK,"
                " len=%zu, flags=NLM_F_REQUEST|NLM_F_CREATE|NLM_F_EXCL",
                (void *)msg, sizeof(struct ifinfomsg));
    if (!nlh)
    {
        fprintf(stderr, "nlmsg_put failed\n");
        NL_CALL_VOID(nlmsg_free(msg),
                     "nlmsg_free", "msg=%p", (void *)msg);
        NL_CALL_VOID(nl_socket_free(sock),
                     "nl_socket_free", "sock=%p", (void *)sock);
        return -1;
    }

    NL_CALL_RET(ifi, (struct ifinfomsg *)nlmsg_data(nlh),
                "nlmsg_data", "nlh=%p", (void *)nlh);
    /* Bug fix: was AF_PACKET — the correct family for link creation is AF_UNSPEC */
    ifi->ifi_family = AF_UNSPEC;
    ifi->ifi_type   = 0;
    ifi->ifi_index  = 0;
    ifi->ifi_flags  = 0;
    ifi->ifi_change = 0;

    /* New VLAN interface name */
    NL_CALL_RET(_nl_err, nla_put_string(msg, IFLA_IFNAME, vlan_ifname),
                "nla_put_string",
                "msg=%p, attr=IFLA_IFNAME, val=\"%s\"",
                (void *)msg, vlan_ifname);
    if (_nl_err < 0)
    {
        perror("nla_put IFLA_IFNAME");
        NL_CALL_VOID(nlmsg_free(msg),
                     "nlmsg_free", "msg=%p", (void *)msg);
        NL_CALL_VOID(nl_socket_free(sock),
                     "nl_socket_free", "sock=%p", (void *)sock);
        return -1;
    }

    /* Bug fix: parent interface index is mandatory for VLAN sub-interfaces */
    NL_CALL_RET(_nl_err, nla_put_u32(msg, IFLA_LINK, (uint32_t)parent_idx),
                "nla_put_u32",
                "msg=%p, attr=IFLA_LINK, val=%u",
                (void *)msg, (uint32_t)parent_idx);
    if (_nl_err < 0)
    {
        perror("nla_put IFLA_LINK");
        NL_CALL_VOID(nlmsg_free(msg),
                     "nlmsg_free", "msg=%p", (void *)msg);
        NL_CALL_VOID(nl_socket_free(sock),
                     "nl_socket_free", "sock=%p", (void *)sock);
        return -1;
    }

    /*
     * Bug fix: IFLA_VLAN_ID must be nested:
     *   IFLA_LINKINFO
     *     IFLA_INFO_KIND = "vlan"
     *     IFLA_INFO_DATA
     *       IFLA_VLAN_ID  (u16, not u32)
     */
    NL_CALL_RET(linkinfo, nla_nest_start(msg, IFLA_LINKINFO),
                "nla_nest_start", "msg=%p, attr=IFLA_LINKINFO", (void *)msg);
    if (!linkinfo)
    {
        fprintf(stderr, "nla_nest_start(IFLA_LINKINFO) failed\n");
        NL_CALL_VOID(nlmsg_free(msg),
                     "nlmsg_free", "msg=%p", (void *)msg);
        NL_CALL_VOID(nl_socket_free(sock),
                     "nl_socket_free", "sock=%p", (void *)sock);
        return -1;
    }

    NL_CALL_RET(_nl_err, nla_put_string(msg, IFLA_INFO_KIND, "vlan"),
                "nla_put_string",
                "msg=%p, attr=IFLA_INFO_KIND, val=\"vlan\"",
                (void *)msg);
    if (_nl_err < 0)
    {
        perror("nla_put IFLA_INFO_KIND");
        NL_CALL_VOID(nlmsg_free(msg),
                     "nlmsg_free", "msg=%p", (void *)msg);
        NL_CALL_VOID(nl_socket_free(sock),
                     "nl_socket_free", "sock=%p", (void *)sock);
        return -1;
    }

    NL_CALL_RET(data, nla_nest_start(msg, IFLA_INFO_DATA),
                "nla_nest_start", "msg=%p, attr=IFLA_INFO_DATA", (void *)msg);
    if (!data)
    {
        fprintf(stderr, "nla_nest_start(IFLA_INFO_DATA) failed\n");
        NL_CALL_VOID(nlmsg_free(msg),
                     "nlmsg_free", "msg=%p", (void *)msg);
        NL_CALL_VOID(nl_socket_free(sock),
                     "nl_socket_free", "sock=%p", (void *)sock);
        return -1;
    }

    /* Bug fix: VLAN ID is u16 per kernel ABI; was nla_put_u32 */
    NL_CALL_RET(_nl_err, nla_put_u16(msg, IFLA_VLAN_ID, (uint16_t)vlan_id),
                "nla_put_u16",
                "msg=%p, attr=IFLA_VLAN_ID, val=%u",
                (void *)msg, (uint16_t)vlan_id);
    if (_nl_err < 0)
    {
        perror("nla_put IFLA_VLAN_ID");
        NL_CALL_VOID(nlmsg_free(msg),
                     "nlmsg_free", "msg=%p", (void *)msg);
        NL_CALL_VOID(nl_socket_free(sock),
                     "nl_socket_free", "sock=%p", (void *)sock);
        return -1;
    }

    NL_CALL_VOID(nla_nest_end(msg, data),
                 "nla_nest_end", "msg=%p, nested=%p", (void *)msg, (void *)data);
    NL_CALL_VOID(nla_nest_end(msg, linkinfo),
                 "nla_nest_end", "msg=%p, nested=%p", (void *)msg, (void *)linkinfo);

    NL_CALL_RET(_nl_err, nl_send_auto(sock, msg),
                "nl_send_auto", "sock=%p, msg=%p", (void *)sock, (void *)msg);
    if (_nl_err < 0)
    {
        NL_CALL_RET(_nl_errmsg, nl_geterror(_nl_err),
                    "nl_geterror", "err=%d", _nl_err);
        fprintf(stderr, "nl_send_auto failed: %s\n", _nl_errmsg);
        NL_CALL_VOID(nlmsg_free(msg),
                     "nlmsg_free", "msg=%p", (void *)msg);
        NL_CALL_VOID(nl_socket_free(sock),
                     "nl_socket_free", "sock=%p", (void *)sock);
        return -1;
    }

    NL_CALL_VOID(nlmsg_free(msg),
                 "nlmsg_free", "msg=%p", (void *)msg);
    NL_CALL_VOID(nl_socket_free(sock),
                 "nl_socket_free", "sock=%p", (void *)sock);

    printf("VLAN %d created on interface %s\n", vlan_id, vlan_ifname);
    return 0;
}

int set_reuseraddr(int sockfd)
{
    int opt = 1;

    return setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
}

int set_nonblocking(int sockfd)
{
    int flags = fcntl(sockfd, F_GETFL, 0);
    if (flags == -1)
    {
        perror("fcntl");
        return -1;
    }
    return fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
}

void process_command(const char *cmd)
{
    char** cmd_words = NULL;
    int cmd_words_cnt = 0;

    /* Bug fix: pass &cmd_words so get_words can store the allocated array */
    if (get_words(cmd, &cmd_words, &cmd_words_cnt) < 0)
    {
        printf("Bad format command string: %s\n", cmd);
        return;
    }

    /* show interfaces */
    if (strcmp(cmd, "show interfaces") == 0)
    {
        printf("Executing: %s\n", cmd);
        cmd_show_interfaces();
    }
    /* rename interfaces <prefix> <new_prefix> */
    else if (strncmp(cmd, "rename interfaces", 17) == 0)
    {
        printf("Executing: %s\n", cmd);
        if (cmd_words_cnt != 4)
        {
            printf("Bad format command: %s\n", cmd);
        }
        else
        {
        cmd_rename_interfaces(cmd_words[2], cmd_words[3]);
    }
    }
    /* show vlan */
    else if (strcmp(cmd, "show vlan") == 0)
    {
        printf("Executing: %s\n", cmd);
        cmd_show_vlan();
    }
    /* set interface Ethernet56 type l2-trunk vlan v2 */
    /* Bug fix: "set interface " is 14 characters, not 15 */
    else if (strncmp(cmd, "set interface ", 14) == 0)
    {
        printf("Executing: %s\n", cmd);
        if (cmd_words_cnt != 7)
        {
            printf("Bad format command: %s\n", cmd);
        }
        else
        {
        cmd_set_vlan_on_interface(cmd_words[2], cmd_words[4], cmd_words[6]);
    }
    }
    /* set vlan v2 id 2 */
    else if (strncmp(cmd, "set vlan ", 9) == 0)
    {
        printf("Executing: %s\n", cmd);
        if (cmd_words_cnt != 5)
        {
            printf("Bad format command: %s\n", cmd);
        }
        else
        {
            cmd_set_vlan(cmd_words[2], cmd_words[4]);
        }
    }
    /* default */
    else
    {
        printf("Unknown command: %s\n", cmd);
    }

    /* Bug fix: free the words array allocated by get_words */
    for (int i = 0; i < cmd_words_cnt; i++)
    {
        free(cmd_words[i]);
    }
    free(cmd_words);
}

/*
 * cmd_show_interfaces - Query and display all network interfaces
 *
 * Description:
 *   Uses the Netlink ROUTE API (via libnl3) to enumerate all network interfaces
 *   present on the system and prints their index, name, type, and flags to stdout.
 *
 * Input parameters: none
 *
 * Output:
 *   Prints a table with columns: IDX, NAME, TYPE, FLAGS
 *   Each row represents one network interface.
 *
 * Return value:
 *    0  - success
 *   -1  - failed to allocate netlink socket
 *   -2  - failed to connect netlink socket to NETLINK_ROUTE
 *   -3  - failed to allocate the link cache from the kernel
 */
int cmd_show_interfaces()
{
    struct nl_sock *sock = NULL;
    struct nl_cache *cache = NULL;
    struct rtnl_link *link = NULL;
    struct nl_object *_nl_iter = NULL;
    int _nl_err;

    NL_CALL_RET(sock, nl_socket_alloc(),
                "nl_socket_alloc", "");
    if (!sock)
    {
        fprintf(stderr, "cmd_show_interfaces: failed to allocate netlink socket\n");
        return -1;
    }

    NL_CALL_RET(_nl_err, nl_connect(sock, NETLINK_ROUTE),
                "nl_connect", "sock=%p, protocol=NETLINK_ROUTE", (void *)sock);
    if (_nl_err < 0)
    {
        fprintf(stderr, "cmd_show_interfaces: failed to connect netlink socket\n");
        NL_CALL_VOID(nl_socket_free(sock),
                     "nl_socket_free", "sock=%p", (void *)sock);
        return -2;
    }

    NL_CALL_RET(_nl_err, rtnl_link_alloc_cache(sock, AF_UNSPEC, &cache),
                "rtnl_link_alloc_cache",
                "sock=%p, family=AF_UNSPEC, cache=%p",
                (void *)sock, (void *)cache);
    if (_nl_err < 0)
    {
        fprintf(stderr, "cmd_show_interfaces: failed to allocate link cache\n");
        NL_CALL_VOID(nl_socket_free(sock),
                     "nl_socket_free", "sock=%p", (void *)sock);
        return -3;
    }

    printf("%-5s  %-20s  %-12s  %s\n", "IDX", "NAME", "TYPE", "FLAGS");
    printf("%-5s  %-20s  %-12s  %s\n", "---", "----", "----", "-----");

    NL_CALL_RET(_nl_iter, nl_cache_get_first(cache),
                "nl_cache_get_first", "cache=%p", (void *)cache);
    link = (struct rtnl_link *)_nl_iter;
    while (link != NULL)
    {
        char flags_buf[256] = {0};
        const char *type;
        unsigned int _flags;
        int _ifindex;
        const char *_name;

        NL_CALL_RET(type, rtnl_link_get_type(link),
                    "rtnl_link_get_type", "link=%p", (void *)link);

        NL_CALL_RET(_flags, rtnl_link_get_flags(link),
                    "rtnl_link_get_flags", "link=%p", (void *)link);
        NL_CALL_VOID(rtnl_link_flags2str(_flags, flags_buf, sizeof(flags_buf)),
                     "rtnl_link_flags2str",
                     "flags=%u, buf=%p, len=%zu",
                     _flags, (void *)flags_buf, sizeof(flags_buf));

        NL_CALL_RET(_ifindex, rtnl_link_get_ifindex(link),
                    "rtnl_link_get_ifindex", "link=%p", (void *)link);
        NL_CALL_RET(_name, rtnl_link_get_name(link),
                    "rtnl_link_get_name", "link=%p", (void *)link);

        printf("%-5d  %-20s  %-12s  %s\n",
               _ifindex,
               _name,
               type ? type : "-",
               flags_buf[0] ? flags_buf : "none");

        NL_CALL_RET(_nl_iter, nl_cache_get_next((struct nl_object *)link),
                    "nl_cache_get_next", "obj=%p", (void *)link);
        link = (struct rtnl_link *)_nl_iter;
    }

    NL_CALL_VOID(nl_cache_free(cache),
                 "nl_cache_free", "cache=%p", (void *)cache);
    NL_CALL_VOID(nl_socket_free(sock),
                 "nl_socket_free", "sock=%p", (void *)sock);
    return 0;
}

/*
 * cmd_rename_interfaces - Rename network interfaces by replacing a name prefix
 *
 * Description:
 *   Iterates all network interfaces via the Netlink ROUTE API. For each
 *   interface whose name begins with `prefix`, renames it by substituting
 *   `prefix` with `new_prefix` while keeping the rest of the name unchanged.
 *   The rename is sent to the kernel via RTM_SETLINK (rtnl_link_change).
 *   Note: the kernel requires an interface to be DOWN before it can be renamed.
 *   Each renamed interface is printed to stdout; errors are printed to stderr.
 *
 * Input parameters:
 *   prefix     - the interface name prefix to match (e.g. "eth")
 *   new_prefix - the replacement prefix to use   (e.g. "net")
 *
 * Output:
 *   For each renamed interface, prints: <old_name> -> <new_name>
 *
 * Return value:
 *    0  - success (all matching interfaces were renamed successfully)
 *   -1  - prefix or new_prefix is NULL
 *   -2  - failed to allocate netlink socket
 *   -3  - failed to connect netlink socket to NETLINK_ROUTE
 *   -4  - failed to allocate the link cache from the kernel
 *   -5  - one or more rename operations failed (kernel error)
 */
int cmd_rename_interfaces(char* prefix, char* new_prefix)
{
    struct nl_sock *sock = NULL;
    struct nl_cache *cache = NULL;
    struct rtnl_link *link = NULL;
    struct rtnl_link *change = NULL;
    struct nl_object *_nl_iter = NULL;
    int ret_code = 0;
    size_t prefix_len;
    int _nl_err;

    if (!prefix || !new_prefix)
    {
        fprintf(stderr, "cmd_rename_interfaces: prefix or new_prefix is NULL\n");
        return -1;
    }

    NL_CALL_RET(sock, nl_socket_alloc(),
                "nl_socket_alloc", "");
    if (!sock)
    {
        fprintf(stderr, "cmd_rename_interfaces: failed to allocate netlink socket\n");
        return -2;
    }

    NL_CALL_RET(_nl_err, nl_connect(sock, NETLINK_ROUTE),
                "nl_connect", "sock=%p, protocol=NETLINK_ROUTE", (void *)sock);
    if (_nl_err < 0)
    {
        fprintf(stderr, "cmd_rename_interfaces: failed to connect netlink socket\n");
        NL_CALL_VOID(nl_socket_free(sock),
                     "nl_socket_free", "sock=%p", (void *)sock);
        return -3;
    }

    NL_CALL_RET(_nl_err, rtnl_link_alloc_cache(sock, AF_UNSPEC, &cache),
                "rtnl_link_alloc_cache",
                "sock=%p, family=AF_UNSPEC, cache=%p",
                (void *)sock, (void *)cache);
    if (_nl_err < 0)
    {
        fprintf(stderr, "cmd_rename_interfaces: failed to allocate link cache\n");
        NL_CALL_VOID(nl_socket_free(sock),
                     "nl_socket_free", "sock=%p", (void *)sock);
        return -4;
    }

    prefix_len = strlen(prefix);

    NL_CALL_RET(_nl_iter, nl_cache_get_first(cache),
                "nl_cache_get_first", "cache=%p", (void *)cache);
    link = (struct rtnl_link *)_nl_iter;
    while (link != NULL)
    {
        const char *name;
        char new_name[IFNAMSIZ];
        int err;

        NL_CALL_RET(name, rtnl_link_get_name(link),
                    "rtnl_link_get_name", "link=%p", (void *)link);

        if (!name || strncmp(name, prefix, prefix_len) != 0)
        {
            NL_CALL_RET(_nl_iter, nl_cache_get_next((struct nl_object *)link),
                        "nl_cache_get_next", "obj=%p", (void *)link);
            link = (struct rtnl_link *)_nl_iter;
            continue;
        }

        snprintf(new_name, sizeof(new_name), "%s%s", new_prefix, name + prefix_len);

        NL_CALL_RET(change, rtnl_link_alloc(),
                    "rtnl_link_alloc", "");
        if (!change)
        {
            fprintf(stderr, "cmd_rename_interfaces: failed to allocate change link for %s\n", name);
            ret_code = -5;
            break;
        }

        NL_CALL_VOID(rtnl_link_set_name(change, new_name),
                     "rtnl_link_set_name", "link=%p, name=\"%s\"",
                     (void *)change, new_name);

        NL_CALL_RET(err, rtnl_link_change(sock, link, change, 0),
                    "rtnl_link_change",
                    "sock=%p, link=%p, change=%p, flags=0",
                    (void *)sock, (void *)link, (void *)change);
        if (err < 0)
        {
            const char *_errmsg;
            NL_CALL_RET(_errmsg, nl_geterror(err),
                        "nl_geterror", "err=%d", err);
            fprintf(stderr, "cmd_rename_interfaces: rename %s -> %s failed: %s\n",
                    name, new_name, _errmsg);
            NL_CALL_VOID(rtnl_link_put(change),
                         "rtnl_link_put", "link=%p", (void *)change);
            ret_code = -5;
            NL_CALL_RET(_nl_iter, nl_cache_get_next((struct nl_object *)link),
                        "nl_cache_get_next", "obj=%p", (void *)link);
            link = (struct rtnl_link *)_nl_iter;
            continue;
        }

        printf("%s -> %s\n", name, new_name);
        NL_CALL_VOID(rtnl_link_put(change),
                     "rtnl_link_put", "link=%p", (void *)change);

        NL_CALL_RET(_nl_iter, nl_cache_get_next((struct nl_object *)link),
                    "nl_cache_get_next", "obj=%p", (void *)link);
        link = (struct rtnl_link *)_nl_iter;
    }

    NL_CALL_VOID(nl_cache_free(cache),
                 "nl_cache_free", "cache=%p", (void *)cache);
    NL_CALL_VOID(nl_socket_free(sock),
                 "nl_socket_free", "sock=%p", (void *)sock);
    return ret_code;
}

/*
 * cmd_show_vlan - Display all existing VLAN interfaces and their basic properties
 *
 * Description:
 *   Enumerates all network interfaces via the Netlink ROUTE API and filters
 *   those whose kernel type is "vlan". For each VLAN interface, the following
 *   properties are displayed:
 *     - Interface name
 *     - Parent interface name (resolved from the parent's ifindex)
 *     - VLAN ID (802.1Q tag)
 *     - VLAN flags (e.g. reorder-hdr, gvrp, loose-binding, mvrp, bridge-binding)
 *
 * Input parameters: none
 *
 * Output:
 *   Prints a table with columns: NAME, PARENT, VLAN_ID, FLAGS
 *
 * Return value:
 *    0  - success
 *   -1  - failed to allocate netlink socket
 *   -2  - failed to connect netlink socket to NETLINK_ROUTE
 *   -3  - failed to allocate the link cache from the kernel
 */
int cmd_show_vlan()
{
    struct nl_sock *sock = NULL;
    struct nl_cache *cache = NULL;
    struct rtnl_link *link = NULL;
    struct rtnl_link *parent_link = NULL;
    struct nl_object *_nl_iter = NULL;
    int _nl_err;

    NL_CALL_RET(sock, nl_socket_alloc(),
                "nl_socket_alloc", "");
    if (!sock)
    {
        fprintf(stderr, "cmd_show_vlan: failed to allocate netlink socket\n");
        return -1;
    }

    NL_CALL_RET(_nl_err, nl_connect(sock, NETLINK_ROUTE),
                "nl_connect", "sock=%p, protocol=NETLINK_ROUTE", (void *)sock);
    if (_nl_err < 0)
    {
        fprintf(stderr, "cmd_show_vlan: failed to connect netlink socket\n");
        NL_CALL_VOID(nl_socket_free(sock),
                     "nl_socket_free", "sock=%p", (void *)sock);
        return -2;
    }

    NL_CALL_RET(_nl_err, rtnl_link_alloc_cache(sock, AF_UNSPEC, &cache),
                "rtnl_link_alloc_cache",
                "sock=%p, family=AF_UNSPEC, cache=%p",
                (void *)sock, (void *)cache);
    if (_nl_err < 0)
    {
        fprintf(stderr, "cmd_show_vlan: failed to allocate link cache\n");
        NL_CALL_VOID(nl_socket_free(sock),
                     "nl_socket_free", "sock=%p", (void *)sock);
        return -3;
    }

    printf("%-20s  %-20s  %-10s  %s\n", "NAME", "PARENT", "VLAN_ID", "FLAGS");
    printf("%-20s  %-20s  %-10s  %s\n", "----", "------", "-------", "-----");

    NL_CALL_RET(_nl_iter, nl_cache_get_first(cache),
                "nl_cache_get_first", "cache=%p", (void *)cache);
    link = (struct rtnl_link *)_nl_iter;
    while (link != NULL)
    {
        char flags_buf[128] = {0};
        const char *parent_name = "-";
        int parent_idx;
        int vlan_id;
        int _is_vlan;
        uint32_t _vlan_flags;
        const char *_name;

        NL_CALL_RET(_is_vlan, rtnl_link_is_vlan(link),
                    "rtnl_link_is_vlan", "link=%p", (void *)link);
        if (!_is_vlan)
        {
            NL_CALL_RET(_nl_iter, nl_cache_get_next((struct nl_object *)link),
                        "nl_cache_get_next", "obj=%p", (void *)link);
            link = (struct rtnl_link *)_nl_iter;
            continue;
        }

        NL_CALL_RET(vlan_id, rtnl_link_vlan_get_id(link),
                    "rtnl_link_vlan_get_id", "link=%p", (void *)link);
        NL_CALL_RET(parent_idx, rtnl_link_get_link(link),
                    "rtnl_link_get_link", "link=%p", (void *)link);

        NL_CALL_RET(parent_link, rtnl_link_get(cache, parent_idx),
                    "rtnl_link_get", "cache=%p, idx=%d", (void *)cache, parent_idx);
        if (parent_link)
        {
            NL_CALL_RET(parent_name, rtnl_link_get_name(parent_link),
                        "rtnl_link_get_name", "link=%p", (void *)parent_link);
        }

        NL_CALL_RET(_vlan_flags, (uint32_t)rtnl_link_vlan_get_flags(link),
                    "rtnl_link_vlan_get_flags", "link=%p", (void *)link);
        NL_CALL_VOID(rtnl_link_vlan_flags2str((int)_vlan_flags, flags_buf, sizeof(flags_buf)),
                     "rtnl_link_vlan_flags2str",
                     "flags=%u, buf=%p, len=%zu",
                     _vlan_flags, (void *)flags_buf, sizeof(flags_buf));

        NL_CALL_RET(_name, rtnl_link_get_name(link),
                    "rtnl_link_get_name", "link=%p", (void *)link);

        printf("%-20s  %-20s  %-10d  %s\n",
               _name,
               parent_name,
               vlan_id,
               flags_buf[0] ? flags_buf : "none");

        if (parent_link)
        {
            NL_CALL_VOID(rtnl_link_put(parent_link),
                         "rtnl_link_put", "link=%p", (void *)parent_link);
            parent_link = NULL;
        }

        NL_CALL_RET(_nl_iter, nl_cache_get_next((struct nl_object *)link),
                    "nl_cache_get_next", "obj=%p", (void *)link);
        link = (struct rtnl_link *)_nl_iter;
    }

    NL_CALL_VOID(nl_cache_free(cache),
                 "nl_cache_free", "cache=%p", (void *)cache);
    NL_CALL_VOID(nl_socket_free(sock),
                 "nl_socket_free", "sock=%p", (void *)sock);
    return 0;
}

/*
 * cmd_set_vlan_on_interface - Add a VLAN sub-interface to a network interface
 *
 * Description:
 *   Creates a new kernel "vlan"-type link on top of the specified parent
 *   interface using the Netlink ROUTE API (RTM_NEWLINK). The new interface is
 *   named "<iface>.<ver>" (e.g. "Ethernet56.v2"). The `type` string (e.g.
 *   "l2-trunk") is stored as the link alias so that cmd_set_vlan() can later
 *   locate this interface by `ver`. A placeholder VLAN ID of 1 is used at
 *   creation time; the actual ID should be set afterwards with cmd_set_vlan().
 *
 * Input parameters:
 *   iface  - parent interface name (e.g. "Ethernet56"); must already exist
 *   type   - VLAN type string (e.g. "l2-trunk", "l2-access"); stored as alias
 *   ver    - VLAN version/name used as the new interface name suffix (e.g. "v2")
 *
 * Output:
 *   On success, prints:
 *     Created VLAN interface <iface>.<ver> (type=<type>) on <iface>
 *
 * Return value:
 *    0  - success
 *   -1  - iface, type, or ver is NULL
 *   -2  - failed to allocate netlink socket
 *   -3  - failed to connect netlink socket to NETLINK_ROUTE
 *   -4  - failed to allocate the link cache from the kernel
 *   -5  - parent interface not found in the cache
 *   -6  - failed to allocate the new VLAN link object
 *   -7  - RTM_NEWLINK failed (kernel error creating VLAN link)
 */
int cmd_set_vlan_on_interface(char* iface, char* type, char* ver)
{
    struct nl_sock *sock = NULL;
    struct nl_cache *cache = NULL;
    struct rtnl_link *parent = NULL;
    struct rtnl_link *vlan_link = NULL;
    char vlan_ifname[IFNAMSIZ];
    int err;
    int _nl_err;
    int _parent_idx;

    if (!iface || !type || !ver)
    {
        fprintf(stderr, "cmd_set_vlan_on_interface: iface, type, or ver is NULL\n");
        return -1;
    }

    NL_CALL_RET(sock, nl_socket_alloc(),
                "nl_socket_alloc", "");
    if (!sock)
    {
        fprintf(stderr, "cmd_set_vlan_on_interface: failed to allocate netlink socket\n");
        return -2;
    }

    NL_CALL_RET(_nl_err, nl_connect(sock, NETLINK_ROUTE),
                "nl_connect", "sock=%p, protocol=NETLINK_ROUTE", (void *)sock);
    if (_nl_err < 0)
    {
        fprintf(stderr, "cmd_set_vlan_on_interface: failed to connect netlink socket\n");
        NL_CALL_VOID(nl_socket_free(sock),
                     "nl_socket_free", "sock=%p", (void *)sock);
        return -3;
    }

    NL_CALL_RET(_nl_err, rtnl_link_alloc_cache(sock, AF_UNSPEC, &cache),
                "rtnl_link_alloc_cache",
                "sock=%p, family=AF_UNSPEC, cache=%p",
                (void *)sock, (void *)cache);
    if (_nl_err < 0)
    {
        fprintf(stderr, "cmd_set_vlan_on_interface: failed to allocate link cache\n");
        NL_CALL_VOID(nl_socket_free(sock),
                     "nl_socket_free", "sock=%p", (void *)sock);
        return -4;
    }

    NL_CALL_RET(parent, rtnl_link_get_by_name(cache, iface),
                "rtnl_link_get_by_name", "cache=%p, name=\"%s\"",
                (void *)cache, iface);
    if (!parent)
    {
        fprintf(stderr, "cmd_set_vlan_on_interface: interface '%s' not found\n", iface);
        NL_CALL_VOID(nl_cache_free(cache),
                     "nl_cache_free", "cache=%p", (void *)cache);
        NL_CALL_VOID(nl_socket_free(sock),
                     "nl_socket_free", "sock=%p", (void *)sock);
        return -5;
    }

    snprintf(vlan_ifname, sizeof(vlan_ifname), "%s.%s", iface, ver);

    /* rtnl_link_vlan_alloc() allocates a link pre-configured as type "vlan" */
    NL_CALL_RET(vlan_link, rtnl_link_vlan_alloc(),
                "rtnl_link_vlan_alloc", "");
    if (!vlan_link)
    {
        fprintf(stderr, "cmd_set_vlan_on_interface: failed to allocate VLAN link object\n");
        NL_CALL_VOID(rtnl_link_put(parent),
                     "rtnl_link_put", "link=%p", (void *)parent);
        NL_CALL_VOID(nl_cache_free(cache),
                     "nl_cache_free", "cache=%p", (void *)cache);
        NL_CALL_VOID(nl_socket_free(sock),
                     "nl_socket_free", "sock=%p", (void *)sock);
        return -6;
    }

    NL_CALL_VOID(rtnl_link_set_name(vlan_link, vlan_ifname),
                 "rtnl_link_set_name", "link=%p, name=\"%s\"",
                 (void *)vlan_link, vlan_ifname);

    NL_CALL_RET(_parent_idx, rtnl_link_get_ifindex(parent),
                "rtnl_link_get_ifindex", "link=%p", (void *)parent);
    NL_CALL_VOID(rtnl_link_set_link(vlan_link, _parent_idx),
                 "rtnl_link_set_link", "link=%p, idx=%d",
                 (void *)vlan_link, _parent_idx);

    NL_CALL_VOID(rtnl_link_set_ifalias(vlan_link, type),
                 "rtnl_link_set_ifalias", "link=%p, alias=\"%s\"",
                 (void *)vlan_link, type);

    /* Use placeholder VLAN ID 1; the real ID is set later by cmd_set_vlan() */
    NL_CALL_VOID(rtnl_link_vlan_set_id(vlan_link, 1),
                 "rtnl_link_vlan_set_id", "link=%p, id=1", (void *)vlan_link);

    NL_CALL_RET(err, rtnl_link_add(sock, vlan_link, NLM_F_CREATE),
                "rtnl_link_add",
                "sock=%p, link=%p, flags=NLM_F_CREATE",
                (void *)sock, (void *)vlan_link);
    if (err < 0)
    {
        const char *_errmsg;
        NL_CALL_RET(_errmsg, nl_geterror(err),
                    "nl_geterror", "err=%d", err);
        fprintf(stderr, "cmd_set_vlan_on_interface: failed to create VLAN interface %s: %s\n",
                vlan_ifname, _errmsg);
        NL_CALL_VOID(rtnl_link_put(vlan_link),
                     "rtnl_link_put", "link=%p", (void *)vlan_link);
        NL_CALL_VOID(rtnl_link_put(parent),
                     "rtnl_link_put", "link=%p", (void *)parent);
        NL_CALL_VOID(nl_cache_free(cache),
                     "nl_cache_free", "cache=%p", (void *)cache);
        NL_CALL_VOID(nl_socket_free(sock),
                     "nl_socket_free", "sock=%p", (void *)sock);
        return -7;
    }

    printf("Created VLAN interface %s (type=%s) on %s\n", vlan_ifname, type, iface);

    NL_CALL_VOID(rtnl_link_put(vlan_link),
                 "rtnl_link_put", "link=%p", (void *)vlan_link);
    NL_CALL_VOID(rtnl_link_put(parent),
                 "rtnl_link_put", "link=%p", (void *)parent);
    NL_CALL_VOID(nl_cache_free(cache),
                 "nl_cache_free", "cache=%p", (void *)cache);
    NL_CALL_VOID(nl_socket_free(sock),
                 "nl_socket_free", "sock=%p", (void *)sock);
    return 0;
}

/*
 * cmd_set_vlan - Set the VLAN ID on an existing VLAN interface
 *
 * Description:
 *   Searches the kernel link cache for VLAN interfaces that match the version
 *   identifier `ver`. Matching is performed in two ways:
 *     1. The link alias equals `ver` (set by cmd_set_vlan_on_interface).
 *     2. The interface name ends with ".<ver>" (e.g. "Ethernet56.v2" for ver="v2").
 *   For every matching VLAN interface, the VLAN ID is updated to the numeric
 *   value of `id` using RTM_SETLINK (rtnl_link_change). Prints the result for
 *   each interface updated.
 *
 * Input parameters:
 *   ver  - VLAN version/name string used to identify the VLAN (e.g. "v2");
 *          must match the alias or name suffix of an existing VLAN interface
 *   id   - VLAN ID as a decimal string (e.g. "2"); valid range: 1-4094
 *
 * Output:
 *   For each updated interface, prints:
 *     Set VLAN ID <id> on <name> (ver=<ver>)
 *
 * Return value:
 *    0  - success (at least one VLAN interface updated)
 *   -1  - ver or id is NULL, or id is outside the valid range 1-4094
 *   -2  - failed to allocate netlink socket
 *   -3  - failed to connect netlink socket to NETLINK_ROUTE
 *   -4  - failed to allocate the link cache from the kernel
 *   -5  - no VLAN interface matching `ver` was found
 *   -6  - one or more RTM_SETLINK operations failed (kernel error)
 */
int cmd_set_vlan(char* ver, char* id)
{
    struct nl_sock *sock = NULL;
    struct nl_cache *cache = NULL;
    struct rtnl_link *link = NULL;
    struct rtnl_link *change = NULL;
    struct nl_object *_nl_iter = NULL;
    int vlan_id;
    int found = 0;
    int ret_code = 0;
    int _nl_err;

    if (!ver || !id)
    {
        fprintf(stderr, "cmd_set_vlan: ver or id is NULL\n");
        return -1;
    }

    vlan_id = atoi(id);
    if (vlan_id < 1 || vlan_id > 4094)
    {
        fprintf(stderr, "cmd_set_vlan: VLAN ID '%s' is out of valid range 1-4094\n", id);
        return -1;
    }

    NL_CALL_RET(sock, nl_socket_alloc(),
                "nl_socket_alloc", "");
    if (!sock)
    {
        fprintf(stderr, "cmd_set_vlan: failed to allocate netlink socket\n");
        return -2;
    }

    NL_CALL_RET(_nl_err, nl_connect(sock, NETLINK_ROUTE),
                "nl_connect", "sock=%p, protocol=NETLINK_ROUTE", (void *)sock);
    if (_nl_err < 0)
    {
        fprintf(stderr, "cmd_set_vlan: failed to connect netlink socket\n");
        NL_CALL_VOID(nl_socket_free(sock),
                     "nl_socket_free", "sock=%p", (void *)sock);
        return -3;
    }

    NL_CALL_RET(_nl_err, rtnl_link_alloc_cache(sock, AF_UNSPEC, &cache),
                "rtnl_link_alloc_cache",
                "sock=%p, family=AF_UNSPEC, cache=%p",
                (void *)sock, (void *)cache);
    if (_nl_err < 0)
    {
        fprintf(stderr, "cmd_set_vlan: failed to allocate link cache\n");
        NL_CALL_VOID(nl_socket_free(sock),
                     "nl_socket_free", "sock=%p", (void *)sock);
        return -4;
    }

    NL_CALL_RET(_nl_iter, nl_cache_get_first(cache),
                "nl_cache_get_first", "cache=%p", (void *)cache);
    link = (struct rtnl_link *)_nl_iter;
    while (link != NULL)
    {
        const char *alias;
        const char *name;
        const char *dot;
        int alias_match = 0;
        int suffix_match = 0;
        int err;
        int _is_vlan;

        NL_CALL_RET(_is_vlan, rtnl_link_is_vlan(link),
                    "rtnl_link_is_vlan", "link=%p", (void *)link);
        if (!_is_vlan)
        {
            NL_CALL_RET(_nl_iter, nl_cache_get_next((struct nl_object *)link),
                        "nl_cache_get_next", "obj=%p", (void *)link);
            link = (struct rtnl_link *)_nl_iter;
            continue;
        }

        NL_CALL_RET(alias, rtnl_link_get_ifalias(link),
                    "rtnl_link_get_ifalias", "link=%p", (void *)link);
        NL_CALL_RET(name, rtnl_link_get_name(link),
                    "rtnl_link_get_name", "link=%p", (void *)link);

        if (alias && strcmp(alias, ver) == 0)
            alias_match = 1;

        if (name)
        {
            dot = strrchr(name, '.');
            if (dot && strcmp(dot + 1, ver) == 0)
                suffix_match = 1;
        }

        if (!alias_match && !suffix_match)
        {
            NL_CALL_RET(_nl_iter, nl_cache_get_next((struct nl_object *)link),
                        "nl_cache_get_next", "obj=%p", (void *)link);
            link = (struct rtnl_link *)_nl_iter;
            continue;
        }

        /* Build a minimal change object containing only the new VLAN ID */
        NL_CALL_RET(change, rtnl_link_vlan_alloc(),
                    "rtnl_link_vlan_alloc", "");
        if (!change)
        {
            fprintf(stderr, "cmd_set_vlan: failed to allocate change link for %s\n",
                    name ? name : "?");
            ret_code = -6;
            break;
        }

        NL_CALL_VOID(rtnl_link_vlan_set_id(change, vlan_id),
                     "rtnl_link_vlan_set_id", "link=%p, id=%d",
                     (void *)change, vlan_id);

        NL_CALL_RET(err, rtnl_link_change(sock, link, change, 0),
                    "rtnl_link_change",
                    "sock=%p, link=%p, change=%p, flags=0",
                    (void *)sock, (void *)link, (void *)change);
        if (err < 0)
        {
            const char *_errmsg;
            NL_CALL_RET(_errmsg, nl_geterror(err),
                        "nl_geterror", "err=%d", err);
            fprintf(stderr, "cmd_set_vlan: failed to set VLAN ID %d on %s: %s\n",
                    vlan_id, name ? name : "?", _errmsg);
            NL_CALL_VOID(rtnl_link_put(change),
                         "rtnl_link_put", "link=%p", (void *)change);
            ret_code = -6;
            NL_CALL_RET(_nl_iter, nl_cache_get_next((struct nl_object *)link),
                        "nl_cache_get_next", "obj=%p", (void *)link);
            link = (struct rtnl_link *)_nl_iter;
            continue;
        }

        printf("Set VLAN ID %d on %s (ver=%s)\n", vlan_id, name ? name : "?", ver);
        NL_CALL_VOID(rtnl_link_put(change),
                     "rtnl_link_put", "link=%p", (void *)change);
        found = 1;

        NL_CALL_RET(_nl_iter, nl_cache_get_next((struct nl_object *)link),
                    "nl_cache_get_next", "obj=%p", (void *)link);
        link = (struct rtnl_link *)_nl_iter;
    }

    if (!found && ret_code == 0)
    {
        fprintf(stderr, "cmd_set_vlan: no VLAN interface found matching ver='%s'\n", ver);
        ret_code = -5;
    }

    NL_CALL_VOID(nl_cache_free(cache),
                 "nl_cache_free", "cache=%p", (void *)cache);
    NL_CALL_VOID(nl_socket_free(sock),
                 "nl_socket_free", "sock=%p", (void *)sock);
    return ret_code;
}

int main()
{
    int server_fd, new_socket;
    struct sockaddr_in address;
    /* Bug fix: addrlen must be socklen_t, not int */
    socklen_t addrlen = sizeof(address);
    char buffer[BUFFER_SIZE];

    /* Bug fix: socket() returns -1 on failure, not 0; fd 0 is a valid descriptor */
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    /* Bug fix: SO_REUSEADDR must be set BEFORE bind(), not after listen() */
    if (set_reuseraddr(server_fd) < 0)
    {
        perror("set_reuseraddr");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0)
    {
        perror("bind failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 3) < 0)
    {
        perror("listen");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    if (set_nonblocking(server_fd) < 0)
    {
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    printf("Server is listening on port %d\n", PORT);

    while (1)
    {
        if ((new_socket = accept(server_fd, (struct sockaddr *)&address, &addrlen)) >= 0)
        {
            printf("New connection established\n");

            int bytes_read;
            while ((bytes_read = read(new_socket, buffer, BUFFER_SIZE - 1)) > 0)
            {
                /* Bug fix: was 'buffer[bytes_read] = '0'' — must be '\0' */
                buffer[bytes_read] = '\0';

                /* Strip trailing CR/LF so strcmp-based dispatch works correctly */
                int len = bytes_read;
                while (len > 0 && (buffer[len - 1] == '\n' || buffer[len - 1] == '\r'))
                {
                    buffer[--len] = '\0';
                }

                printf("Received command: %s\n", buffer);
                process_command(buffer);
            }

            close(new_socket);
            printf("Connection closed\n");
        }
        else
        {
            usleep(100000); /* 100 ms */
        }
    }

    close(server_fd);
    return 0;
}
