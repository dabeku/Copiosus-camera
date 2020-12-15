#ifndef COP_UTILITY_H
#define COP_UTILITY_H

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h> // bool
#include <string.h>
#include <time.h>
#include <sys/statvfs.h> // filesystem
#include <dirent.h> // List files

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <libavutil/avstring.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/base64.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_thread.h>

#include "cop_list.h"

// A generic buffer size used for network and strings. Use whenever possible.
#define BUFFER_SIZE 512

void cop_debug(const char* format, ...);
void cop_error(const char* format, ...);
char* get_timestamp();
char* concat(const char *str1, const char *str2);
int str_to_int(char* num);
char* int_to_str(int num);
bool equals(char* str1, char* str2);
int compare(char* str1, char* str2);
bool contains(char* str, char* find);
char* rand_str(size_t length);
unsigned long get_available_space_mb(const char* path);
void house_keeping(char* path, char* prefix);

int decode(AVCodecContext *avctx, AVFrame *frame, AVPacket *pkt, int *got_frame);
int encode(AVCodecContext *avctx, AVFrame *frame, AVPacket *pkt, int *got_frame);

#endif