/*
 * Captures camera and microphone and sends it to a destination
 */

/*
 * TODO:
 * - Remove constants: nb_samples, 44100, etc.
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <signal.h>
#include <unistd.h> // sleep()

#include <libavutil/avassert.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/mathematics.h>
#include <libavutil/timestamp.h>
#include <libavutil/imgutils.h>
#include <libavformat/avformat.h>
#include <libavdevice/avdevice.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/time.h>

#include <SDL2/SDL_thread.h>

#include "cop_network.h"
#include "cop_status_code.h"
#include "cop_utility.h"

#define USE_PROXY 1
#define TEST_LOCAL 0

#define CFG_WIDTH 640
#define CFG_HEIGHT 480
#define CFG_FRAME_RATE 30

static char* LOCALHOST_IP = "127.0.0.1";
static const char* MPEG_TS_OPTIONS = "?pkt_size=1316";

// A global quit flag: 0 = running, 1 = quit
int quit = 0;
static int isAudioQuit = 1;
static int isVideoQuit = 1;
static int isAudioProcessing = 0;
static int isVideoProcessing = 0;

static char* platform = NULL;
static char* senderId = NULL;

// 0 = idle
// 1 = initializing
// 2 = connected
// 3 = disconnecting
int state = 0;

// Config section
int cfg_framerate = 0;
int cfg_width = 0;
int cfg_height = 0;

SDL_mutex *write_mutex = NULL;
SDL_Thread *audio_thread = NULL;
SDL_Thread *video_thread = NULL;

// Set by main() args
char* pCamName = NULL;
AVFormatContext *pCamFormatCtx = NULL;
AVInputFormat *pCamInputFormat = NULL;
AVDictionary *pCamOpt = NULL;
AVCodecContext *pCamCodecCtx = NULL;
AVCodec *pCamCodec = NULL;
AVPacket camPacket;
AVFrame *pCamFrame = NULL;
int camVideoStreamIndex = -1;
struct SwsContext *pCamSwsContext = NULL;
AVFrame *newpicture = NULL;

// Set by main() args
char* pMicName = NULL;
AVFormatContext *pMicFormatCtx = NULL;
AVInputFormat *pMicInputFormat = NULL;
AVDictionary *pMicOpt = NULL;
AVCodecContext *pMicCodecCtx = NULL;
AVCodec *pMicCodec = NULL;
//AVPacket micPacket;
AVFrame *decoded_frame = NULL;
int camAudioStreamIndex = -1;
struct SwrContext *swr_ctx = NULL;
// TODO: Decide what to do with this
uint8_t **src_data = NULL;
AVFrame *final_frame = NULL;

// Set by main() args
static char* encryptionPwd = NULL;

// a wrapper around a single output AVStream
typedef struct OutputStream {
    AVStream *st;
    AVCodecContext *enc;

    /* pts of the next frame that will be generated */
    int64_t next_pts;
    //int samples_count;

    AVFrame *frame;

    /* Bitstream context: This is needed to add SPS and PPS information to
     * all I frames in h264 stream. This is needed so we can start viewing
     * a stream in the middle instead of always from the beginning */
    AVBSFContext *bsf_ctx;
} OutputStream;

typedef struct Container {
    OutputStream *outputStreamAudio;
    OutputStream *outputStreamVideo;
    AVFormatContext *format_context_cam;
    AVFormatContext *format_context_mic;
} Container;

typedef struct FileItem {
    char* file_name;
    long file_size_kb;
} FileItem;

// Used here to free memory when closing app
AVFormatContext *output_context_cam = NULL;
AVFormatContext *output_context_mic = NULL;
OutputStream *video_st = NULL;
OutputStream *audio_st = NULL;
int have_video = 0;
int have_audio = 0;

/*
static void log_packet(const AVFormatContext *fmt_ctx, const AVPacket *pkt)
{
    AVRational *time_base = &fmt_ctx->streams[pkt->stream_index]->time_base;

    printf("pts:%s pts_time:%s dts:%s dts_time:%s duration:%s duration_time:%s stream_index:%d\n",
           av_ts2str(pkt->pts), av_ts2timestr(pkt->pts, time_base),
           av_ts2str(pkt->dts), av_ts2timestr(pkt->dts, time_base),
           av_ts2str(pkt->duration), av_ts2timestr(pkt->duration, time_base),
           pkt->stream_index);
}
*/

void intHandler(int dummy) {
    cop_debug("[intHandler] Closing app.");
    proxy_close();
    server_close();
    quit = 1;
}

void sigPipeHandler(int dummy) {
    cop_debug("[sigPipeHandler].");
}

void ePipeHandler(int dummy) {
    cop_debug("[ePipeHandler].");
}

/*
 * 0 = IDLE,
 * 1 = INITIALIZING,
 * 2 = CONNECTED,
 * 3 = DISCONNECTING
 * x = UNKNOWN
 */
static void changeState(int newState) {
    state = newState;
    network_send_state(senderId, NULL);
}

static void changeStateInclIp(int newState, char* incl_ip) {
    state = newState;
    network_send_state(senderId, incl_ip);
}

static int decode_video(AVCodecContext *avctx, AVFrame *frame, AVPacket *pkt, int *got_frame) {
    return decode(avctx, frame, pkt, got_frame);
}

static int decode_audio(AVCodecContext *avctx, AVFrame *frame, AVPacket *pkt, int *got_frame) {
    return decode(avctx, frame, pkt, got_frame);
}

static int encode_video(AVCodecContext *avctx, AVFrame *frame, AVPacket *pkt, int *got_frame) {
    return encode(avctx, frame, pkt, got_frame);
}

static int write_frame(AVFormatContext *fmt_ctx, const AVRational *time_base, AVStream *st, AVPacket *pkt, AVBSFContext* bsf_ctx) {
    
    pkt->stream_index = st->index;
    
    if (st->index == 1) {
        // Rescale audio pts from 1152 to 2351
        av_packet_rescale_ts(pkt, *time_base, st->time_base);
    }
    //cop_debug("PTS (stream: %d): %lld", st->index, pkt->pts);

    SDL_LockMutex(write_mutex);

    int result = av_bsf_send_packet(bsf_ctx, pkt);
    
    if (result < 0) {
        cop_error("[write_frame] Could not send packet to bsf: %d.", result);
        return result;
    }
    while ((result = av_bsf_receive_packet(bsf_ctx, pkt)) >= 0) {
        av_interleaved_write_frame(fmt_ctx, pkt);
    }
    
    SDL_UnlockMutex(write_mutex);
    
    return STATUS_CODE_OK;
}

