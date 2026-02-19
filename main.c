#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/netlink.h>
#include <linux/if.h>
#include <linux/if_ether.h>
#include <linux/if_vlan.h>
//#include <net/if.h>
#include <netinet/in.h>
#include <linux/netlink.h>
#include <netlink/socket.h>
#include <netlink/netlink.h>
#include <netlink/route/link.h>
#include <netlink/route/link/vlan.h>
#include <sys/socket.h>
#include <sys/types.h>

#define PORT 8888
#define BUFFER_SIZE 1024

int get_words(const char* str, char** words, int* cnt);
int cmd_show_interfaces();
int cmd_rename_interfaces(char* prefix, char* new_prefix);
int cmd_show_vlan();
int cmd_set_vlan_on_interface(char* iface, char* type, char* ver);
int cmd_set_vlan(char* ver, char* id);

int get_words(const char* str, char** words, int* cnt)
{
    int ret_code = -1;
    int length = 0;
    int in_word = 0;
    int word_index = 0;

    if (!str)
    {
        printf("str is NULL\n");
        return -1;
    }

    if(!cnt)
    {
        printf("cnt is NULL\n");
        return -2;
    }

    *cnt = 0;

    if(words)
    {
        printf("words is not NULL\n");
        free(words);
        words = NULL;
    }

    length = strlen(str);
    words = malloc(length * sizeof(char*));

    if (words == NULL)
    {
        perror("Failed to allocate memory for words");
        return -3;
    }

    char* word = malloc(length + 1);

    if (word == NULL)
    {
        perror("Failed to allocate memory for word");
        free(words);
        return -4;
    }

    word_index = 0;
    in_word = 0;
    for (int i = 0; i <= length; i++)
    {
        if (str[i] != ' ' && str[i] != '0')
        {
            word[word_index++] = str[i];
            in_word = 1;
        }
        else
        {
            if (in_word)
            {
                word[word_index] = '0';
                words[*cnt] = malloc((word_index + 1) * sizeof(char));
                if (words[*cnt] == NULL) {
                    perror("Failed to allocate memory for a word");
                    for (int j = 0; j < *cnt; j++)
                    {
                        free(words[j]);
                    }
                    free(words);
                    free(word);
                    return -5;
                }
                strcpy(words[*cnt], word);
                (*cnt)++;
                word_index = 0;
            }
            in_word = 0;
        }
    }

    free(word);

    return 0;
}

