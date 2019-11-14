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
#include "cop_status_code.h"

static int receive_udp_socket = -1;
static int receive_udp_broadcast_socket = -1;
static int receive_tcp_socket = -1;

static int proxy_send_udp_socket = -1;
static int proxy_receive_udp_socket = -1;
static struct sockaddr_in dest_addr;
static bool isNetworkRunning = false;
static FILE* video_file = NULL;
static char* video_file_name = NULL;

// Set by main() args
static const char* encryptionPwd = NULL;

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
        if (equals(data->cmd, "CONNECT")) {
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
        }
        if (equals(data->cmd, "DELETE")) {
            if (i == 1) {
                data->file_name = token;
                i++;
                continue;
            }
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
        
        int result = sendto(s, data, buffSizeToSend, 0, (struct sockaddr *)&addr, sizeof(addr));
        if (result < 0) {
            cop_error("[network_send_udp] Could not send data. Result: %d.", result);
            exit(-1);
        }
        sizeLeftToSend -= BUFFER_SIZE;

        data = data + buffSizeToSend;
    }
}

void proxy_close() {
    if (proxy_send_udp_socket < 0) {
        cop_error("[proxy_close] Socket send not open: %d.", proxy_send_udp_socket);
    } else {
        cop_debug("[proxy_close] Close 'send-udp-socket'.");
        close(proxy_send_udp_socket);
    }
    if (proxy_receive_udp_socket < 0) {
        cop_error("[proxy_close] Socket receive not open: %d.", proxy_receive_udp_socket);
    } else {
        cop_debug("[proxy_close] Close 'receive-udp-socket'.");
        close(proxy_receive_udp_socket);
    }
    isNetworkRunning = false;
}

void server_close() {
    if (receive_tcp_socket < 0) {
        cop_error("[proxy_close] Socket receive (tcp) not open: %d.", receive_tcp_socket);
    } else {
        cop_debug("[proxy_close] Close 'receive-tcp-socket'.");
        close(receive_tcp_socket);
    }
}

void set_next_video_file() {
    video_file_name = "video_";
    video_file_name = concat(video_file_name, get_timestamp());
    video_file_name = concat(video_file_name, ".ts");
    cop_debug("[set_next_video_file] New video file: %s.", video_file_name);
    video_file = fopen(video_file_name, "ab");
}

void proxy_init(const char* dest_ip, int dest_port, const char* pwd) {
    cop_debug("[proxy_init] %s %d.", dest_ip, dest_port);
    proxy_send_udp_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (proxy_send_udp_socket < 0) {
        cop_error("[proxy_init] Could not create socket: %d.", proxy_send_udp_socket);
        return;
    }
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_addr.s_addr = inet_addr(dest_ip);
    dest_addr.sin_port = htons(dest_port);
    encryptionPwd = pwd;
    set_next_video_file();

    isNetworkRunning = true;

    cop_debug("[proxy_init] Done.");
}

// We only want to redirect output to localhost again
void proxy_reset() {
    cop_debug("[proxy_reset] Resetting proxy to localhost and port %d.", PORT_PROXY_DESTINATION_DUMMY);
    dest_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    dest_addr.sin_port = htons(PORT_PROXY_DESTINATION_DUMMY);
    cop_debug("[proxy_reset] Done.");
}