// Add an output stream
static int add_stream(OutputStream *ost, AVFormatContext *oc, AVCodec **codec, enum AVCodecID codec_id) {
    AVCodecContext *c = NULL;
    int i;

    // Find the codec
    if (codec_id == AV_CODEC_ID_H264) {
        *codec = avcodec_find_encoder_by_name("h264_omx");
        if (!(*codec)) {
            cop_debug("[add_stream] Could not find encoder h264_omx. Try software encoder instead.");
            *codec = avcodec_find_encoder(codec_id);
        }
    } else {
        *codec = avcodec_find_encoder(codec_id);
    }
    
    if (!(*codec)) {
        cop_error("[add_stream] Could not find encoder for '%s'.", avcodec_get_name(codec_id));
        return STATUS_CODE_NOK;
    }

    // Add -bsf:v dump_extra so h264 stream adds SPS and PPS to I frames
    const AVBitStreamFilter* filter = av_bsf_get_by_name("dump_extra");
    if (!filter) {
        cop_error("Filter dump_extra not found.\n");
        return STATUS_CODE_NOK;
    }

    int ret = av_bsf_alloc(filter, &ost->bsf_ctx);
    if (ret < 0) {
        cop_error("Filter dump_extra not allocated.\n");
        return STATUS_CODE_NOK;
    }

    ost->st = avformat_new_stream(oc, NULL);
    if (!ost->st) {
        cop_error("[add_stream] Could not allocate stream.");
        return STATUS_CODE_NOK;
    }
    ost->st->id = oc->nb_streams-1;
    c = avcodec_alloc_context3(*codec);
    if (!c) {
        cop_error("[add_stream] Could not alloc an encoding context.");
        return STATUS_CODE_CANT_ALLOCATE;
    }
    ost->enc = c;

    switch ((*codec)->type) {
    case AVMEDIA_TYPE_AUDIO:
        c->sample_fmt  = (*codec)->sample_fmts ? (*codec)->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
        c->bit_rate    = 64000;
        c->sample_rate = 44100;
        if ((*codec)->supported_samplerates) {
            c->sample_rate = (*codec)->supported_samplerates[0];
            for (i = 0; (*codec)->supported_samplerates[i]; i++) {
                if ((*codec)->supported_samplerates[i] == 44100)
                    c->sample_rate = 44100;
            }
        }
        c->channels       = av_get_channel_layout_nb_channels(c->channel_layout);
        c->channel_layout = AV_CH_LAYOUT_STEREO;
        if ((*codec)->channel_layouts) {
            c->channel_layout = (*codec)->channel_layouts[0];
            for (i = 0; (*codec)->channel_layouts[i]; i++) {
                if ((*codec)->channel_layouts[i] == AV_CH_LAYOUT_STEREO)
                    c->channel_layout = AV_CH_LAYOUT_STEREO;
            }
        }
        c->channels        = av_get_channel_layout_nb_channels(c->channel_layout);
        ost->st->time_base = (AVRational){ 1, c->sample_rate };
        break;

    case AVMEDIA_TYPE_VIDEO:
        cop_debug("[add_stream] Set codec to '%d' (28 = H264).", codec_id);
        c->codec_id = codec_id;

        c->bit_rate = 400000;
        c->profile = FF_PROFILE_H264_BASELINE;
        /* Resolution must be a multiple of two. */
        c->width    = cfg_width;
        c->height   = cfg_height;
        /* timebase: This is the fundamental unit of time (in seconds) in terms
         * of which frame timestamps are represented. For fixed-fps content,
         * timebase should be 1/framerate and timestamp increments should be
         * identical to 1. */
        c->time_base       = (AVRational){ 1, cfg_framerate };

        //c->gop_size      = 12; /* emit one intra frame every twelve frames at most */
        c->pix_fmt       = AV_PIX_FMT_YUV420P;
        if (c->codec_id == AV_CODEC_ID_MPEG2VIDEO) {
            /* just for testing, we also add B-frames */
            c->max_b_frames = 1;
        }
        if (c->codec_id == AV_CODEC_ID_MPEG1VIDEO) {
            /* Needed to avoid using macroblocks in which some coeffs overflow.
             * This does not happen with normal video, it just happens here as
             * the motion of the chroma plane does not match the luma plane. */
            c->mb_decision = 2;
        }
        
        //av_opt_set(c->priv_data, "preset", "ultrafast", 0);
        av_opt_set(c->priv_data, "tune", "zerolatency", 0);
    break;

    default:
        break;
    }


    /* We need -flags:v +global_header so that h264_omx can
     * add SPS and PPS information to I frame header so we
     * can join the stream in the middle */
    c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    
    return STATUS_CODE_OK;
}

static AVFrame *alloc_audio_frame(enum AVSampleFormat sample_fmt, uint64_t channel_layout, int sample_rate, int nb_samples) {
    
    AVFrame *frame = av_frame_alloc();
    int ret;

    if (!frame) {
        cop_error("Can't allocate audio frame.");
        return NULL;
    }

    frame->format = sample_fmt;
    frame->channel_layout = channel_layout;
    frame->sample_rate = sample_rate;
    frame->nb_samples = nb_samples;

    if (nb_samples) {
        ret = av_frame_get_buffer(frame, 0);
        if (ret < 0) {
            cop_error("Can't allocate audio buffer.");
            return NULL;
        }
    }

    return frame;
}

static int open_audio(AVFormatContext *oc, AVCodec *codec, OutputStream *ost, AVDictionary *opt_arg) {
    
    AVCodecContext *c;
    int ret;
    AVDictionary *opt = NULL;

    c = ost->enc;

    /* open it */
    av_dict_copy(&opt, opt_arg, 0);
    ret = avcodec_open2(c, codec, &opt);
    
    av_dict_free(&opt);
    if (ret < 0) {
        cop_error("[open_audio] Could not open audio codec: %s.", av_err2str(ret));
        return STATUS_CODE_NOK;
    }

    ost->frame     = alloc_audio_frame(c->sample_fmt, c->channel_layout,
                                       c->sample_rate, c->frame_size);

    // Copy the stream parameters to the muxer
    ret = avcodec_parameters_from_context(ost->st->codecpar, c);
    if (ret < 0) {
        cop_error("[open_audio] Could not copy the stream parameters.");
        return STATUS_CODE_NOK;
    }
    return STATUS_CODE_OK;
}