void create_vlan(const char *iface_name, int vlan_id)
{
    struct nl_sock *sock;
    struct nl_msg *msg;
    struct nlmsghdr *nlh;
    struct ifinfomsg *ifi;
    char vlan_ifname[IFNAMSIZ];

    sock = nl_socket_alloc();
    if (!sock)
    {
        perror("nl_socket_alloc");
        exit(EXIT_FAILURE);
    }

    if (nl_connect(sock, NETLINK_ROUTE) < 0)
    {
        perror("nl_connect");
        nl_socket_free(sock);
        exit(EXIT_FAILURE);
    }

    snprintf(vlan_ifname, sizeof(vlan_ifname), "%s.%d", iface_name, vlan_id);

    msg = nlmsg_alloc();
    if (!msg)
    {
        perror("nlmsg_alloc");
        nl_socket_free(sock);
        exit(EXIT_FAILURE);
    }

    nlh = nlmsg_put(msg, NL_AUTO_PORT, NL_AUTO_SEQ, RTM_NEWLINK, sizeof(struct ifinfomsg), NLM_F_CREATE | NLM_F_EXCL);
    
    ifi = (struct ifinfomsg *)NLMSG_DATA(nlh);
    ifi->ifi_family = AF_PACKET;
    ifi->ifi_change = IFF_UP;

    if (nla_put_string(msg, IFLA_IFNAME, vlan_ifname) ||
        nla_put_u32(msg, IFLA_VLAN_ID, vlan_id))
    {
        perror("nla_put");
        nlmsg_free(msg);
        nl_socket_free(sock);
        exit(EXIT_FAILURE);
    }

    if (nl_send_auto(sock, msg) < 0)
    {
        perror("nl_send_auto");
        nlmsg_free(msg);
        nl_socket_free(sock);
        exit(EXIT_FAILURE);
    }

    nlmsg_free(msg);
    nl_socket_free(sock);

    printf("VLAN %d created on interface %s\n", vlan_id, vlan_ifname);
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

    if (get_words(cmd, cmd_words, &cmd_words_cnt) < 0)
    {
        printf("Bad format command string: %s\n", cmd);
        return;
    }

    // show interfaces
    if (strcmp(cmd, "show interfaces") == 0)
    {
        printf("Executing: %s\n", cmd);
        cmd_show_interfaces();  
    }
    // rename interfaces
    else if (strcmp(cmd, "rename interfaces") == 0)
    {
        printf("Executing: %s\n", cmd);
        if (cmd_words_cnt != 4)
        {
            printf("Bad format command: %s\n", cmd);
            return;
        }
        cmd_rename_interfaces(cmd_words[2], cmd_words[3]);
    }
    // show vlan
    else if (strcmp(cmd, "show vlan") == 0)
    {
        printf("Executing: %s\n", cmd);
        cmd_show_vlan();
    }
    // set interface Ethernet56 type l2-trunk vlan v2
    else if (strncmp(cmd, "set interface ", 15) == 0)
    {
        printf("Executing: %s\n", cmd);
        if (cmd_words_cnt != 7)
        {
            printf("Bad format command: %s\n", cmd);
            return;
        }
        cmd_set_vlan_on_interface(cmd_words[2], cmd_words[4], cmd_words[6]);
    }
    // set vlan v2 id 2
    else if (strncmp(cmd, "set vlan ", 9) == 0)
    {
        printf("Executing: %s\n", cmd);
        if (cmd_words_cnt != 5)
        {
            printf("Bad format command: %s\n", cmd);
            return;
        }
        cmd_set_vlan(cmd_words[2], cmd_words[4]);  
    }
    // default
    else
    {
        printf("Unknown command: %s\n", cmd);
    }
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

    sock = nl_socket_alloc();
    if (!sock)
    {
        fprintf(stderr, "cmd_show_interfaces: failed to allocate netlink socket\n");
        return -1;
    }

    if (nl_connect(sock, NETLINK_ROUTE) < 0)
    {
        fprintf(stderr, "cmd_show_interfaces: failed to connect netlink socket\n");
        nl_socket_free(sock);
        return -2;
    }

    if (rtnl_link_alloc_cache(sock, AF_UNSPEC, &cache) < 0)
    {
        fprintf(stderr, "cmd_show_interfaces: failed to allocate link cache\n");
        nl_socket_free(sock);
        return -3;
    }

    printf("%-5s  %-20s  %-12s  %s\n", "IDX", "NAME", "TYPE", "FLAGS");
    printf("%-5s  %-20s  %-12s  %s\n", "---", "----", "----", "-----");

    for (link = (struct rtnl_link *)nl_cache_get_first(cache);
         link != NULL;
         link = (struct rtnl_link *)nl_cache_get_next((struct nl_object *)link))
    {
        char flags_buf[256] = {0};
        const char *type = rtnl_link_get_type(link);

        rtnl_link_flags2str(rtnl_link_get_flags(link), flags_buf, sizeof(flags_buf));

        printf("%-5d  %-20s  %-12s  %s\n",
               rtnl_link_get_ifindex(link),
               rtnl_link_get_name(link),
               type ? type : "-",
               flags_buf[0] ? flags_buf : "none");
    }

    nl_cache_free(cache);
    nl_socket_free(sock);
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
    int ret_code = 0;
    size_t prefix_len;

    if (!prefix || !new_prefix)
    {
        fprintf(stderr, "cmd_rename_interfaces: prefix or new_prefix is NULL\n");
        return -1;
    }

    sock = nl_socket_alloc();
    if (!sock)
    {
        fprintf(stderr, "cmd_rename_interfaces: failed to allocate netlink socket\n");
        return -2;
    }

    if (nl_connect(sock, NETLINK_ROUTE) < 0)
    {
        fprintf(stderr, "cmd_rename_interfaces: failed to connect netlink socket\n");
        nl_socket_free(sock);
        return -3;
    }

    if (rtnl_link_alloc_cache(sock, AF_UNSPEC, &cache) < 0)
    {
        fprintf(stderr, "cmd_rename_interfaces: failed to allocate link cache\n");
        nl_socket_free(sock);
        return -4;
    }

    prefix_len = strlen(prefix);

    for (link = (struct rtnl_link *)nl_cache_get_first(cache);
         link != NULL;
         link = (struct rtnl_link *)nl_cache_get_next((struct nl_object *)link))
    {
        const char *name = rtnl_link_get_name(link);
        char new_name[IFNAMSIZ];
        int err;

        if (!name || strncmp(name, prefix, prefix_len) != 0)
            continue;

        snprintf(new_name, sizeof(new_name), "%s%s", new_prefix, name + prefix_len);

        change = rtnl_link_alloc();
        if (!change)
        {
            fprintf(stderr, "cmd_rename_interfaces: failed to allocate change link for %s\n", name);
            ret_code = -5;
            break;
        }

        rtnl_link_set_name(change, new_name);

        err = rtnl_link_change(sock, link, change, 0);
        if (err < 0)
        {
            fprintf(stderr, "cmd_rename_interfaces: rename %s -> %s failed: %s\n",
                    name, new_name, nl_geterror(err));
            rtnl_link_put(change);
            ret_code = -5;
            continue;
        }

        printf("%s -> %s\n", name, new_name);
        rtnl_link_put(change);
    }

    nl_cache_free(cache);
    nl_socket_free(sock);
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

    sock = nl_socket_alloc();
    if (!sock)
    {
        fprintf(stderr, "cmd_show_vlan: failed to allocate netlink socket\n");
        return -1;
    }

    if (nl_connect(sock, NETLINK_ROUTE) < 0)
    {
        fprintf(stderr, "cmd_show_vlan: failed to connect netlink socket\n");
        nl_socket_free(sock);
        return -2;
    }

    if (rtnl_link_alloc_cache(sock, AF_UNSPEC, &cache) < 0)
    {
        fprintf(stderr, "cmd_show_vlan: failed to allocate link cache\n");
        nl_socket_free(sock);
        return -3;
    }

    printf("%-20s  %-20s  %-10s  %s\n", "NAME", "PARENT", "VLAN_ID", "FLAGS");
    printf("%-20s  %-20s  %-10s  %s\n", "----", "------", "-------", "-----");

    for (link = (struct rtnl_link *)nl_cache_get_first(cache);
         link != NULL;
         link = (struct rtnl_link *)nl_cache_get_next((struct nl_object *)link))
    {
        char flags_buf[128] = {0};
        const char *parent_name = "-";
        int parent_idx;
        int vlan_id;

        if (!rtnl_link_is_vlan(link))
            continue;

        vlan_id    = rtnl_link_vlan_get_id(link);
        parent_idx = rtnl_link_get_link(link);

        parent_link = rtnl_link_get(cache, parent_idx);
        if (parent_link)
            parent_name = rtnl_link_get_name(parent_link);

        rtnl_link_vlan_flags2str((int)rtnl_link_vlan_get_flags(link),
                                 flags_buf, sizeof(flags_buf));

        printf("%-20s  %-20s  %-10d  %s\n",
               rtnl_link_get_name(link),
               parent_name,
               vlan_id,
               flags_buf[0] ? flags_buf : "none");

        if (parent_link)
        {
            rtnl_link_put(parent_link);
            parent_link = NULL;
        }
    }

    nl_cache_free(cache);
    nl_socket_free(sock);
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

    if (!iface || !type || !ver)
    {
        fprintf(stderr, "cmd_set_vlan_on_interface: iface, type, or ver is NULL\n");
        return -1;
    }

    sock = nl_socket_alloc();
    if (!sock)
    {
        fprintf(stderr, "cmd_set_vlan_on_interface: failed to allocate netlink socket\n");
        return -2;
    }

    if (nl_connect(sock, NETLINK_ROUTE) < 0)
    {
        fprintf(stderr, "cmd_set_vlan_on_interface: failed to connect netlink socket\n");
        nl_socket_free(sock);
        return -3;
    }

    if (rtnl_link_alloc_cache(sock, AF_UNSPEC, &cache) < 0)
    {
        fprintf(stderr, "cmd_set_vlan_on_interface: failed to allocate link cache\n");
        nl_socket_free(sock);
        return -4;
    }

    parent = rtnl_link_get_by_name(cache, iface);
    if (!parent)
    {
        fprintf(stderr, "cmd_set_vlan_on_interface: interface '%s' not found\n", iface);
        nl_cache_free(cache);
        nl_socket_free(sock);
        return -5;
    }

    snprintf(vlan_ifname, sizeof(vlan_ifname), "%s.%s", iface, ver);

    /* rtnl_link_vlan_alloc() allocates a link pre-configured as type "vlan" */
    vlan_link = rtnl_link_vlan_alloc();
    if (!vlan_link)
    {
        fprintf(stderr, "cmd_set_vlan_on_interface: failed to allocate VLAN link object\n");
        rtnl_link_put(parent);
        nl_cache_free(cache);
        nl_socket_free(sock);
        return -6;
    }

    rtnl_link_set_name(vlan_link, vlan_ifname);
    rtnl_link_set_link(vlan_link, rtnl_link_get_ifindex(parent));
    rtnl_link_set_ifalias(vlan_link, type);
    /* Use placeholder VLAN ID 1; the real ID is set later by cmd_set_vlan() */
    rtnl_link_vlan_set_id(vlan_link, 1);

    err = rtnl_link_add(sock, vlan_link, NLM_F_CREATE);
    if (err < 0)
    {
        fprintf(stderr, "cmd_set_vlan_on_interface: failed to create VLAN interface %s: %s\n",
                vlan_ifname, nl_geterror(err));
        rtnl_link_put(vlan_link);
        rtnl_link_put(parent);
        nl_cache_free(cache);
        nl_socket_free(sock);
        return -7;
    }

    printf("Created VLAN interface %s (type=%s) on %s\n", vlan_ifname, type, iface);

    rtnl_link_put(vlan_link);
    rtnl_link_put(parent);
    nl_cache_free(cache);
    nl_socket_free(sock);
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
    int vlan_id;
    int found = 0;
    int ret_code = 0;

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

    sock = nl_socket_alloc();
    if (!sock)
    {
        fprintf(stderr, "cmd_set_vlan: failed to allocate netlink socket\n");
        return -2;
    }

    if (nl_connect(sock, NETLINK_ROUTE) < 0)
    {
        fprintf(stderr, "cmd_set_vlan: failed to connect netlink socket\n");
        nl_socket_free(sock);
        return -3;
    }

    if (rtnl_link_alloc_cache(sock, AF_UNSPEC, &cache) < 0)
    {
        fprintf(stderr, "cmd_set_vlan: failed to allocate link cache\n");
        nl_socket_free(sock);
        return -4;
    }

    for (link = (struct rtnl_link *)nl_cache_get_first(cache);
         link != NULL;
         link = (struct rtnl_link *)nl_cache_get_next((struct nl_object *)link))
    {
        const char *alias;
        const char *name;
        const char *dot;
        int alias_match = 0;
        int suffix_match = 0;
        int err;

        if (!rtnl_link_is_vlan(link))
            continue;

        alias = rtnl_link_get_ifalias(link);
        name  = rtnl_link_get_name(link);

        if (alias && strcmp(alias, ver) == 0)
            alias_match = 1;

        if (name)
        {
            dot = strrchr(name, '.');
            if (dot && strcmp(dot + 1, ver) == 0)
                suffix_match = 1;
        }

        if (!alias_match && !suffix_match)
            continue;

        /* Build a minimal change object containing only the new VLAN ID */
        change = rtnl_link_vlan_alloc();
        if (!change)
        {
            fprintf(stderr, "cmd_set_vlan: failed to allocate change link for %s\n",
                    name ? name : "?");
            ret_code = -6;
            break;
        }

        rtnl_link_vlan_set_id(change, vlan_id);

        err = rtnl_link_change(sock, link, change, 0);
        if (err < 0)
        {
            fprintf(stderr, "cmd_set_vlan: failed to set VLAN ID %d on %s: %s\n",
                    vlan_id, name ? name : "?", nl_geterror(err));
            rtnl_link_put(change);
            ret_code = -6;
            continue;
        }

        printf("Set VLAN ID %d on %s (ver=%s)\n", vlan_id, name ? name : "?", ver);
        rtnl_link_put(change);
        found = 1;
    }

    if (!found && ret_code == 0)
    {
        fprintf(stderr, "cmd_set_vlan: no VLAN interface found matching ver='%s'\n", ver);
        ret_code = -5;
    }

    nl_cache_free(cache);
    nl_socket_free(sock);
    return ret_code;
}

int main()
{
    int server_fd, new_socket;
    struct sockaddr_in address;
    int addrlen = sizeof(address);
    char buffer[BUFFER_SIZE];

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0)
    {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0)
    {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 3) < 0)
    {
        perror("listen");
        exit(EXIT_FAILURE);
    }

    if (set_reuseraddr(server_fd) < 0)
    {
        exit(EXIT_FAILURE);
    }

    if (set_nonblocking(server_fd) < 0)
    {
        exit(EXIT_FAILURE);
    }

    printf("Server is listening on port %d\n", PORT);

    while (1)
    {
        if ((new_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen)) >= 0)
        {
            printf("New connection established\n");

            int bytes_read;
            while ((bytes_read = read(new_socket, buffer, BUFFER_SIZE - 1)) > 0)
            {
                buffer[bytes_read] = '0';
                printf("Received command: %s\n", buffer);
                process_command(buffer);
            }

            close(new_socket);
            printf("Connection closed\n");
        }
        else
        {
            usleep(100000); // 100 ms
        }
    }

    close(server_fd);
    return 0;
}