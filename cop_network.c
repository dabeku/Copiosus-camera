//
//  scr-network.c
//  
//
//  Created by gwen on 13/05/2017.
//
//

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h> // for close()

#include <arpa/inet.h>
#include <sys/socket.h>

#include "cop_network.h"
#include "cop_utility.h"

static int receive_udp_socket = -1;
static int receive_udp_broadcast_socket = -1;

broadcast_data* network_receive_udp_broadcast(int port) {
    cop_debug("[network_receive_udp_broadcast].");

    struct sockaddr_in addr, si_other;
    cop_debug("[network_receive_udp_broadcast] Creating socket.");
    receive_udp_broadcast_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (receive_udp_broadcast_socket == -1) {
        cop_error("[network_receive_udp_broadcast] Could not create socket.");
        return NULL;
    }
    int broadcast = 1;

    setsockopt(receive_udp_broadcast_socket, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof broadcast);

    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    cop_debug("[network_receive_udp_broadcast] Binding socket.");
    int result = bind(receive_udp_broadcast_socket, (struct sockaddr *)&addr, sizeof(addr));
    if (result == -1) {
        cop_error("[network_receive_udp_broadcast] Could not bind socket.");
        return NULL;
    }

    char* buffer = malloc(sizeof(char) * BUFFER_SIZE);
    memset(buffer, '\0', BUFFER_SIZE);
    unsigned slen=sizeof(addr);
    cop_debug("[network_receive_udp_broadcast] Receiving data.");
    recvfrom(receive_udp_broadcast_socket, buffer, BUFFER_SIZE, 0, (struct sockaddr *)&si_other, &slen);
    close(receive_udp_broadcast_socket);

    broadcast_data* data = malloc(sizeof(broadcast_data));
    data->buffer = buffer;
    data->src_ip = inet_ntoa(si_other.sin_addr);

    return data;
}

command_data* network_receive_udp(int listen_port) {
    cop_debug("[network_receive_udp].");

    struct sockaddr_in addr, si_other;
    receive_udp_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (receive_udp_socket == -1) {
        cop_error("[network_receive_udp] Could not create socket.");
        return NULL;
    }

    addr.sin_family = AF_INET;
    addr.sin_port = htons(listen_port);
    addr.sin_addr.s_addr = INADDR_ANY;

    cop_debug("[network_receive_udp] Bind to port %d.", listen_port);
    int result = bind(receive_udp_socket, (struct sockaddr *)&addr, sizeof(addr));
    if (result == -1) {
        cop_error("[network_receive_udp] Could not bind socket do %d.", listen_port);
        return NULL;
    }

    char* buffer = malloc(sizeof(char) * BUFFER_SIZE);
    memset(buffer, '\0', BUFFER_SIZE);
    unsigned slen=sizeof(addr);
    recvfrom(receive_udp_socket, buffer, BUFFER_SIZE, 0, (struct sockaddr *)&si_other, &slen);
    cop_debug("[network_receive_udp] Received: %s.", buffer);
    close(receive_udp_socket);

    char* token;

    command_data* data = malloc(sizeof(command_data));
    int i = 0;

    if (!contains(buffer, " ")) {
        data->cmd = buffer;
        return data;
    }

    while ((token = strsep(&buffer, " ")) != NULL) {
        cop_debug("Token: %s", token);
        if (i == 0) {
            data->cmd = token;
            i++;
            continue;
        }
        if (i == 1) {
            data->protocol = token;
            i++;
            continue;
        }
        if (i == 2) {
            data->ip = token;
            i++;
            continue;
        }
        if (i == 3) {
            data->port = str_to_int(token);
            i++;
            continue;
        }
        i++;
    }

    return data;
}

void network_send_udp(const void *data, size_t size, broadcast_data* broadcast_data) {
    cop_debug("[network_send_udp].");

    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) {
        cop_error("[network_send_udp] Could not create socket.");
        exit(-1);
    }
    
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(broadcast_data->src_ip);
    addr.sin_port = htons(PORT_LISTEN_SERVER);
    
    cop_debug("[network_send_udp] Send data with length: %zu.", size);
    
    int sizeLeftToSend = size;
    for (int i = 0; i < size; i+=BUFFER_SIZE) {
        
        int buffSizeToSend = BUFFER_SIZE;
        if (sizeLeftToSend < BUFFER_SIZE) {
            buffSizeToSend = sizeLeftToSend;
        }
        cop_debug("[network_send_udp] Send: %d bytes to %s:%d", buffSizeToSend, broadcast_data->src_ip, PORT_LISTEN_SERVER);
        data = data + i;
        
        int result = sendto(s, data, buffSizeToSend, 0, (struct sockaddr *)&addr, sizeof(addr));
        if (result < 0) {
            cop_error("[network_send_udp] Could not send data. Result: %d.", result);
            exit(-1);
        }
        sizeLeftToSend -= BUFFER_SIZE;
    }
}