static AVFrame *get_audio_frame(OutputStream *ost) {
    
    AVPacket micPacket = { 0 };

    while (isAudioQuit == 0) {
        // TODO: Add interrupt like in node module (when changing network while cop_sender is running
        // it will remain in state: 3)
        int ret = av_read_frame(pMicFormatCtx, &micPacket);
        if (ret < 0) {
            cop_error("[get_audio_frame] av_read_frame returned < 0.");
            continue;
        }
        if (micPacket.stream_index == camAudioStreamIndex) {
            int micFrameFinished = 0;
            
            ret = decode_audio(pMicCodecCtx, decoded_frame, &micPacket, &micFrameFinished);
            if (ret < 0) {
                cop_error("[get_audio_frame] Error in decoding audio frame.");
                av_packet_unref(&micPacket);
                continue;
            }
            
            av_packet_unref(&micPacket);
            
            if (micFrameFinished) {
				
				int nb_samples = decoded_frame->nb_samples;
				
                src_data = decoded_frame->data;

                /*int dst_bufsize_before = av_samples_get_buffer_size(NULL, 2,
                                                 nb_samples, AV_SAMPLE_FMT_S16, 0);
                FILE* dst_file = fopen("raw_before.mic", "ab");
                fwrite(src_data[0], 1, dst_bufsize_before, dst_file);
                fclose(dst_file);*/

                // Use swr_convert() as FIFO: Put in some data
                int outSamples = swr_convert(swr_ctx, NULL, 0, (const uint8_t **)src_data, nb_samples);

                if (outSamples < 0) {
                    cop_error("[get_audio_frame] No samples.");
                    return NULL;
                }
                
                while (1) {
                    // Get stored up data: Filled by swr_convert()
                    outSamples = swr_get_out_samples(swr_ctx, 0);

                    // 2 = channels of dest
                    // 1152 = frame_size of dest
                    int nb_channels = av_get_channel_layout_nb_channels(ost->enc->channel_layout);
                    int nb_samples = final_frame->nb_samples;
                    if (outSamples < nb_channels * nb_samples) {
                        // We don't have enough samples yet. Continue reading frames.
                        break;
                    }
                    // We got enough samples. Convert to destination format
                    outSamples = swr_convert(swr_ctx, final_frame->data, final_frame->nb_samples, NULL, 0);

                    /*int dst_bufsize = av_samples_get_buffer_size(NULL, nb_channels,
                                                 outSamples, AV_SAMPLE_FMT_S16, 0);
                    FILE* dst_file = fopen("raw_after.mic", "ab");
                    fwrite(final_frame->data[0], 1, dst_bufsize, dst_file);
                    fclose(dst_file);*/

                    final_frame->pts = ost->next_pts;
                    ost->next_pts += final_frame->nb_samples;
                    return final_frame;
                }
            }
        }
    }
    cop_debug("[get_audio_frame] Done.");
    return NULL;
}

static AVFrame *get_video_frame(OutputStream *ost) {
    
    AVCodecContext *c = ost->enc;
    
    while (isVideoQuit == 0) {
        int ret = av_read_frame(pCamFormatCtx, &camPacket);
        // On mac we will get -35 = EAGAIN since frame not available yet: Just wait for next frame
        if (ret == AVERROR(EAGAIN)) {
            usleep(100);
            continue;
        }
        if (ret < 0) {
            cop_error("[get_video_frame] av_read_frame returned: %d.", ret);
            continue;
        }
        if (camPacket.stream_index == camVideoStreamIndex) {
            int camFrameFinished;
            ret = decode_video(pCamCodecCtx, pCamFrame, &camPacket, &camFrameFinished);
            av_packet_unref(&camPacket);
            
            if (camFrameFinished) {
                sws_scale(pCamSwsContext, (uint8_t const * const *) pCamFrame->data, pCamFrame->linesize, 0, pCamCodecCtx->height, newpicture->data, newpicture->linesize);
                            newpicture->height =c->height;
                            newpicture->width =c->width;
                            newpicture->format = c->pix_fmt;
                
                ost->frame = newpicture;

                return ost->frame;
            } else {
                cop_debug("[get_video_frame] Frame not finished. Wait for next one.");
            }
        }
    }
    // TODO: Check why this is not called when disconnecting client
    cop_debug("[get_video_frame] Done.");
    return NULL;
}

static int write_audio_frame(AVFormatContext *oc, OutputStream *ost) {
    AVCodecContext *c;
    AVPacket pkt = { 0 }; // data and size must be 0;
    AVFrame *frame;
    int ret;
    int got_packet;

    av_init_packet(&pkt);
    c = ost->enc;

    frame = get_audio_frame(ost);

    ret = encode(c, frame, &pkt, &got_packet);
    if (ret < 0) {
        cop_error("[write_audio_frame] Error encoding audio frame: %s.", av_err2str(ret));
        return STATUS_CODE_NOK;
    }

    if (got_packet) {
        ret = write_frame(oc, &c->time_base, ost->st, &pkt, ost->bsf_ctx);
        if (ret < 0) {
            cop_error("[write_audio_frame] Error while writing audio frame: %s.", av_err2str(ret));
            return STATUS_CODE_NOK;
        }
        av_packet_unref(&pkt);
    }

    return (frame || got_packet) ? 0 : 1;
}

static int fps = 0;

static void logStats() {
    long availableMb = get_available_space_mb("/");
    
    cop_debug("[logStats] FPS: %d. Available space: %lu", fps, availableMb);
    fps = 0;

    if (availableMb < 300) {
        house_keeping(get_video_file_name(), "video_");
        house_keeping(get_audio_file_name(), "audio_");
    }
}

/*Uint32 periodic_cb(Uint32 interval, void *param) {
    logStats();
    return(interval);
}*/

static int64_t prevPts = 0;

