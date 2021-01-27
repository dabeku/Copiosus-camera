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
#include <signal.h> // for raise()

#include "cop_network.h"

static int receive_tcp_socket = -1;

static int proxy_receive_udp_socket_cam = -1;
static int proxy_receive_udp_socket_mic = -1;
static bool is_network_running_cam = false;
static bool is_network_running_mic = false;
static FILE* file_cam = NULL;
static char* file_cam_name = NULL;
static FILE* file_mic = NULL;
static char* file_mic_name = NULL;

// Set by main() args
static const char* encryption_pwd_cam = NULL;
static const char* encryption_pwd_mic = NULL;

struct list_item* client_data_cam_list = NULL;
struct list_item* client_data_mic_list = NULL;

typedef struct FileItem {
    char* file_name;
    long file_size_kb;
} FileItem;

static list_item* list_clone(list_item* list) {
    struct list_item* clone = NULL;
    for (int i = 0; i < list_length(list); i++) {
        list_item* item = list_get(list, i);
        client_data* data = (client_data*)item->data;
        client_data* new_data = malloc(sizeof(client_data));
        new_data->src_ip = strdup(data->src_ip);
        new_data->src_port = data->src_port;
        new_data->socket = data->socket;
        clone = list_push(clone, new_data);
    }
    return clone;
}

static void list_clear(list_item* list) {
    for (int i = 0; i < list_length(list); i++) {
        list_item* item = list_get(list, 0);
        client_data* data = (client_data*)item->data;
        char* temp = data->src_ip;
        list = list_delete(list, 0);
        free(temp);
        free(data);
    }
}

static void network_send_tcp(const void *data, size_t size, char* dst_ip) {

    cop_debug("[network_send_tcp] Send data to %s with length: %zu.", dst_ip, size);

    int send_tcp_socket; 
    struct sockaddr_in serv_addr; 
  
    // socket create and varification 
    send_tcp_socket = socket(AF_INET, SOCK_STREAM, 0); 
    if (send_tcp_socket < 0) {
        cop_error("[network_send_tcp] Could not create socket.");
        return;
    }

    cop_debug("[network_send_tcp] Socket successfully created.");
    bzero(&serv_addr, sizeof(serv_addr)); 
  
    // assign IP, PORT 
    serv_addr.sin_family = AF_INET; 
    serv_addr.sin_addr.s_addr = inet_addr(dst_ip); 
    serv_addr.sin_port = htons(PORT_LISTEN_SERVER); 
  
    // connect the client socket to server socket 
    if (connect(send_tcp_socket, (struct sockaddr*) &serv_addr, sizeof(serv_addr)) != 0) { 
        cop_error("[network_send_tcp] Connection with the server failed."); 
        return;
    }
  
    // function for chat 
    int result = send(send_tcp_socket, data, size, 0);
    if (result < 0) {
        cop_error("[network_send_tcp] Send failed: %d.", result);
        return;
    }

    close(send_tcp_socket); 
}

/*
 * 0 = IDLE
 * 1 = INITIALIZING
 * 2 = CONNECTED;ip-send-to
 * 3 = DISCONNECTING
 * x = UNKNOWN
 */
static char* get_state_str() {
    if (state == 0) {
        return concat("IDLE", "");
    } else if (state == 1) {
        return concat("INITIALIZING", "");
    } else if (state == 2) {
        const char* ipSendTo = get_sendto_ip();
        return concat("CONNECTED;", ipSendTo);
    } else if (state == 3) {
        return concat("DISCONNECTING", "");
    }
    return concat("UNKNOWN", "");
}

// Will send: STATE senderId 192.168.0.24:V;192.168.0.27:A
void network_send_state(const char* senderId, char* incl_ip) {

    cop_debug("[network_send_state]");

    char* state_str = get_state_str();
    char* msg1 = concat("STATE ", senderId);
    char* msg2 = concat(msg1, " ");
    char* msg3 = concat(msg2, state_str);
    size_t msg_length = strlen(msg3);

    if (incl_ip != NULL) {
        cop_debug("[network_send_state] incl: Send state: %s to: %s.", msg3, incl_ip);
        network_send_tcp(msg3, msg_length, incl_ip);
    }

    // Camera
    list_item* clone_cam = list_clone(client_data_cam_list);
    for (int i = 0; i < list_length(clone_cam); i++) {
        list_item* item = list_get(clone_cam, i);
        client_data* data = (client_data*)item->data;

        cop_debug("[network_send_state] cam: Send state: %s to: %s.", msg3, data->src_ip);
        network_send_tcp(msg3, msg_length, data->src_ip);
    }
    list_clear(clone_cam);

    // Mic
    list_item* clone_mic = list_clone(client_data_mic_list);
    for (int i = 0; i < list_length(clone_mic); i++) {
        list_item* item = list_get(clone_mic, i);
        client_data* data = (client_data*)item->data;

        cop_debug("[network_send_state] mic: Send state: %s to: %s.", msg3, data->src_ip);
        network_send_tcp(msg3, msg_length, data->src_ip);
    }
    list_clear(clone_mic);

    free(state_str);
    free(msg1);
    free(msg2);
    free(msg3);
}

