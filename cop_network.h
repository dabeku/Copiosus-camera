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

// The port the camera listens to for commands from client
#define PORT_LISTEN_COMMAND_TCP 6090

// Packet size is 1472 by default which is the limit for VPN packets
// If this makes problem you can set the pkt_size url parameter like
// in this example: "udp://192.168.0.24:1235?pkt_size=1472".
// We use 188 * 7 = 1316 since mpegts packets are 188 bytes long.
#define PROXY_SEND_BUFFER_SIZE_BYTES 1316
#define PROXY_BUFFER_SIZE_BYTES 1316

typedef struct system_config {
    const char* senderId;
    int width;
    int height;
    int has_video;
    int has_audio;
} system_config;

typedef struct command_data {
    char* cmd;
    // CONNECT
    char* protocol;
    char* ip;
    int port_cam;
    int port_mic;
    // DELETE
    char* file_name;
    // RESET
    char* reset_ip;
    // STOP
    char* stop_ip;
    // START
    char* start_ip;
} command_data;

typedef void (*callback)();
typedef void (*callback_cd)(command_data* command_data);

typedef struct container_config {
    system_config* system_config;
    callback_cd cb_start;
    callback_cd cb_connect;
    callback_cd cb_stop;
    callback_cd cb_delete;
    callback_cd cb_reset;
} container_config;

typedef struct client_data {
    char* src_ip;
    int src_port;
    int socket;
} client_data;

extern int state;
extern int quit;

void network_init();

void network_send_state(const char* senderId, char* incl_ip);

// Close proxy related stuff
void proxy_close();
// Reset proxy to 127.0.0.1
void proxy_remove_all_clients_cam();
void proxy_remove_all_clients_mic();
void proxy_remove_client_cam(char* reset_ip);
void proxy_remove_client_mic(char* reset_ip);
// Connect proxy to remote client and port
void proxy_connect_cam(char* dest_ip, int dest_port);
void proxy_connect_mic(char* dest_ip, int dest_port);
// Close TCP server related stuff
void server_close();
void proxy_init_cam(const char* encryptionPwd);
void proxy_init_mic(const char* encryptionPwd);
int proxy_receive_udp_cam(void* arg);
int proxy_receive_udp_mic(void* arg);

char* get_video_file_name();
char* get_audio_file_name();

int network_receive_tcp(void* arg);

char* get_sendto_ip();

#endif /* scr_network_h */