static int write_video_frame(AVFormatContext *oc, OutputStream *ost) {
    
    fps++; 

    int ret;
    AVCodecContext *c;
    AVFrame *frame = NULL;
    int got_packet = 0;
    AVPacket pkt = { 0 };
    
    c = ost->enc;
    frame = get_video_frame(ost);
    av_init_packet(&pkt);

    // Calculate pts based on current time. 1000000 is for usec
    int64_t now = av_gettime();
    const AVRational codecTimebase = c->time_base;
    int64_t rescaledNow = av_rescale_q( now, (AVRational){1, 1000000},codecTimebase);
    frame->pts = rescaledNow; 

    // Encode the image
    ret = encode_video(c, frame, &pkt, &got_packet);
    
    if (ret < 0) {
        cop_error("[write_video_frame] Error encoding video frame: %s.", av_err2str(ret));
        return STATUS_CODE_NOK;
    }
    
    if (got_packet) {

        pkt.pts = av_rescale_q(pkt.pts, c->time_base, ost->st->time_base);
        pkt.dts = av_rescale_q(pkt.dts, c->time_base, ost->st->time_base);

        // To prevent non monotonically increasing pts and dts
        if (prevPts == pkt.pts) {
            pkt.pts = prevPts + 1;
            pkt.dts = prevPts + 1;
        }
        prevPts = pkt.pts;

        ret = write_frame(oc, &c->time_base, ost->st, &pkt, ost->bsf_ctx);
        av_packet_unref(&pkt);
    } else {
        ret = 0;
    }

    if (ret < 0) {
        cop_error("[write_video_frame] Error while writing video frame: %s.", av_err2str(ret));
    }

    return (frame || got_packet) ? 0 : 1;
}

int write_video(void *arg) {
    Container *container = (Container *)arg;
    while (isVideoQuit == 0) {
        isVideoProcessing = 1;
        write_video_frame(container->format_context_cam, container->outputStreamVideo);
        isVideoProcessing = 0;
    }
    return STATUS_CODE_OK;
}

int write_audio(void *arg) {
    Container *container = (Container *)arg;
    while (isAudioQuit == 0) {
        isAudioProcessing = 1;
        write_audio_frame(container->format_context_mic, container->outputStreamAudio);
        isAudioProcessing = 0;
    }
    return STATUS_CODE_OK;
}

static AVFrame *alloc_picture(enum AVPixelFormat pix_fmt, int width, int height) {
    
    AVFrame *picture;
    int ret;

    picture = av_frame_alloc();
    if (!picture) {
        cop_error("[alloc_picture] Could not allocate frame data.");
        return NULL;
    }

    picture->format = pix_fmt;
    picture->width  = width;
    picture->height = height;

    /* allocate the buffers for the frame data */
    ret = av_frame_get_buffer(picture, 32);
    if (ret < 0) {
        cop_error("[alloc_picture] Could not get frame buffer.");
        return NULL;
    }

    return picture;
}

static int open_video(AVFormatContext *oc, AVCodec *codec, OutputStream *ost, AVDictionary *opt_arg) {
    
    int ret;
    AVCodecContext *c = ost->enc;
    AVDictionary *opt = NULL;

    av_dict_copy(&opt, opt_arg, 0);

    /* open the codec */
    ret = avcodec_open2(c, codec, &opt);
    av_dict_free(&opt);
    if (ret < 0) {
        cop_error("[open_video] Could not open video codec: %s.", av_err2str(ret));
        return STATUS_CODE_NOK;
    }

    /* allocate and init a re-usable frame */
    ost->frame = alloc_picture(c->pix_fmt, c->width, c->height);
    if (!ost->frame) {
        cop_error("[open_video] Could not allocate video frame.");
        return STATUS_CODE_NOK;
    }

    /* copy the stream parameters to the muxer */
    ret = avcodec_parameters_from_context(ost->st->codecpar, c);
    if (ret < 0) {
        cop_error("[open_video] Could not copy the stream parameters.");
        return STATUS_CODE_NOK;
    }
    return STATUS_CODE_OK;
}

static void close_stream(AVFormatContext *oc, OutputStream *ost) {
    avcodec_free_context(&ost->enc);
    av_frame_free(&ost->frame);
}

void delete_file(char* file_name) {
    cop_debug("[delete_file].");

    if (file_name == NULL) {
        return;
    }
    remove(file_name);
}

void sender_stop(char* stop_ip) {
    cop_debug("[sender_stop] Source ip: %s.", stop_ip);

    changeStateInclIp(3, stop_ip);

    isAudioQuit = 1;
    isVideoQuit = 1;

    proxy_close();

    if (output_context_cam == NULL) {
        cop_debug("[sender_stop] cam: Output context not set. Do nothing.");
        changeStateInclIp(0, stop_ip);
        return;
    }

    if (output_context_mic == NULL) {
        cop_debug("[sender_stop] mic: Output context not set. Do nothing.");
        changeStateInclIp(0, stop_ip);
        return;
    }

    while (isAudioProcessing == 1 || isVideoProcessing == 1) {
        cop_debug("[sender_stop] Wait until audio and video finished processing.");
        sleep(1);
    }

    cop_debug("[sender_stop] Audio and video is idle.");

    int threadReturnValue;

    SDL_WaitThread(video_thread, &threadReturnValue);
    cop_debug("[sender_stop] Stop thread: %d.", threadReturnValue);
    SDL_WaitThread(audio_thread, &threadReturnValue);
    cop_debug("[sender_stop] Stop thread: %d.", threadReturnValue);

    /*cop_debug("[sender_stop] Detach video thread");
    SDL_DetachThread(video_thread);
    cop_debug("[sender_stop] Detach audio thread");
    SDL_DetachThread(audio_thread);*/

    cop_debug("[sender_stop] Write trailer.");

    /* Write the trailer, if any. The trailer must be written before you
     * close the CodecContexts open when you wrote the header; otherwise
     * av_write_trailer() may try to use memory that was freed on
     * av_codec_close().*/
    av_write_trailer(output_context_cam);
    av_write_trailer(output_context_mic);

    if (have_video) {
        cop_debug("[sender_stop] Close video stream.");
        close_stream(output_context_cam, video_st);
        avformat_close_input(&pCamFormatCtx);
    }
    if (have_audio) {
        cop_debug("[sender_stop] Close audio stream.");
        close_stream(output_context_mic, audio_st);
        // Attention: We may not close the mic context since it's started in main()
        //avformat_close_input(&pMicFormatCtx);
    }

    cop_debug("[sender_stop] Close avio.");

    avio_closep(&output_context_cam->pb);
    avio_closep(&output_context_mic->pb);

    cop_debug("[sender_stop] Free context.");

    /* free the stream */
    avformat_free_context(output_context_cam);
    avformat_free_context(output_context_mic);

    cop_debug("[sender_stop] Change state.");

    changeStateInclIp(0, stop_ip);

    cop_debug("[sender_stop] Done.");
}

