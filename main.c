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

int cmd_show_interfaces()
{
    int ret_code = -1;

    return ret_code;
}

int cmd_rename_interfaces(char* prefix, char* new_prefix)
{
    int ret_code = -1;

    return ret_code;
}

int cmd_show_vlan()
{
    int ret_code = -1;

    return ret_code;
}

int cmd_set_vlan_on_interface(char* iface, char* type, char* ver)
{
    int ret_code = -1;

    return ret_code;
}

int cmd_set_vlan(char* ver, char* id)
{
    int ret_code = -1;

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