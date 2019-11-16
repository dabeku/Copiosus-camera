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

// The port the localhost proxy listens to
#define PORT_PROXY_LISTEN 6070
// The dummy port the proxy sends to if noone is connected
#define PORT_PROXY_DESTINATION_DUMMY 6075

// The port the tcp server listens to for downloading a file in network_receive_tcp()
#define PORT_LISTEN_TCP_DOWNLOAD 6090

// Packet size is 1472 by default which is the limit for VPN packets
// If this makes problem you can set the pkt_size url parameter like
// in this example: "udp://192.168.0.24:1235?pkt_size=1472"
#define PROXY_SEND_BUFFER_SIZE_BYTES 1472
#define PROXY_BUFFER_SIZE_BYTES 1472

typedef struct broadcast_data {
    char* src_ip;
    char* buffer;
} broadcast_data;

typedef struct command_data {
    char* cmd;
    // CONNECT
    char* protocol;
    char* ip;
    int port;
    // DELETE
    char* file_name;

} command_data;

broadcast_data* network_receive_udp_broadcast(int port);
void network_send_udp(const void *data, size_t size, broadcast_data* broadcast_data);
command_data* network_receive_udp(int listen_port);

// Close proxy related stuff
void proxy_close();
// Reset proxy to 127.0.0.1
void proxy_reset();
// Close TCP server related stuff
void server_close();
void proxy_init(const char* dest_ip, int dest_port, const char* encryptionPwd);
int proxy_receive_udp(void* arg);
char* get_video_file_name();

int network_receive_tcp(void* arg);

char* get_sendto_ip();

#endif /* scr_network_h */