int sender_initialize(char* url_cam, char* url_mic) {
    cop_debug("[sender_initialize].");

    changeState(1);

    isAudioQuit = 0;
    isVideoQuit = 0;

    Container *container = NULL;
    
    AVOutputFormat *output_format_cam = NULL;
    AVOutputFormat *output_format_mic = NULL;
    AVCodec *audio_codec;
    AVCodec *video_codec;
    int ret;
    have_video = 0;
    have_audio = 0;
    AVDictionary *opt = NULL;

    // Allocate the output media context (cam)
    avformat_alloc_output_context2(&output_context_cam, NULL, "mpegts", url_cam);
    if (!output_context_cam) {
        cop_error("[sender_initialize] cam: Can't allocate output context.");
        changeState(0);
        return STATUS_CODE_CANT_ALLOCATE;
    }
    output_format_cam = output_context_cam->oformat;

    // Allocate the output media context (mic)
    avformat_alloc_output_context2(&output_context_mic, NULL, "mpegts", url_mic);
    if (!output_context_mic) {
        cop_error("[sender_initialize] mic: Can't allocate output context.");
        changeState(0);
        return STATUS_CODE_CANT_ALLOCATE;
    }
    output_format_mic = output_context_mic->oformat;

    // Add the audio and video streams.
    if (output_format_cam->video_codec != AV_CODEC_ID_NONE && pCamName != NULL) {
        // Default: outputFormat->video_codec (=2, AV_CODEC_ID_MPEG2VIDEO) instead of AV_CODEC_ID_H264 (=28)
        add_stream(video_st, output_context_cam, &video_codec, AV_CODEC_ID_H264);
        have_video = 1;
    }
    if (output_format_mic->audio_codec != AV_CODEC_ID_NONE && pMicName != NULL) {
        // Default: outputFormat->audio_codec (=86016) is equal to AV_CODEC_ID_MP2 (=86016)
        add_stream(audio_st, output_context_mic, &audio_codec, AV_CODEC_ID_MP2);
        have_audio = 1;
    }

    cop_debug("[sender_initialize] Video: %d, Audio: %d", have_video, have_audio);

    // Now that all the parameters are set, we can open the audio and
    // video codecs and allocate the necessary encode buffers.
    if (have_video) {
        open_video(output_context_cam, video_codec, video_st, opt);
    }
    if (have_audio) {
        open_audio(output_context_mic, audio_codec, audio_st, opt);
    }

    av_dump_format(output_context_cam, 0, url_cam, 1);
    av_dump_format(output_context_mic, 0, url_mic, 1);

    // Open the output
    if (have_video) {
        ret = avio_open(&output_context_cam->pb, url_cam, AVIO_FLAG_WRITE);
        if (ret < 0) {
            cop_error("[sender_initialize] cam: Can't open '%s': %s.", url_cam, av_err2str(ret));
            changeState(0);
            return STATUS_CODE_CANT_ALLOCATE;
        }
        // Write the stream header
        ret = avformat_write_header(output_context_cam, &opt);
        if (ret < 0) {
            cop_error("[sender_initialize] cam: Can't write header: %s.", av_err2str(ret));
            changeState(0);
            return STATUS_CODE_NOK;
        }
    }
    if (have_audio) {
        ret = avio_open(&output_context_mic->pb, url_mic, AVIO_FLAG_WRITE);
        if (ret < 0) {
            cop_error("[sender_initialize] mic: Can't open '%s': %s.", url_mic, av_err2str(ret));
            changeState(0);
            return STATUS_CODE_CANT_ALLOCATE;
        }

        ret = avformat_write_header(output_context_mic, &opt);
        if (ret < 0) {
            cop_error("[sender_initialize] mic: Can't write header: %s.", av_err2str(ret));
            changeState(0);
            return STATUS_CODE_NOK;
        }
    }
    
    /*
     * Video
     */
    if (have_video == 1) {
        cop_debug("[sender_initialize] Setup video.");
        pCamFormatCtx = avformat_alloc_context();
        //pCamFormatCtx->interrupt_callback.callback = interrupt_cb;
        //pCamFormatCtx->interrupt_callback.opaque = pCamFormatCtx;

        if (equals(platform, "linux")) {
            pCamInputFormat = av_find_input_format("video4linux");
        } else {
            pCamInputFormat = av_find_input_format("avfoundation");
        }
        av_dict_set(&pCamOpt, "video_size", concat(concat(int_to_str(cfg_width), "x"), int_to_str(cfg_height)), 0);
        av_dict_set(&pCamOpt, "framerate", int_to_str(cfg_framerate), 0);
        
        ret = avformat_open_input(&pCamFormatCtx, pCamName, pCamInputFormat, &pCamOpt);
        if (ret != 0) {
            cop_error("[sender_initialize] Camera: Can't open format: %d.", ret);
            changeState(0);
            return STATUS_CODE_NOK;
        }

        if (avformat_find_stream_info(pCamFormatCtx, NULL) < 0) {
            cop_error("[sender_initialize] Camera: Can't find stream information.");
            changeState(0);
            return STATUS_CODE_NOK;
        }
        
        av_dump_format(pCamFormatCtx, 0, pCamName, 0);
        for(int i=0; i<pCamFormatCtx->nb_streams; i++) {
            if(pCamFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                camVideoStreamIndex = i;
                break;
            }
        }
        if (camVideoStreamIndex == -1) {
            changeState(0);
            return STATUS_CODE_NOK;
        }

        pCamCodec = avcodec_find_decoder(pCamFormatCtx->streams[camVideoStreamIndex]->codecpar->codec_id);
        if (pCamCodec==NULL) {
            cop_error("[sender_initialize] Codec %d not found.", pCamFormatCtx->streams[camVideoStreamIndex]->codecpar->codec_id);
            changeState(0);
            return STATUS_CODE_NOK;
        }
        
        pCamCodecCtx = avcodec_alloc_context3(pCamCodec);
        if (avcodec_parameters_to_context(pCamCodecCtx, pCamFormatCtx->streams[camVideoStreamIndex]->codecpar) < 0) {
            cop_error("[sender_initialize] Failed to copy video codec parameters to decoder context.");
            changeState(0);
            return STATUS_CODE_CANT_COPY_CODEC;
        }
        
        if (avcodec_open2(pCamCodecCtx, pCamCodec, NULL) < 0) {
            cop_error("[sender_initialize] Can't open camera codec.");
            changeState(0);
            return STATUS_CODE_CANT_OPEN;
        }
        pCamFrame = av_frame_alloc();
        
        pCamSwsContext = sws_getContext(pCamCodecCtx->width, pCamCodecCtx->height,
                                        pCamCodecCtx->pix_fmt,
                                        video_st->enc->width, video_st->enc->height,
                                        video_st->enc->pix_fmt,
                                        SWS_BICUBIC, NULL, NULL, NULL);
        if (!pCamSwsContext) {
            cop_error("[sender_initialize] Could not initialize the conversion context.");
            changeState(0);
            return STATUS_CODE_NOK;
        }
        
        uint8_t *picbuf;
        int picbuf_size = av_image_get_buffer_size(video_st->enc->pix_fmt,
                                                video_st->enc->width,
                                                video_st->enc->height,
                                                16);
        picbuf = (uint8_t*)av_malloc(picbuf_size);
        newpicture = av_frame_alloc();
        
        av_image_fill_arrays (newpicture->data, newpicture->linesize, picbuf, video_st->enc->pix_fmt, video_st->enc->width, video_st->enc->height, 1);
    }

    /*
     * Audio
     */
    if (have_audio == 1) {

        cop_debug("[sender_initialize] Setup audio.");
        av_dump_format(pMicFormatCtx, 0, pMicName, 0);
        for (int i=0; i<pMicFormatCtx->nb_streams; i++) {
            if (pMicFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                camAudioStreamIndex = i;
                break;
            }
        }
        cop_debug("[sender_initialize] Audio stream index: %d.", camAudioStreamIndex);
        if (camAudioStreamIndex == -1) {
            changeState(0);
            return STATUS_CODE_NOK;
        }
        
        cop_debug("[sender_initialize] Calling avcodec_find_decoder().");
        pMicCodec = avcodec_find_decoder(pMicFormatCtx->streams[camAudioStreamIndex]->codecpar->codec_id);
        if (pMicCodec==NULL) {
            cop_error("[sender_initialize] Codec %d not found.", pMicFormatCtx->streams[camAudioStreamIndex]->codecpar->codec_id);
            changeState(0);
            return STATUS_CODE_NOK;
        }
        
        cop_debug("[sender_initialize] Calling avcodec_alloc_context3().");
        pMicCodecCtx = avcodec_alloc_context3(pMicCodec);
        if (avcodec_parameters_to_context(pMicCodecCtx, pMicFormatCtx->streams[camAudioStreamIndex]->codecpar) < 0) {
            cop_error("[sender_initialize] Failed to copy audio codec parameters to decoder context.");
            changeState(0);
            return STATUS_CODE_CANT_COPY_CODEC;
        }
        
        cop_debug("[sender_initialize] Calling avcodec_open2().");
        if (avcodec_open2(pMicCodecCtx, pMicCodec, NULL) < 0) {
            cop_error("[sender_initialize] Can't open audio codec");
            changeState(0);
            return STATUS_CODE_CANT_OPEN;
        }
        
        cop_debug("[sender_initialize] Calling av_frame_alloc().");
        decoded_frame = av_frame_alloc();
        if (!decoded_frame) {
            cop_error("[sender_initialize] Could not allocate audio frame.");
            changeState(0);
            return STATUS_CODE_CANT_ALLOCATE;
        }
        
        // AUDIO: Output
        // Channel layout: 3 = STEREO (LEFT | RIGHT)
        // Sample rate: 44100
        // Sample format: 1 = AV_SAMPLE_FMT_S16
        // Number of samples per channel: 1152 (for mp2) from output stream encoder
        int64_t dst_channel_layout = AV_CH_LAYOUT_STEREO;
        int dst_sample_rate = 44100;
        enum AVSampleFormat dst_sample_fmt = AV_SAMPLE_FMT_S16;
        int nb_samples = audio_st->enc->frame_size;
        
        final_frame = av_frame_alloc();
        final_frame->channel_layout = dst_channel_layout;
        final_frame->sample_rate = dst_sample_rate;
        final_frame->format = dst_sample_fmt;
        final_frame->nb_samples = nb_samples;

        cop_debug("[sender_initialize] Calling av_frame_get_buffer().");
        ret = av_frame_get_buffer(final_frame, 0);
        if (ret < 0) {
            cop_error("[sender_initialize] Error allocating an audio buffer.");
            changeState(0);
            return STATUS_CODE_NOK;
        }
        // Create resampler context
        cop_debug("[sender_initialize] Calling swr_alloc().");
        swr_ctx = swr_alloc();
        if (!swr_ctx) {
            cop_error("[sender_initialize] Could not allocate resampler context.");
            changeState(0);
            return STATUS_CODE_NOK;
        }

        cop_debug("[sender_initialize] Channel layout: %lu, Sample rate: %d, Sample format: %d", pMicCodecCtx->channel_layout, pMicCodecCtx->sample_rate, pMicCodecCtx->sample_fmt);
        
        // AUDIO: Input
        // Channel layout: 3 = STEREO (LEFT | RIGHT)
        // Sample rate: 44100
        // Sample format: 3 = AV_SAMPLE_FMT_FLT
        av_opt_set_int(swr_ctx, "in_channel_count",     2, 0);
        av_opt_set_int(swr_ctx, "in_channel_layout",    pMicCodecCtx->channel_layout, 0);
        av_opt_set_int(swr_ctx, "in_sample_rate",       pMicCodecCtx->sample_rate, 0);
        av_opt_set_sample_fmt(swr_ctx, "in_sample_fmt", pMicCodecCtx->sample_fmt, 0);
        av_opt_set_int(swr_ctx, "out_channel_layout",    dst_channel_layout, 0);
        av_opt_set_int(swr_ctx, "out_sample_rate",       dst_sample_rate, 0);
        av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt", dst_sample_fmt, 0);
        
        // Initialize the resampling context
        if ((ret = swr_init(swr_ctx)) < 0) {
            cop_error("[sender_initialize] Failed to initialize the resampling context.");
            changeState(0);
            return STATUS_CODE_NOK;
        }
    }
    
    write_mutex = SDL_CreateMutex();
    
    container = malloc(sizeof(Container));
    container->outputStreamAudio = audio_st;
    container->outputStreamVideo = video_st;
    container->format_context_cam = output_context_cam;
    container->format_context_mic = output_context_mic;
    if (have_video) {
        video_thread = SDL_CreateThread(write_video, "write_video", container);
    }
    if (have_audio) {
        audio_thread = SDL_CreateThread(write_audio, "write_audio", container);
    }
    
    changeState(2);

    return STATUS_CODE_OK;
}