void proxy_send_udp(const char* data) {
    //cop_debug("[proxy_send_udp].");

    if (proxy_send_udp_socket < 0) {
        cop_error("[proxy_send_udp] Socket not available: %d", proxy_send_udp_socket);
    }
    //cop_debug("[proxy_send_udp] Send data.");

    int result = sendto(proxy_send_udp_socket, data, PROXY_SEND_BUFFER_SIZE_BYTES, 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (result < 0) {
        cop_error("[proxy_send_udp] Could not send data. Result: %d.", result);
    }
}

int proxy_receive_udp(void* arg) {

    struct sockaddr_in addr, si_other;
    proxy_receive_udp_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (proxy_receive_udp_socket == -1) {
        cop_error("[proxy_receive_udp] Could not create socket.");
        return STATUS_CODE_NOK;
    }

    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT_PROXY_LISTEN);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    cop_debug("[proxy_receive_udp] Bind to port %d.", PORT_PROXY_LISTEN);
    int result = bind(proxy_receive_udp_socket, (struct sockaddr *)&addr, sizeof(addr));
    if (result == -1) {
        cop_error("[proxy_receive_udp] Could not bind socket do %d.", PORT_PROXY_LISTEN);
        return STATUS_CODE_NOK;
    }

    char* sendBuffer = malloc(sizeof(char) * PROXY_SEND_BUFFER_SIZE_BYTES);
    memset(sendBuffer, '\0', PROXY_SEND_BUFFER_SIZE_BYTES);
    int sendIndex = 0;

    char* buffer = malloc(sizeof(char) * PROXY_BUFFER_SIZE_BYTES);
    memset(buffer, '\0', PROXY_BUFFER_SIZE_BYTES);
    unsigned slen=sizeof(addr);

    while (isNetworkRunning) {
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

            if (video_file != NULL) {
                fwrite(sendBuffer, PROXY_SEND_BUFFER_SIZE_BYTES, 1, video_file);
                long size_in_kb = ftell(video_file) / 1024;
                //cop_debug("File size: %lu.", size_in_kb);
                // Set max size to 250 mb
                //if (size_in_kb > 1024 * 250) {
                if (size_in_kb > 1024 * 100) {
                    fclose(video_file);
                    set_next_video_file();
                }
            }

            // Do encryption
            if (encryptionPwd != NULL) {
                size_t pwd_length = strlen(encryptionPwd);
                if (pwd_length > 0) {
                    for(int i = 0; i < PROXY_SEND_BUFFER_SIZE_BYTES; i++) {
                        //sendBuffer[i] = sendBuffer[i] ^ i;
                        sendBuffer[i] = sendBuffer[i] ^ encryptionPwd[i % pwd_length];
                    }
                }
            }

            proxy_send_udp(sendBuffer);
            memcpy(sendBuffer, &buffer[PROXY_SEND_BUFFER_SIZE_BYTES - sendIndex], read - (PROXY_SEND_BUFFER_SIZE_BYTES - sendIndex));
            sendIndex = read - (PROXY_SEND_BUFFER_SIZE_BYTES - sendIndex);
        }
    }

    if (video_file != NULL) {
        fclose(video_file);
    }
    video_file_name = NULL;
    video_file = NULL;

    cop_debug("[proxy_receive_udp] Done.");

    return STATUS_CODE_OK;
}

char* get_video_file_name() {
    return video_file_name;
}

char* get_sendto_ip() {
    char* str = malloc(sizeof(char) * INET_ADDRSTRLEN);
    inet_ntop(AF_INET, &(dest_addr.sin_addr), str, INET_ADDRSTRLEN);
    return str;
}

int network_receive_tcp(void* arg) {

    struct sockaddr_in serv_addr;
    struct sockaddr_in client_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT_LISTEN_TCP_DOWNLOAD);
    serv_addr.sin_addr.s_addr = INADDR_ANY;

    receive_tcp_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (receive_tcp_socket < 0) {
        cop_error("[network_receive_tcp] Could not create socket.");
        return STATUS_CODE_NOK;
    }

    // Bind
    if (bind(receive_tcp_socket, (struct sockaddr*) &serv_addr, sizeof(serv_addr)) < 0) {
        cop_error("[network_receive_tcp] Bind failed.");
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
            break;
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

            int i = 0;
            char* token;
            char* cmd;
            while ((token = strsep(&buffer, " ")) != NULL) {
                cop_debug("[network_receive_tcp] Token: %s", token);
                if (i == 0) {
                    cmd = token;
                    i++;
                    continue;
                }
                if (equals(cmd, "DOWNLOAD")) {
                    if (i == 1) {
                        FILE* download_file = fopen(token, "rb");

                        // Calc the size needed
                        fseek(download_file, 0, SEEK_END); 
                        int size = ftell(download_file);
                        fseek(download_file, 0, SEEK_SET);
                        // Allocale space on heap
                        char* download_buffer = malloc(size);
                        memset(download_buffer, '\0', size);

                        cop_debug("[network_send_tcp] Read %d bytes from file.", size);

                        fread(download_buffer, 1, size, download_file);

                        cop_debug("[network_send_tcp] Successfully read binary data.");

                        int sizeLeftToSend = size;
                        for (int j = 0; j < size; j+=BUFFER_SIZE) {
                            
                            int buffSizeToSend = BUFFER_SIZE;
                            if (sizeLeftToSend < BUFFER_SIZE) {
                                buffSizeToSend = sizeLeftToSend;
                            }
                            
                            int result = send(client_socket, download_buffer, buffSizeToSend, 0);
                            if (result < 0) {
                                //close(client_socket);
                                cop_error("[network_send_tcp] Send failed: %d.", result);
                                break;
                            }
                            sizeLeftToSend -= BUFFER_SIZE;
                            // Set start position of sending data
                            download_buffer = download_buffer + buffSizeToSend;
                        }

                        cop_debug("[network_send_tcp] Finishing download: Shutdown socket.");
                        shutdown(client_socket, SHUT_RDWR);
                        cop_debug("[network_send_tcp] Finishing download: Close socket.");
                        close(client_socket);
                        cop_debug("[network_send_tcp] Finishing download: Close file.");
                        fclose(download_file);
                        i++;
                        break;
                    }
                }
                i++;
            }
        } else {
            close(client_socket);
            cop_error("[network_receive_tcp] Receive failed.");
            continue;
        }
    }

    cop_debug("[network_receive_tcp] Done.");

    return STATUS_CODE_OK;
}