//
//  scr-network.h
//  
//
//  Created by gwen on 13/05/2017.
//
//

#ifndef COP_NETWORK_H
#define COP_NETWORK_H

#include "cop_utility.h"
#include "cop_status_code.h"

// Sender receives incoming CONNECT requests on this port
#define PORT_COMMAND_CAMERA 6081
// Sender will send back to this server port
#define PORT_LISTEN_SERVER 6085

// The port the localhost proxy listens to
#define PORT_PROXY_LISTEN_CAM 6070
#define PORT_PROXY_LISTEN_MIC 6071
// The dummy port the proxy sends to if noone is connected
#define PORT_PROXY_DESTINATION_DUMMY_CAM 6075
#define PORT_PROXY_DESTINATION_DUMMY_MIC 6076

// The port the camera listens to for commands from client
#define PORT_LISTEN_COMMAND_TCP 6090

// Packet size is 1472 by default which is the limit for VPN packets
// If this makes problem you can set the pkt_size url parameter like
// in this example: "udp://192.168.0.24:1235?pkt_size=1472"
#define PROXY_SEND_BUFFER_SIZE_BYTES 1472
#define PROXY_BUFFER_SIZE_BYTES 1472

typedef struct system_config {
    const char* senderId;
    int width;
    int height;
    int has_video;
    int has_audio;
} system_config;

typedef struct client_data {
    char* src_ip;
    int src_port;
    int socket;
} client_data;

typedef struct command_data {
    char* cmd;
    // CONNECT
    char* protocol;
    char* ip;
    int port_cam;
    int port_mic;
    // DELETE
    char* file_name;
} command_data;

extern int state;
extern int quit;

command_data* network_receive_udp(int listen_port);

void network_send_state(const char* senderId);

// Close proxy related stuff
void proxy_close();
// Reset proxy to 127.0.0.1
void proxy_reset_cam();
void proxy_reset_mic();
// Connect proxy to remote client and port
void proxy_connect_cam(char* dest_ip, int dest_port);
void proxy_connect_mic(char* dest_ip, int dest_port);
// Close TCP server related stuff
void server_close();
void proxy_init_cam(char* dest_ip, int dest_port, const char* encryptionPwd);
void proxy_init_mic(char* dest_ip, int dest_port, const char* encryptionPwd);
int proxy_receive_udp_cam(void* arg);
int proxy_receive_udp_mic(void* arg);

char* get_video_file_name();
char* get_audio_file_name();

int network_receive_tcp(void* arg);

char* get_sendto_ip();

#endif /* scr_network_h */
