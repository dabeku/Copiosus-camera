#include "cop_utility.h"

void cop_debug(const char* format, ...) {
    va_list argptr;

    time_t rawtime;
    struct tm * timeinfo;
    time (&rawtime);
    timeinfo = localtime (&rawtime);
    char output[128];
    sprintf(
        output,
        "[%02d.%02d.%04d %02d:%02d:%02d]",
        timeinfo->tm_mday,
        timeinfo->tm_mon + 1,
        timeinfo->tm_year + 1900,
        timeinfo->tm_hour,
        timeinfo->tm_min,
        timeinfo->tm_sec);

    char* prefix = concat(output, " [DEBUG] ");
    char* format1 = concat(prefix, format);
    char* format2 = concat(format1, "\n");

    va_start(argptr, format);
    vfprintf(stdout, format2, argptr);
    va_end(argptr);
    fflush(stdout);
    
    free(prefix);
    free(format1);
    free(format2);

}

void cop_error(const char* format, ...) {
    va_list argptr;

    time_t rawtime;
    struct tm * timeinfo;
    time (&rawtime);
    timeinfo = localtime (&rawtime);
    char output[128];
    sprintf(
        output,
        "[%02d.%02d.%04d %02d:%02d:%02d]",
        timeinfo->tm_mday,
        timeinfo->tm_mon + 1,
        timeinfo->tm_year + 1900,
        timeinfo->tm_hour,
        timeinfo->tm_min,
        timeinfo->tm_sec);

    char* prefix = concat(output, " [-ERROR-] ");
    char* format1 = concat(prefix, format);
    char* format2 = concat(format1, "\n");

    va_start(argptr, format);
    vfprintf(stdout, format2, argptr);
    va_end(argptr);
    fflush(stdout);
    
    free(prefix);
    free(format1);
    free(format2);
}

char* get_timestamp() {
    time_t rawtime;
    struct tm * timeinfo;
    time (&rawtime);
    timeinfo = localtime (&rawtime);
    char* buffer = malloc(sizeof(char) * 64);
    memset(buffer, '\0', 64);
    sprintf(
        buffer,
        "%04d-%02d-%02d_%02d-%02d-%02d",
        timeinfo->tm_year + 1900,
        timeinfo->tm_mon + 1,
        timeinfo->tm_mday,
        timeinfo->tm_hour,
        timeinfo->tm_min,
        timeinfo->tm_sec);

    return buffer;
}

int decode(AVCodecContext *avctx, AVFrame *frame, AVPacket *pkt, int *got_frame) {
    int ret;
    
    *got_frame = 0;
    
    if (pkt) {
        ret = avcodec_send_packet(avctx, pkt);
        // In particular, we don't expect AVERROR(EAGAIN), because we read all
        // decoded frames with avcodec_receive_frame() until done.
        if (ret < 0) {
            printf("[encode] Error sending a frame for decoding: %d.\n", ret);
            return ret == AVERROR_EOF ? 0 : ret;
        }
    }
    
    ret = avcodec_receive_frame(avctx, frame);
    if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
        printf("[encode] Error receiving a frame from decoding: %d.\n", ret);
        return ret;
    } else if (ret >= 0) {
        *got_frame = 1;
    }
    
    return 0;
}

int encode(AVCodecContext *avctx, AVFrame *frame, AVPacket *pkt, int *got_frame) {
    int ret;
    
    *got_frame = 0;
    
    // Send the frame to the encoder
    ret = avcodec_send_frame(avctx, frame);
    if (ret < 0) {
        cop_error("[encode] Error sending a frame for encoding: %d.", ret);
        return ret;
    }
    
    ret = avcodec_receive_packet(avctx, pkt);
    if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
        cop_error("[encode] Error receiving a frame from encoding: %d.", ret);
        return ret;
    } else if (ret >= 0) {
        *got_frame = 1;
    }
    
    return 0;
}

static int str_to_uint(char* num) {
    int dec = 0, i, len;
    len = strlen(num);
    for(i=0; i<len; i++){
        dec = dec * 10 + ( num[i] - '0' );
    }
    return dec;
}

int str_to_int(char * c) {
    return (*c == '-') ? -str_to_uint(c+1) : str_to_uint(c);
}

char* int_to_str(int num) {
    char* str = calloc(32, sizeof(char));
    sprintf(str, "%d", num);
    return str;
}

char* concat(const char *str1, const char *str2) {
    char *result = malloc(strlen(str1)+strlen(str2)+1); //+1 for the zero-terminator
    strcpy(result, str1);
    strcat(result, str2);
    return result;
}

bool equals(char* str1, char* str2) {
    if (str1 == NULL || str2 == NULL) {
        return 0;
    }
    
    int ret = strncmp(str1, str2, BUFFER_SIZE);

    if (ret == 0) {
        return true;
    }
    return false;
}

int compare(char* str1, char* str2) {
    return strncmp(str1, str2, BUFFER_SIZE);
}

bool contains(char* str, char* find) {
    if (strstr(str, find) != NULL) {
        return true;
    }
    return false;
}

char* rand_str(size_t length) {

    // Init seed generator so we get a different value
    srand (time(NULL));

    static char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";        
    char *randomString = NULL;

    if (length) {
        randomString = malloc(sizeof(char) * (length +1));

        if (randomString) {            
            for (int n = 0;n < length;n++) {            
                int key = rand() % (int)(sizeof(charset) -1);
                randomString[n] = charset[key];
            }

            randomString[length] = '\0';
        }
    }

    return randomString;
}

unsigned long get_available_space_mb(const char* path) {
    struct statvfs stat;
    if (statvfs(path, &stat) != 0) {
        // Error happens, just quits here
        return -1;
    }

    unsigned long available = stat.f_bavail * stat.f_frsize / 1024;
    return available / 1024;
}

/*
 * Removes oldest files if we need space.
 * prefix: "video_" or "audio_"
 */
void house_keeping(char* current_file, char* prefix) {

    if (current_file == NULL) {
        return;
    }

    struct dirent *entry;
    DIR *dir = opendir("./");
    if (dir == NULL) {
        return;
    }

    char* oldest_file = NULL;

    while ((entry = readdir(dir)) != NULL) {
        if (contains(entry->d_name, prefix)) {
            if (oldest_file == NULL) {
                oldest_file = strdup(entry->d_name);
            } else {
                if (compare(oldest_file, entry->d_name) > 0) {
                    oldest_file = strdup(entry->d_name);
                }
            }
        }
    }
    closedir(dir);

    if (equals(current_file, oldest_file)) {
        // Do nothing since we won't delete the current file
        return;
    }

    if (oldest_file != NULL) {
        cop_debug("[house_keeping] Delete oldest file: %s.", oldest_file);
        remove(oldest_file);
    }
}