void proxy_close() {

    list_item* clone_cam = list_clone(client_data_cam_list);
    for (int i = 0; i < list_length(clone_cam); i++) {
        list_item* item = list_get(clone_cam, i);
        client_data* data = (client_data*)item->data;
        if (data->socket < 0) {
            cop_error("[proxy_close] cam: Socket receive not open: %d (ip: %s).", data->socket, data->src_ip);
        } else {
            cop_debug("[proxy_close] cam: Close socket (ip: %s).", data->src_ip);
            close(data->socket);
        }
    }
    list_clear(clone_cam);

    list_item* clone_mic = list_clone(client_data_mic_list);
    for (int i = 0; i < list_length(clone_mic); i++) {
        list_item* item = list_get(clone_mic, i);
        client_data* data = (client_data*)item->data;
        if (data->socket < 0) {
            cop_error("[proxy_close] mic: Socket receive not open: %d (ip: %s).", data->socket, data->src_ip);
        } else {
            cop_debug("[proxy_close] mic: Close socket (ip: %s).", data->src_ip);
            close(data->socket);
        }
    }
    list_clear(clone_mic);

    if (proxy_receive_udp_socket_cam < 0) {
        cop_error("[proxy_close] cam: Socket receive not open: %d.", proxy_receive_udp_socket_cam);
    } else {
        cop_debug("[proxy_close] cam: Close 'receive-udp-socket'.");
        close(proxy_receive_udp_socket_cam);
    }
    is_network_running_cam = false;
    if (proxy_receive_udp_socket_mic < 0) {
        cop_error("[proxy_close] mic: Socket receive not open: %d.", proxy_receive_udp_socket_mic);
    } else {
        cop_debug("[proxy_close] mic: Close 'receive-udp-socket'.");
        close(proxy_receive_udp_socket_mic);
    }
    is_network_running_mic = false;
}

void server_close() {
    if (receive_tcp_socket < 0) {
        cop_error("[proxy_close] Socket receive (tcp) not open: %d.", receive_tcp_socket);
    } else {
        cop_debug("[proxy_close] Close 'receive-tcp-socket'.");
        close(receive_tcp_socket);
    }
}

static void set_next_file_cam() {
    char* timestamp = get_timestamp();
    char* file_cam_name2 = concat("video_", timestamp);
    char* file_cam_name3 = concat(file_cam_name2, ".ts");
    cop_debug("[set_next_file_cam] New video file: %s.", file_cam_name3);
    file_cam = fopen(file_cam_name3, "ab");
    free(timestamp);
    free(file_cam_name2);
    if (file_cam_name != NULL) {
        free(file_cam_name);
    }
    file_cam_name = file_cam_name3;
}

static void set_next_file_mic() {
    char* timestamp = get_timestamp();
    char* file_mic_name2 = concat("audio_", timestamp);
    char* file_mic_name3 = concat(file_mic_name2, ".ts");
    cop_debug("[set_next_file_mic] New audio file: %s.", file_mic_name3);
    file_mic = fopen(file_mic_name3, "ab");
    free(timestamp);
    free(file_mic_name2);
    if (file_mic_name != NULL) {
        free(file_mic_name);
    }
    file_mic_name = file_mic_name3;
}