static void execute_start(command_data* command_data) {
    cop_debug("[execute_start]");
    // Store stream by using proxy when starting app
    if (pCamName != NULL) {
        proxy_init_cam(encryptionPwd);
        SDL_CreateThread(proxy_receive_udp_cam, "proxy_receive_udp_cam", NULL);
    }
    if (pMicName != NULL) {
        proxy_init_mic(encryptionPwd);
        SDL_CreateThread(proxy_receive_udp_mic, "proxy_receive_udp_mic", NULL);
    }
    
    char* url_cam = "udp://";
    url_cam = concat(url_cam, LOCALHOST_IP);
    url_cam = concat(url_cam, ":");
    url_cam = concat(url_cam, int_to_str(PORT_PROXY_LISTEN_CAM));
    url_cam = concat(url_cam, MPEG_TS_OPTIONS);

    char* url_mic = "udp://";
    url_mic = concat(url_mic, LOCALHOST_IP);
    url_mic = concat(url_mic, ":");
    url_mic = concat(url_mic, int_to_str(PORT_PROXY_LISTEN_MIC));
    url_mic = concat(url_mic, MPEG_TS_OPTIONS);

    sender_initialize(url_cam, url_mic);

    if (command_data != NULL) {
        network_send_state(senderId, command_data->start_ip);
    }
}

