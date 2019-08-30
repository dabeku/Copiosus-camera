//
//  scr-network.h
//  
//
//  Created by gwen on 13/05/2017.
//
//

#ifndef COP_NETWORK_H
#define COP_NETWORK_H

// Sender listens to this UDP broadcast port for SCAN message
#define PORT_SCAN_BROADCAST 6080
// Sender receives incoming CONNECT requests on this port
#define PORT_COMMAND_CAMERA 6081
// Sender will send back to this server port
#define PORT_LISTEN_SERVER 6085

typedef struct broadcast_data {
    char* src_ip;
    char* buffer;
} broadcast_data;

typedef struct command_data {
    char* cmd;
    char* protocol;
    char* ip;
    int port;
} command_data;

broadcast_data* network_receive_udp_broadcast(int port);
void network_send_udp(const void *data, size_t size, broadcast_data* broadcast_data);
command_data* network_receive_udp(int listen_port);

#endif /* scr_network_h */