static list_item* add_ip_to_list(list_item* client_data_list, char* ip, int dest_port) {

    list_item* clone = list_clone(client_data_list);

    bool exists = false;
    for (int i = 0; i < list_length(clone); i++) {
        list_item* item = list_get(clone, i);
        client_data* data = (client_data*)item->data;
        exists = equals(data->src_ip, ip) && data->src_port == dest_port;
        if (exists) {
            break;
        }
    }
    if (!exists) {
        cop_debug("[add_ip_to_list] Ip does not exists yet: %s. Add to list.", ip);
        // Add client data
        client_data* data = malloc(sizeof(client_data));
        data->src_ip = strdup(ip);
        data->src_port = dest_port;
        int client_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket < 0) {
            cop_error("[add_ip_to_list] Could not create socket: %d.", socket);
            return clone;
        }
        data->socket = client_socket;
        clone = list_push(clone, data);
    } else {
        cop_debug("[add_ip_to_list] Ip already exists: %s. Do nothing.", ip);
    }

    return clone;
}

void proxy_init_cam(const char* pwd) {
    cop_debug("[proxy_init_cam]");
    encryption_pwd_cam = pwd;
    set_next_file_cam();
    is_network_running_cam = true;
    cop_debug("[proxy_init_cam] Done.");
}

void proxy_init_mic(const char* pwd) {
    cop_debug("[proxy_init_mic]");
    encryption_pwd_mic = pwd;
    set_next_file_mic();
    is_network_running_mic = true;
    cop_debug("[proxy_init_mic] Done.");
}

// We only want to redirect output to localhost again
void proxy_remove_all_clients_cam() {
    cop_debug("[proxy_remove_all_clients_cam]");
    list_item* clone_cam = list_clone(client_data_cam_list);
    for (int i = 0; i < list_length(clone_cam); i++) {
        clone_cam = list_delete(clone_cam, 0);
    }
    list_item* ptr = client_data_cam_list;
    client_data_cam_list = clone_cam;
    list_clear(ptr);
    cop_debug("[proxy_remove_all_clients_cam] Done.");
}
void proxy_remove_all_clients_mic() {
    cop_debug("[proxy_remove_all_clients_mic]");
    list_item* clone_mic = list_clone(client_data_mic_list);
    for (int i = 0; i < list_length(clone_mic); i++) {
        clone_mic = list_delete(clone_mic, 0);
    }
    list_item* ptr = client_data_mic_list;
    client_data_mic_list = clone_mic;
    list_clear(ptr);
    cop_debug("[proxy_remove_all_clients_mic] Done.");
}

void proxy_remove_client_cam(char* reset_ip) {
    cop_debug("[proxy_remove_client_cam] %s.", reset_ip);
    list_item* clone_cam = list_clone(client_data_cam_list);
    int i = 0;
    for (; i < list_length(clone_cam); i++) {
        list_item* item = list_get(clone_cam, i);
        client_data* data = (client_data*)item->data;
        if (equals(data->src_ip, reset_ip)) {
            break;
        }
    }
    if (i < list_length(clone_cam)) {
        clone_cam = list_delete(clone_cam, i);
    }
    list_item* ptr = client_data_cam_list;
    client_data_cam_list = clone_cam;
    list_clear(ptr);
    cop_debug("[proxy_remove_client_cam] Done.");
}
void proxy_remove_client_mic(char* reset_ip) {
    cop_debug("[proxy_remove_client_mic] %s.", reset_ip);
    list_item* clone_mic = list_clone(client_data_mic_list);
    int i = 0;
    for (; i < list_length(clone_mic); i++) {
        list_item* item = list_get(clone_mic, i);
        client_data* data = (client_data*)item->data;
        if (equals(data->src_ip, reset_ip)) {
            break;
        }
    }
    if (i < list_length(clone_mic)) {
        clone_mic = list_delete(clone_mic, i);
    }
    list_item* ptr = client_data_mic_list;
    client_data_mic_list = clone_mic;
    list_clear(ptr);
    cop_debug("[proxy_remove_client_mic] Done.");
}

void proxy_connect_cam(char* dest_ip, int dest_port) {
    cop_debug("[proxy_connect_cam] Connect proxy to %s and port %d.", dest_ip, dest_port);
    // Add client data
    list_item* clone = add_ip_to_list(client_data_cam_list, dest_ip, dest_port);
    list_item* ptr = client_data_cam_list;
    client_data_cam_list = clone;
    list_clear(ptr);
    cop_debug("[proxy_connect_cam] Done.");
}
void proxy_connect_mic(char* dest_ip, int dest_port) {
    cop_debug("[proxy_connect_mic] Connect proxy to %s and port %d.", dest_ip, dest_port);
    // Add client data
    list_item* clone = add_ip_to_list(client_data_mic_list, dest_ip, dest_port);
    list_item* ptr = client_data_mic_list;
    client_data_mic_list = clone;
    list_clear(ptr);
    cop_debug("[proxy_connect_mic] Done.");
}