static void execute_connect(command_data* command_data) {
    cop_debug("[execute_connect]");
    if (command_data->port_cam != -1) {
        proxy_connect_cam(command_data->ip, command_data->port_cam);
    }
    if (command_data->port_mic != -1) {
        proxy_connect_mic(command_data->ip, command_data->port_mic);
    }
    // Update previous state with new IP for 'send to'
    changeState(state);
}

static void execute_stop(command_data* command_data) {
    cop_debug("[execute_stop]");
    sender_stop(command_data->stop_ip);
}

static void execute_delete(command_data* command_data) {
    cop_debug("[execute_delete]");
    delete_file(command_data->file_name);
}

static void execute_reset(command_data* command_data) {
    cop_debug("[execute_reset]");
    proxy_remove_client_cam(command_data->reset_ip);
    proxy_remove_client_mic(command_data->reset_ip);
    // Update previous state with new IP for 'send to'
    changeStateInclIp(state, command_data->reset_ip);
}

void list_devices() {

    if (equals(platform, "mac")) {
        //Avfoundation: [video]:[audio]
        AVFormatContext *pFormatCtx = avformat_alloc_context();
        AVDictionary* options = NULL;
        av_dict_set(&options,"list_devices","true",0);
        AVInputFormat *iformat = av_find_input_format("avfoundation");
        cop_debug("=== avfoundation device list ===.");
        avformat_open_input(&pFormatCtx,"",iformat,&options);
        avformat_close_input(&pFormatCtx);
        cop_debug("================================.");
    } else if (equals(platform, "linux")) {
        AVFormatContext *pFormatCtx = avformat_alloc_context();
        AVDictionary* options = NULL;
        av_dict_set(&options,"list_devices","true",0);
        // TODO: Maybe try video4linux
        AVInputFormat *iformat = av_find_input_format("video4linux2");
        cop_debug("=== video4linux device list ===.");
        avformat_open_input(&pFormatCtx,"",iformat,&options);
        avformat_close_input(&pFormatCtx);
        cop_debug("===============================.");
    } else if (equals(platform, "win")) {
        AVFormatContext *pFormatCtx = avformat_alloc_context();
        AVDictionary* options = NULL;
        av_dict_set(&options,"list_devices","true",0);
        // TODO: Maybe try dshow
        AVInputFormat *iformat = av_find_input_format("vfwcap");
        cop_debug("=== video4linux device list ===.");
        avformat_open_input(&pFormatCtx,"",iformat,&options);
        avformat_close_input(&pFormatCtx);
        cop_debug("===============================.");
    } else {
        cop_debug("[list_devices] Unknown platform: %s.", platform);
    }
}

/*
 * Usage: ./cop_sender -platform=mac|linux|win -cmd=start|list -cam=[name] -mic=[name]
 * ./cop_sender -platform=mac -cmd=list
 * ./cop_sender -platform=mac -cmd=start -cam="FaceTime HD Camera" -mic=":Built-in Microphone" -pwd="do-IT-my-way"
 * ./cop_sender -platform=mac -cmd=start -cam="FaceTime HD Camera" -mic=":AK5371" -pwd="do-IT-my-way"
 * ./cop_sender -platform=mac -cmd=start -cam="FaceTime HD Camera" -mic=":Built-in Microphone"
 * ./cop_sender -platform=mac -cmd=start -cam="FaceTime HD Camera" -width="960" -height="480" -framerate="30"
 * ./cop_sender -platform=mac -cmd=start -cam="FaceTime HD Camera"
 * ./cop_sender -platform=mac -cmd=start -cam="Capture screen 0" -mic=":Built-in Microphone"
 * 
 * ./ffmpeg -f avfoundation -framerate 30 -i 0 -c:v h264 -flags:v +global_header -bsf:v dump_extra -f rawvideo udp://127.0.0.1:1234
 */
