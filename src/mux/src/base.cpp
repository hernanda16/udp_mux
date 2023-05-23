#include <ros/ros.h>
#include <ros/package.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <pthread.h>
#include <errno.h>
#include <fstream>
#include <sys/time.h>
#include <unistd.h>
#include <sstream>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/if.h>
#include <unistd.h>
#include <linux/if_ether.h>
#include "yaml-cpp/yaml.h"
#include <chrono>

#define PERR(txt, par...) \
    printf("ERROR: (%s / %s): " txt "\n", __FILE__, __FUNCTION__, ##par)
#define PERRNO(txt) \
    printf("ERROR: (%s / %s): " txt ": %s\n", __FILE__, __FUNCTION__, strerror(errno))

typedef struct multiSocket_tag
{
    struct sockaddr_in destAddress;
    int socketID;
    bool compressedData;
} multiSocket_t;
typedef struct nw_config
{
    char multicast_ip[16];
    char iface[10];
    char identifier[1];
    unsigned int port;
    uint8_t compress_type;
} config;
multiSocket_t multiSocket;
multiSocket_t *recv_socket;
struct sockaddr src_addr;
socklen_t addr_len = sizeof(src_addr);

//--Prototypes
ros::Timer Timer_60Hz;
void Callback(const ros::TimerEvent &);

//--Data types


int if_NameToIndex(char *ifname, char *address)
{
    int fd;
    struct ifreq if_info;
    int if_index;

    memset(&if_info, 0, sizeof(if_info));
    strncpy(if_info.ifr_name, ifname, IFNAMSIZ - 1);

    if ((fd = socket(AF_INET, SOCK_DGRAM, 0)) == -1)
    {
        PERRNO("socket");
        return -1;
    }
    if (ioctl(fd, SIOCGIFINDEX, &if_info) == -1)
    {
        PERRNO("ioctl");
        close(fd);
        return -1;
    }
    if_index = if_info.ifr_ifindex;

    if (ioctl(fd, SIOCGIFADDR, &if_info) == -1)
    {
        PERRNO("ioctl");
        close(fd);
        return -1;
    }

    close(fd);

    sprintf(address, "%d.%d.%d.%d\n",
            (int)((unsigned char *)if_info.ifr_hwaddr.sa_data)[2],
            (int)((unsigned char *)if_info.ifr_hwaddr.sa_data)[3],
            (int)((unsigned char *)if_info.ifr_hwaddr.sa_data)[4],
            (int)((unsigned char *)if_info.ifr_hwaddr.sa_data)[5]);
#ifdef COMM_DEBUG
    printf("**** Using device %s -> %s\n", if_info.ifr_name, address);
#endif

    return if_index;
}

void closeSocket()
{
    if (multiSocket.socketID != -1)
        shutdown(multiSocket.socketID, SHUT_RDWR);
}
int openSocket()
{
    struct sockaddr_in multicastAddress;
    struct ip_mreqn mreqn;
    struct ip_mreq mreq;
    int opt;
    char address[16]; // IPV4: xxx.xxx.xxx.xxx/0

    /* It use to receive data */
    bzero(&multicastAddress, sizeof(struct sockaddr_in));
    multicastAddress.sin_family = AF_INET;
    multicastAddress.sin_port = htons(1026);
    multicastAddress.sin_addr.s_addr = INADDR_ANY;

    /* It use to send data */
    bzero(&multiSocket.destAddress, sizeof(struct sockaddr_in));
    multiSocket.destAddress.sin_family = AF_INET;
    multiSocket.destAddress.sin_port = htons(1026);
    multiSocket.destAddress.sin_addr.s_addr = inet_addr("224.16.32.80");

    /* Set socket as Datagram (UDP) */
    if ((multiSocket.socketID = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1)
    {
        PERRNO("socket");
        return -1;
    }

    /* Assign WiFi Interface to use multicast */
    memset((void *)&mreqn, 0, sizeof(mreqn));
    mreqn.imr_ifindex = if_NameToIndex("wlp3s0", address);
    if ((setsockopt(multiSocket.socketID, SOL_IP, IP_MULTICAST_IF, &mreqn, sizeof(mreqn))) == -1)
    {
        PERRNO("setsockopt 1");
        return -1;
    }

    /* It allow to use more than one process that bind on the same UPD socket */
    opt = 1;
    if ((setsockopt(multiSocket.socketID, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) == -1)
    {
        PERRNO("setsockopt 2");
        return -1;
    }

    memset((void *)&mreq, 0, sizeof(mreq));
    mreq.imr_multiaddr.s_addr = inet_addr("224.16.32.80");
    mreq.imr_interface.s_addr = inet_addr(address);

    /* Assign socket to multicast IP, so he will collect all message on them */
    if ((setsockopt(multiSocket.socketID, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq))) == -1)
    {
        PERRNO("setsockopt 3");
        return -1;
    }

    /**
     * 0 for Disable reception of our own multicast
     * 1 for Receive our data
     * */
    opt = 0;
    if ((setsockopt(multiSocket.socketID, IPPROTO_IP, IP_MULTICAST_LOOP, &opt, sizeof(opt))) == -1)
    {
        PERRNO("setsockopt");
        return -1;
    }

    /* Bind to socket, so i can receive data  */
    if (bind(multiSocket.socketID, (struct sockaddr *)&multicastAddress, sizeof(struct sockaddr_in)) == -1)
    {
        PERRNO("bind");
        return -1;
    }

    return 0;
}

void Callback(const ros::TimerEvent &)
{
    char send_buffer[128];
    char recv_buf[128];
    char header[4]="its";
    int8_t id_user='0';
    int8_t id_target[2]={'1', '2'};
    int8_t data;

    /* Use MSG_DONTWAIT because of non-blocking */
    int8_t nrecv = recvfrom(multiSocket.socketID, recv_buf, 64, MSG_DONTWAIT, &src_addr, &addr_len);

    if (nrecv > 0 && recv_buf[4] == '2')
    {
        memcpy(&data,recv_buf+5,1);

        printf("%d\n", data);

        data = 69;
        memcpy(send_buffer,header,3);
        memcpy(send_buffer+3,&id_user,1);
        memcpy(send_buffer+4,&id_target[0],1);
        memcpy(send_buffer+5,&id_target[1],1);
        memcpy(send_buffer+6,&data,1);

        int nsend = sendto(multiSocket.socketID, send_buffer, sizeof(send_buffer), 0, (struct sockaddr *)&multiSocket.destAddress, sizeof(struct sockaddr));
    }else{
        data = 69;
        memcpy(send_buffer,header,3);
        memcpy(send_buffer+3,&id_user,1);
        memcpy(send_buffer+4,&id_target[0],1);
        memcpy(send_buffer+5,&id_target[1],1);
        memcpy(send_buffer+6,&data,1);

        int nsend = sendto(multiSocket.socketID, send_buffer, sizeof(send_buffer), 0, (struct sockaddr *)&multiSocket.destAddress, sizeof(struct sockaddr));
    }
}

int main(int argc, char **argv)
{
    ros::init(argc, argv, "base");
    ros::NodeHandle NH;
    ros::MultiThreadedSpinner spinner(0);

    closeSocket();
    if (openSocket() == -1)
    {
        PERR("openMulticastSocket");
        return -1;
    }

    Timer_60Hz = NH.createTimer(ros::Duration(0.01666666666), Callback);

    spinner.spin();
    Timer_60Hz.stop();

    return 0;
}