// Type = 0: cam, 1: mic
void proxy_send_udp(int type, const char* data, int size) {

    static struct sockaddr_in dest_addr;

    if (type == 0) {
        list_item* clone_cam = list_clone(client_data_cam_list);
        for (int i = 0; i < list_length(clone_cam); i++) {
            list_item* item = list_get(clone_cam, i);
            client_data* cl_data = (client_data*)item->data;
            dest_addr.sin_family = AF_INET;
            dest_addr.sin_addr.s_addr = inet_addr(cl_data->src_ip);
            dest_addr.sin_port = htons(cl_data->src_port);
            if (cl_data->socket < 0) {
                cop_error("[proxy_send_udp] cam: Socket not available: %d", cl_data->socket);
            }
            int result = sendto(cl_data->socket, data, size, 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
            if (result < 0) {
                cop_error("[proxy_send_udp] cam: Could not send data. Result: %d.", result);
            }
        }
        list_clear(clone_cam);
    } else if (type == 1) {
        list_item* clone_mic = list_clone(client_data_mic_list);
        for (int i = 0; i < list_length(clone_mic); i++) {
            list_item* item = list_get(clone_mic, i);
            client_data* cl_data = (client_data*)item->data;
            dest_addr.sin_family = AF_INET;
            dest_addr.sin_addr.s_addr = inet_addr(cl_data->src_ip);
            dest_addr.sin_port = htons(cl_data->src_port);
            if (cl_data->socket < 0) {
                cop_error("[proxy_send_udp] mic: Socket not available: %d", cl_data->socket);
            }
            int result = sendto(cl_data->socket, data, size, 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
            if (result < 0) {
                cop_error("[proxy_send_udp] mic: Could not send data. Result: %d.", result);
            }
        }
        list_clear(clone_mic);
    }
}

// Type = 0: cam, 1: mic
static int proxy_receive_udp(int type, int proxy_receive_udp_socket, int port_proxy_listen) {

    struct sockaddr_in addr, si_other;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_proxy_listen);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    cop_debug("[proxy_receive_udp] Bind to port %d.", port_proxy_listen);
    int result = bind(proxy_receive_udp_socket, (struct sockaddr *)&addr, sizeof(addr));
    if (result == -1) {
        cop_error("[proxy_receive_udp] Could not bind socket do %d.", port_proxy_listen);
        return STATUS_CODE_NOK;
    }

    char* sendBuffer = malloc(sizeof(char) * PROXY_SEND_BUFFER_SIZE_BYTES);
    memset(sendBuffer, '\0', PROXY_SEND_BUFFER_SIZE_BYTES);
    int sendIndex = 0;

    char* buffer = malloc(sizeof(char) * PROXY_BUFFER_SIZE_BYTES);
    memset(buffer, '\0', PROXY_BUFFER_SIZE_BYTES);
    unsigned slen=sizeof(addr);

    while ((type == 0 && is_network_running_cam) || (type == 1 && is_network_running_mic)) {
        int read = recvfrom(proxy_receive_udp_socket, buffer, PROXY_BUFFER_SIZE_BYTES, 0, (struct sockaddr *)&si_other, &slen);

        if (read == -1) {
            cop_error("[proxy_receive_udp] Stop proxy.");
            break;
        }

        if (sendIndex + read < PROXY_SEND_BUFFER_SIZE_BYTES) {
            // Buffer won't be filled
            memcpy(&sendBuffer[sendIndex], buffer, read);
            sendIndex += read;
        } else {
            // Buffer is filled
            memcpy(&sendBuffer[sendIndex], buffer, PROXY_SEND_BUFFER_SIZE_BYTES - sendIndex);

            // Write to file
            if (type == 0 && file_cam != NULL) {
                fwrite(sendBuffer, PROXY_SEND_BUFFER_SIZE_BYTES, 1, file_cam);
                long size_in_kb = ftell(file_cam) / 1024;
                // Set max size to 100 mb
                if (size_in_kb > 1024 * 100) {
                    fclose(file_cam);
                    set_next_file_cam();
                }
            } else if (type == 1 && file_mic != NULL) {
                fwrite(sendBuffer, PROXY_SEND_BUFFER_SIZE_BYTES, 1, file_mic);
                long size_in_kb = ftell(file_mic) / 1024;
                // Set max size to 100 mb
                if (size_in_kb > 1024 * 100) {
                    fclose(file_mic);
                    set_next_file_mic();
                }
            }

            // Do encryption
            if (type == 0 && encryption_pwd_cam != NULL) {
                size_t pwd_length = strlen(encryption_pwd_cam);
                if (pwd_length > 0) {
                    for(int i = 0; i < PROXY_SEND_BUFFER_SIZE_BYTES; i++) {
                        sendBuffer[i] = sendBuffer[i] ^ encryption_pwd_cam[i % pwd_length];
                    }
                }
            } else if (type == 1 && encryption_pwd_mic != NULL) {
                size_t pwd_length = strlen(encryption_pwd_mic);
                if (pwd_length > 0) {
                    for(int i = 0; i < PROXY_SEND_BUFFER_SIZE_BYTES; i++) {
                        sendBuffer[i] = sendBuffer[i] ^ encryption_pwd_mic[i % pwd_length];
                    }
                }
            }

            proxy_send_udp(type, sendBuffer, PROXY_SEND_BUFFER_SIZE_BYTES);
            memcpy(sendBuffer, &buffer[PROXY_SEND_BUFFER_SIZE_BYTES - sendIndex], read - (PROXY_SEND_BUFFER_SIZE_BYTES - sendIndex));
            sendIndex = read - (PROXY_SEND_BUFFER_SIZE_BYTES - sendIndex);
        }
    }

    if (file_cam != NULL) {
        fclose(file_cam);
    }
    file_cam_name = NULL;
    file_cam = NULL;

    if (file_mic != NULL) {
        fclose(file_mic);
    }
    file_mic_name = NULL;
    file_mic = NULL;
    
    cop_debug("[proxy_receive_udp] Done.");

    return STATUS_CODE_OK;
}

int proxy_receive_udp_cam(void* arg) {
    proxy_receive_udp_socket_cam = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (proxy_receive_udp_socket_cam == -1) {
        cop_error("[proxy_receive_udp_cam] Could not create socket.");
        return STATUS_CODE_NOK;
    }
    return proxy_receive_udp(0, proxy_receive_udp_socket_cam, PORT_PROXY_LISTEN_CAM);
}

int proxy_receive_udp_mic(void* arg) {
    proxy_receive_udp_socket_mic = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (proxy_receive_udp_socket_mic == -1) {
        cop_error("[proxy_receive_udp_mic] Could not create socket.");
        return STATUS_CODE_NOK;
    }
    return proxy_receive_udp(1, proxy_receive_udp_socket_mic, PORT_PROXY_LISTEN_MIC);
}

char* get_video_file_name() {
    return file_cam_name;
}

char* get_audio_file_name() {
    return file_mic_name;
}

// Produces tuples ip:type (V for video, A for audio): 192.168.0.25:V;182.168.0.27:A
char* get_sendto_ip() {
    char* buffer = "";
    list_item* clone_cam = list_clone(client_data_cam_list);
    list_item* clone_mic = list_clone(client_data_mic_list);
    for (int i = 0; i < list_length(clone_cam); i++) {
        list_item* item = list_get(clone_cam, i);
        client_data* data = (client_data*)item->data;
        buffer = concat(buffer, data->src_ip);
        buffer = concat(buffer, ":");
        buffer = concat(buffer, "V");
        if (i < list_length(clone_cam) - 1) {
            buffer = concat(buffer, ";");
        }
    }
    if (list_length(clone_mic) > 0) {
        buffer = concat(buffer, ";");
    }
    for (int i = 0; i < list_length(clone_mic); i++) {
        list_item* item = list_get(clone_mic, i);
        client_data* data = (client_data*)item->data;
        buffer = concat(buffer, data->src_ip);
        buffer = concat(buffer, ":");
        buffer = concat(buffer, "A");
        if (i < list_length(clone_mic) - 1) {
            buffer = concat(buffer, ";");
        }
    }
    list_clear(clone_cam);
    list_clear(clone_mic);
    return buffer;
}

static char* get_hostname() {
    int MAX_LENGTH = 256;
    char* hostname = malloc(sizeof(char) * (MAX_LENGTH + 1));
    int ret = gethostname(hostname, MAX_LENGTH + 1);
    if (ret != 0) {
        return "n/a";
    }
    return hostname;
}

// Will return: SCAN hostname senderId state width height has_video has_audio
static void tcp_return_scan(int client_socket, const char* senderId, int width, int height, int has_video, int has_audio) {

    char* hostname = get_hostname();
    char* state = get_state_str();
    char* width_str = int_to_str(width);
    char* height_str = int_to_str(height);
    char* has_video_str = int_to_str(has_video);
    char* has_audio_str = int_to_str(has_audio);
    char* buffer1 = concat("SCAN ", hostname);
    char* buffer2 = concat(buffer1, " ");
    char* buffer3 = concat(buffer2, senderId);
    char* buffer4 = concat(buffer3, " ");
    char* buffer5 = concat(buffer4, state);
    char* buffer6 = concat(buffer5, " ");
    char* buffer7 = concat(buffer6, width_str);
    char* buffer8 = concat(buffer7, " ");
    char* buffer9 = concat(buffer8, height_str);
    char* buffer10 = concat(buffer9, " ");
    char* buffer11 = concat(buffer10, has_video_str);
    char* buffer12 = concat(buffer11, " ");
    char* buffer13 = concat(buffer12, has_audio_str);
    char* base_ptr = buffer13;

    int size = strlen(buffer13);

    cop_debug("[tcp_return_scan] Send '%d' bytes to caller.", size);

    int sizeLeftToSend = size;
    
    for (int j = 0; j < size; j+=BUFFER_SIZE) {
        
        int buffSizeToSend = BUFFER_SIZE;
        if (sizeLeftToSend < BUFFER_SIZE) {
            buffSizeToSend = sizeLeftToSend;
        }
        
        int result = send(client_socket, base_ptr, buffSizeToSend, 0);
        if (result < 0) {
            cop_error("[tcp_return_scan] Send failed: %d.", result);
            break;
        }
        sizeLeftToSend -= BUFFER_SIZE;
        // Set start position of sending data
        base_ptr = base_ptr + buffSizeToSend;
    }

    cop_debug("[tcp_return_scan] Finishing: Shutdown socket.");
    shutdown(client_socket, SHUT_RDWR);
    cop_debug("[tcp_return_scan] Finishing: Close socket.");
    close(client_socket);

    free(buffer1);
    free(buffer2);
    free(buffer3);
    free(buffer4);
    free(buffer5);
    free(buffer6);
    free(buffer7);
    free(buffer8);
    free(buffer9);
    free(buffer10);
    free(buffer11);
    free(buffer12);
    free(buffer13);
    free(hostname);
    free(state);
    free(width_str);
    free(height_str);
    free(has_video_str);
    free(has_audio_str);
}

// Will return: STATUS temperature (in milli degrees, divide by 1000)
static void tcp_return_status(int client_socket, const char* senderId) {

    int temp_in_milli_degrees;
    FILE* temp_file = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
    if (temp_file == NULL) {
        cop_error("[tcp_return_status] Temperature not available.");
    }
    fscanf(temp_file, "%d", &temp_in_milli_degrees);
    fclose(temp_file);

    char* temp_str = int_to_str(temp_in_milli_degrees);
    char* buffer1 = concat("STATUS ", senderId);
    char* buffer2 = concat(buffer1, " ");
    char* buffer3 = concat(buffer2, temp_str);
    char* base_ptr = buffer3;

    int size = strlen(buffer3);

    cop_debug("[tcp_return_status] Send '%d' bytes to caller.", size);

    int sizeLeftToSend = size;
    
    for (int j = 0; j < size; j+=BUFFER_SIZE) {
        
        int buffSizeToSend = BUFFER_SIZE;
        if (sizeLeftToSend < BUFFER_SIZE) {
            buffSizeToSend = sizeLeftToSend;
        }
        
        int result = send(client_socket, base_ptr, buffSizeToSend, 0);
        if (result < 0) {
            cop_error("[tcp_return_status] Send failed: %d.", result);
            break;
        }
        sizeLeftToSend -= BUFFER_SIZE;
        // Set start position of sending data
        base_ptr = base_ptr + buffSizeToSend;
    }

    cop_debug("[tcp_return_status] Finishing: Shutdown socket.");
    shutdown(client_socket, SHUT_RDWR);
    cop_debug("[tcp_return_status] Finishing: Close socket.");
    close(client_socket);

    free(buffer1);
    free(buffer2);
    free(buffer3);
    free(temp_str);
}

static void tcp_return_download(int client_socket, const char* fileName) {
    FILE* download_file = fopen(fileName, "rb");

    // Calc the size needed
    fseek(download_file, 0, SEEK_END); 
    int size = ftell(download_file);
    fseek(download_file, 0, SEEK_SET);
    // Allocale space on heap
    char* download_buffer = malloc(size);
    memset(download_buffer, '\0', size);

    cop_debug("[tcp_return_download] Read %d bytes from file.", size);

    fread(download_buffer, 1, size, download_file);

    cop_debug("[tcp_return_download] Successfully read binary data.");

    int overall = 0;

    int sizeLeftToSend = size;
    for (int j = 0; j < size; j+=BUFFER_SIZE) {
        
        int buffSizeToSend = BUFFER_SIZE;
        if (sizeLeftToSend < BUFFER_SIZE) {
            buffSizeToSend = sizeLeftToSend;
        }

        // Do encryption
        // TODO: Assume camera password
        if (encryption_pwd_cam != NULL) {
            size_t pwd_length = strlen(encryption_pwd_cam);
            if (pwd_length > 0) {
                for (int i = 0; i < buffSizeToSend; i++) {
                    download_buffer[i] = download_buffer[i] ^ encryption_pwd_cam[overall % pwd_length];
                    overall++;
                }
            }
        }
        
        int result = send(client_socket, download_buffer, buffSizeToSend, 0);
        if (result < 0) {
            //close(client_socket);
            cop_error("[tcp_return_download] Send failed: %d.", result);
            break;
        }
        sizeLeftToSend -= BUFFER_SIZE;
        // Set start position of sending data
        download_buffer = download_buffer + buffSizeToSend;
    }

    cop_debug("[tcp_return_download] Finishing download: Shutdown socket.");
    shutdown(client_socket, SHUT_RDWR);
    cop_debug("[tcp_return_download] Finishing download: Close socket.");
    close(client_socket);
    cop_debug("[tcp_return_download] Finishing download: Close file.");
    fclose(download_file);
}

static void tcp_return_list_files(int client_socket) {

    struct dirent *entry;
    DIR *dir = opendir("./");
    if (dir == NULL) {
        return;
    }

    struct list_item* file_list = NULL;

    while ((entry = readdir(dir)) != NULL) {
        if (contains(entry->d_name, "video_") || contains(entry->d_name, "audio_")) {
            FileItem* file_item = malloc(sizeof(FileItem));
            file_item->file_name = strdup(entry->d_name);
            file_list = list_push(file_list, file_item);
            FILE* file = fopen(entry->d_name, "rb");
            fseek(file, 0, SEEK_END);
            file_item->file_size_kb = ftell(file) / 1024;
            fclose(file);
        }
    }
    closedir(dir);

    const char* buffer = "LIST_FILES";

    for (int i = 0; i < list_length(file_list); i++) {
        list_item* item = list_get(file_list, i);
        FileItem* file = (FileItem*)item->data;
        buffer = concat(buffer, " ");
        buffer = concat(buffer, file->file_name);
        buffer = concat(buffer, ";");
        buffer = concat(buffer, int_to_str(file->file_size_kb));
    }

    int size = strlen(buffer);

    cop_debug("[tcp_return_list_files] Send '%d' bytes to caller.", size);

    int sizeLeftToSend = size;
    
    for (int j = 0; j < size; j+=BUFFER_SIZE) {
        
        int buffSizeToSend = BUFFER_SIZE;
        if (sizeLeftToSend < BUFFER_SIZE) {
            buffSizeToSend = sizeLeftToSend;
        }
        
        int result = send(client_socket, buffer, buffSizeToSend, 0);
        if (result < 0) {
            cop_error("[tcp_return_list_files] Send failed: %d.", result);
            break;
        }
        sizeLeftToSend -= BUFFER_SIZE;
        // Set start position of sending data
        buffer = buffer + buffSizeToSend;
    }

    cop_debug("[tcp_return_list_files] Finishing: Shutdown socket.");
    shutdown(client_socket, SHUT_RDWR);
    cop_debug("[tcp_return_list_files] Finishing: Close socket.");
    close(client_socket);
}

int network_receive_tcp(void* arg) {

    container_config* container = (container_config*)arg;
    system_config* config = container->system_config;

    struct sockaddr_in serv_addr;
    struct sockaddr_in client_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT_LISTEN_COMMAND_TCP);
    serv_addr.sin_addr.s_addr = INADDR_ANY;

    receive_tcp_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (receive_tcp_socket < 0) {
        cop_error("[network_receive_tcp] Could not create socket.");
        return STATUS_CODE_NOK;
    }

    // Bind
    if (bind(receive_tcp_socket, (struct sockaddr*) &serv_addr, sizeof(serv_addr)) < 0) {
        cop_error("[network_receive_tcp] Bind failed.");
        // Since we're in a thread signal the main application
        quit = 1;
        return STATUS_CODE_NOK;
    }

    cop_debug("[network_receive_tcp] Start listening for clients.");

    listen(receive_tcp_socket, 1);

    int c = sizeof(struct sockaddr_in);

    while (true) {

        cop_debug("[network_receive_tcp] Wait for clients: %d.", receive_tcp_socket);

        int client_socket = accept(receive_tcp_socket, (struct sockaddr*) &client_addr, (socklen_t*)&c);

        if (client_socket < 0) {
            cop_error("[network_receive_tcp] Accept failed: %d.", client_socket);
            continue;
        }

        cop_debug("[network_receive_tcp] Accept incoming connection.");

        // Receive data
        int read_size = 0;
        char* buffer = malloc(sizeof(char) * BUFFER_SIZE);
        memset(buffer, '\0', BUFFER_SIZE);

        if ((read_size = recv(client_socket, buffer, BUFFER_SIZE, 0)) > 0) {
            char* string = strdup(buffer);
            if (string == NULL) {
                close(client_socket);
                cop_error("[network_receive_tcp] Receive failed.");
                continue;
            }

            command_data* data = malloc(sizeof(command_data));

            char* token = string;
            char* end = string;

            cop_debug("[network_receive_tcp] String: %s.", string);

            int i = 0;
            char* cmd;
            while(token != NULL) {
                // This is the only way to use strsep() without causing memory leaks
                strsep(&end, " ");
                cop_debug("[network_receive_tcp] Token: %s. Index: %d", token, i);
                if (i == 0) {
                    cmd = token;

                    if (equals(cmd, "START")) {
                        container->cb_start();
                        close(client_socket);
                        break;
                    }
                    if (equals(cmd, "STOP")) {
                        container->cb_stop();
                        close(client_socket);
                        break;
                    }

                    // LIST_FILES: Returns list of files
                    if (equals(cmd, "LIST_FILES")) {
                        tcp_return_list_files(client_socket);
                        break;
                    }

                    // STATUS: Returns status like temperature
                    if (equals(cmd, "STATUS")) {
                        tcp_return_status(client_socket, config->senderId);
                        break;
                    }

                    // SCAN: Returns device id, width and height
                    if (equals(cmd, "SCAN")) {
                        tcp_return_scan(client_socket, config->senderId, config->width, config->height, config->has_video, config->has_audio);
                        break;
                    }
                }
                // DOWNLOAD: Returns single file
                if (equals(cmd, "DOWNLOAD")) {
                    if (i == 1) {
                        tcp_return_download(client_socket, token);
                        i++;
                        break;
                    }
                }
                // CONNECT UDP <ip> <port-cam> <port-mic>
                if (equals(cmd, "CONNECT")) {
                    if (i == 1) {
                        data->protocol = token;
                    }
                    if (i == 2) {
                        data->ip = token;
                    }
                    if (i == 3) {
                        data->port_cam = str_to_int(token);
                    }
                    if (i == 4) {
                        data->port_mic = str_to_int(token);
                        container->cb_connect(data);
                        close(client_socket);
                        i++;
                        break;
                    }
                }
                // DELETE <filename>
                if (equals(cmd, "DELETE")) {
                    if (i == 1) {
                        data->file_name = token;
                        container->cb_delete(data);
                        close(client_socket);
                        i++;
                        break;
                    }
                }
                // RESET <ip>
                if (equals(cmd, "RESET")) {
                    if (i == 1) {
                        data->reset_ip = token;
                        container->cb_reset(data);
                        close(client_socket);
                        i++;
                        break;
                    }
                }

                i++;
                token = end;
            }

            free(data);
            free(string);
            free(buffer);
        } else {
            close(client_socket);
            cop_error("[network_receive_tcp] Receive failed.");
            continue;
        }
    }

    cop_debug("[network_receive_tcp] Done.");

    return STATUS_CODE_OK;
}