int main(int argc, char* argv[]) {

    // Register all devices (camera, microphone, screen, etc.)
    avdevice_register_all();
    // Initialize networking
    avformat_network_init();

    char* platformParam = NULL;
    char* cmd = NULL;
    char* cam = NULL;
    char* mic = NULL;
    char* pwd = NULL;
    char* width = NULL;
    char* height = NULL;
    char* framerate = NULL;

    bool isPlatform = false;
    bool isCmd = false;
    bool isCam = false;
    bool isMic = false;
    bool isPwd = false;
    bool isWidth = false;
    bool isHeight = false;
    bool isFramerate = false;

    for (int i = 1; i < argc; i++) {
        char* param = argv[i];

        char* token;
        while ((token = strsep(&param, "=")) != NULL) {

            if (isPlatform) {
                platformParam = token;
                isPlatform = false;
            }
            if (equals(token, "-platform")) {
                isPlatform = true;
            }

            if (isCmd) {
                cmd = token;
                isCmd = false;
            }
            if (equals(token, "-cmd")) {
                isCmd = true;
            }

            if (isCam) {
                cam = token;
                isCam = false;
            }
            if (equals(token, "-cam")) {
                isCam = true;
            }

            if (isMic) {
                mic = token;
                isMic = false;
            }
            if (equals(token, "-mic")) {
                isMic = true;
            }

            if (isPwd) {
                pwd = token;
                isPwd = false;
            }
            if (equals(token, "-pwd")) {
                isPwd = true;
            }

            if (isWidth) {
                width = token;
                isWidth = false;
            }
            if (equals(token, "-width")) {
                isWidth = true;
            }

            if (isHeight) {
                height = token;
                isHeight = false;
            }
            if (equals(token, "-height")) {
                isHeight = true;
            }

            if (isFramerate) {
                framerate = token;
                isFramerate = false;
            }
            if (equals(token, "-framerate")) {
                isFramerate = true;
            }
        }
    }

    if (platformParam == NULL || cmd == NULL) {
        cop_debug("[main] ./cop_sender -platform=mac|linux|win -cmd=start|list -cam=[name] -mic=[name].");
        return STATUS_CODE_OK;
    }

    platform = platformParam;

    cop_debug("[main] Arguments: %s %s.", platform, cmd);

    if (equals(cmd, "list")) {
        list_devices();
        return STATUS_CODE_OK;
    }

    pCamName = cam;
    pMicName = mic;

    if (pCamName == NULL) {
        cop_debug("[main] Ignoring video.");    
    } else {
        cop_debug("[main] Camera: %s.", pCamName);
    }
    if (pMicName == NULL) {
        cop_debug("[main] Ignoring audio.");    
    } else {
        cop_debug("[main] Audio: %s.", pMicName);
    }

    if (pCamName != NULL && pMicName != NULL) {
        cop_error("[main] It's only possible to specify cam OR mic name");
        return STATUS_CODE_OK;
    }

    encryptionPwd = pwd;

    if (encryptionPwd == NULL) {
        cop_debug("[main] No password set.");    
    } else {
        cop_debug("[main] Encryption password set.");
    }

    if (width == NULL) {
        cop_debug("[main] No width set. Use default: %d.", CFG_WIDTH);
        cfg_width = CFG_WIDTH;
    } else {
        cop_debug("[main] Override width: %s.", width);
        cfg_width = str_to_int(width);
    }

    if (height == NULL) {
        cop_debug("[main] No height set. Use default: %d.", CFG_HEIGHT);
        cfg_height = CFG_HEIGHT;
    } else {
        cop_debug("[main] Override height: %s.", height);
        cfg_height = str_to_int(height);
    }

    if (framerate == NULL) {
        cop_debug("[main] No framerate set. Use default: %d.", CFG_FRAME_RATE);
        cfg_framerate = CFG_FRAME_RATE;
    } else {
        cop_debug("[main] Override framerate: %s.", framerate);
        cfg_framerate = str_to_int(framerate);
    }

    senderId = rand_str(32);

    changeState(0);

    audio_st = malloc(sizeof(OutputStream));
    video_st = malloc(sizeof(OutputStream));

    if (pMicName != NULL) {
        /*
        * Calling av_find_input_format() needs to be done on main thread
        * since otherwise you'll get the error 'audio format not found'.
        */
        cop_debug("[main] Open audio format.");
        pMicFormatCtx = avformat_alloc_context();
        if (equals(platform, "linux")) {
            pMicInputFormat = av_find_input_format("alsa");
        } else {
            pMicInputFormat = av_find_input_format("avfoundation");
        }
        
        if (avformat_open_input(&pMicFormatCtx, pMicName, pMicInputFormat, &pMicOpt) != 0) {
            // Error: Can't find mic
            cop_error("[sender_initialize] Mic: Can't open format. Ignore audio.");
            pMicName = NULL;
        } else {
            // Success: Found mic
            cop_debug("[main] Calling avformat_find_stream_info().");
            if (avformat_find_stream_info(pMicFormatCtx, NULL) < 0) {
                cop_error("[main] Mic: Can't find stream information.");
                return STATUS_CODE_NOK;
            }
            cop_debug("[main] Setup audio: Done");
        }
    }

    signal(SIGINT, intHandler);
    
    signal(SIGPIPE, sigPipeHandler);
    signal(EPIPE, ePipeHandler);

    if (TEST_LOCAL) {
        if (USE_PROXY) {
            proxy_init_cam(encryptionPwd);
            proxy_init_mic(encryptionPwd);
            SDL_CreateThread(proxy_receive_udp_cam, "proxy_receive_udp_cam", NULL);
            SDL_CreateThread(proxy_receive_udp_mic, "proxy_receive_udp_mic", NULL);

            // Test without UDP commands
            //sender_initialize("udp://192.168.0.24:1234", 640, 480, 10);
            // Packet size is 1472 by default which is the limit for VPN packets
            // If this makes problem you can set the pkt_size url parameter like
            // in this example: "udp://192.168.0.24:1235?pkt_size=1472"
            //sender_initialize("udp://192.168.0.24:1235", 640, 480, 10);
            //sender_initialize(concat("udp://127.0.0.1:", int_to_str(PORT_PROXY_LISTEN)), CFG_WIDTH, CFG_HEIGHT, CFG_FRAME_RATE);
            sender_initialize(
                concat(
                    concat(
                        concat("udp://", LOCALHOST_IP),
                        concat(":", int_to_str(PORT_PROXY_LISTEN_CAM))
                    ),
                    MPEG_TS_OPTIONS
                ),
                concat(
                    concat(
                        concat("udp://", LOCALHOST_IP),
                        concat(":", int_to_str(PORT_PROXY_LISTEN_MIC))
                    ),
                    MPEG_TS_OPTIONS
                ));
        } else {
            sender_initialize(
                concat("udp://192.168.0.24:1234", MPEG_TS_OPTIONS),
                concat("udp://192.168.0.24:1235", MPEG_TS_OPTIONS)
            );
        }
    } else {

        container_config* container = malloc(sizeof(container_config));
        container->cb_start = &execute_start;
        container->cb_connect = &execute_connect;
        container->cb_stop = &execute_stop;
        container->cb_delete = &execute_delete;
        container->cb_reset = &execute_reset;

        system_config* config = malloc(sizeof(system_config));
        config->senderId = senderId;
        config->width = cfg_width;
        config->height = cfg_height;
        config->has_video = pCamName == NULL ? 0 : 1;
        config->has_audio = pMicName == NULL ? 0 : 1;

        container->system_config = config;

        SDL_CreateThread(network_receive_tcp, "network_receive_tcp", container);

        execute_start(NULL);
    }

    while (quit == 0) {
        cop_debug("[main] Waiting for quit signal. State: %d.", state);
        logStats();
        sleep(1);
    }

    return STATUS_CODE_OK;
}
