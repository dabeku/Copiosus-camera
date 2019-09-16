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

#include <SDL2/SDL_thread.h>

#include "cop_network.h"
#include "cop_status_code.h"
#include "cop_utility.h"

#define USE_PROXY 1
#define TEST_LOCAL 0

#define CFG_WIDTH 640
#define CFG_HEIGHT 480
#define CFG_FRAME_RATE 10

static const char* LOCALHOST_IP = "127.0.0.1";
static const char* MPEG_TS_OPTIONS = "?pkt_size=1472";

// A global quit flag: 0 = running, 1 = quit
static int quit = 0;
static int isAudioQuit = 1;
static int isVideoQuit = 1;
static int isAudioProcessing = 0;
static int isVideoProcessing = 0;

static char* senderId = NULL;

// 0 = idle
// 1 = initializing
// 2 = connected
// 3 = disconnecting
static int state = 0;

static broadcast_data* last_broadcast_data = NULL;

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
int src_nb_samples = 512;
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

    //struct SwsContext *sws_ctx;
} OutputStream;

typedef struct Container {
    OutputStream *outputStreamAudio;
    OutputStream *outputStreamVideo;
    AVFormatContext *formatContext;
} Container;

// Used here to free memory when closing app
AVFormatContext *outputContext = NULL;
OutputStream *video_st = NULL;
OutputStream *audio_st = NULL;
int have_video = 0;
int have_audio = 0;

void intHandler(int dummy) {
    cop_debug("[intHandler] Closing app.");
    proxy_close();
    quit = 1;
}

static int interrupt_cb(void *ctx) {
    cop_debug("[interrupt_cb] Stop due to interrupt poll.");
    return 0;
}

static void sendState(broadcast_data* broadcast_data) {
    if (state == 0) {
        const char* msg = concat("IDLE ", senderId);
        size_t msg_length = strlen(msg);
        network_send_udp(msg, msg_length, broadcast_data);
    } else if (state == 1) {
        const char* msg = concat("INITIALIZING ", senderId);
        size_t msg_length = strlen(msg);
        network_send_udp(msg, msg_length, broadcast_data);
    } else if (state == 2) {
        const char* msg = concat("CONNECTED ", senderId);
        size_t msg_length = strlen(msg);
        network_send_udp(msg, msg_length, broadcast_data);
    } else if (state == 3) {
        const char* msg = concat("DISCONNECTING ", senderId);
        size_t msg_length = strlen(msg);
        network_send_udp(msg, msg_length, broadcast_data);
    } else {
        const char* msg = concat("UNKNOWN ", senderId);
        size_t msg_length = strlen(msg);
        network_send_udp(msg, msg_length, broadcast_data);
    }
}

static void changeState(int newState) {
    state = newState;

    if (last_broadcast_data != NULL) {
        sendState(last_broadcast_data);
    }
}

static int receive_broadcast(void* arg) {
    cop_debug("[receive_broadcast].");
    while (quit == 0) {
        broadcast_data* broadcast_data = network_receive_udp_broadcast(PORT_SCAN_BROADCAST);
        if (broadcast_data != NULL) {
            if (equals(broadcast_data->buffer, "SCAN")) {
                last_broadcast_data = broadcast_data;
                sendState(broadcast_data);
            } else {
                cop_debug("[receive_broadcast] IGNORE: %s", broadcast_data->buffer);
            }
        } else {
            // Sleep a little bit so we don't flood the logs
            sleep(1);
        }
    }

    cop_debug("[receive_broadcast] Done.");
    return 0;
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

static int write_frame(AVFormatContext *fmt_ctx, const AVRational *time_base, AVStream *st, AVPacket *pkt) {
    
    /* rescale output packet timestamp values from codec to stream timebase */
    //av_packet_rescale_ts(pkt, *time_base, st->time_base);
    pkt->stream_index = st->index;
    
    if (st->index == 1) {
        // Rescale audio pts from 1152 to 2351
        av_packet_rescale_ts(pkt, *time_base, st->time_base);
    }
    //cop_debug("PTS (stream: %d): %lld\n", st->index, pkt->pts);

    SDL_LockMutex(write_mutex);
    /* Write the compressed frame to the media file. */
    //log_packet(fmt_ctx, pkt);
//    return av_interleaved_write_frame(fmt_ctx, pkt);
    int result = av_write_frame(fmt_ctx, pkt);
    
    SDL_UnlockMutex(write_mutex);
    
    return result;
}

// Add an output stream
static int add_stream(OutputStream *ost, AVFormatContext *oc, AVCodec **codec, enum AVCodecID codec_id) {
    AVCodecContext *c = NULL;
    int i;

    /* find the encoder */
    *codec = avcodec_find_encoder(codec_id);
    if (!(*codec)) {
        cop_error("[add_stream] Could not find encoder for '%s'.", avcodec_get_name(codec_id));
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
        /* Resolution must be a multiple of two. */
        c->width    = cfg_width;
        c->height   = cfg_height;
        /* timebase: This is the fundamental unit of time (in seconds) in terms
         * of which frame timestamps are represented. For fixed-fps content,
         * timebase should be 1/framerate and timestamp increments should be
         * identical to 1. */
        ost->st->time_base = (AVRational){ 1, cfg_framerate };
        c->time_base       = ost->st->time_base;

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

    /* Some formats want stream headers to be separate. */
    if (oc->oformat->flags & AVFMT_GLOBALHEADER) {
        c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    
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
        if (micPacket.stream_index == camAudioStreamIndex) {
            int micFrameFinished = 0;
            
            ret = decode_audio(pMicCodecCtx, decoded_frame, &micPacket, &micFrameFinished);
            if (ret < 0) {
                cop_error("Error in decoding audio frame.");
                av_packet_unref(&micPacket);
                continue;
            }
            
            av_packet_unref(&micPacket);
            
            if (micFrameFinished) {
                src_data = decoded_frame->data;

                int dst_bufsize_before = av_samples_get_buffer_size(NULL, 2,
                                                 src_nb_samples, AV_SAMPLE_FMT_FLT, 0);
                //cop_debug("dst_bufsize2: %d", dst_bufsize2);
                FILE* dst_file = fopen("raw_before.mic", "ab");
                fwrite(src_data[0], 1, dst_bufsize_before, dst_file);
                fclose(dst_file);

                // Use swr_convert() as FIFO: Put in some data
                int outSamples = swr_convert(swr_ctx, NULL, 0, (const uint8_t **)src_data, src_nb_samples);

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

                    int dst_bufsize = av_samples_get_buffer_size(NULL, nb_channels,
                                                 outSamples, AV_SAMPLE_FMT_S16, 0);
                    //cop_debug("dst_bufsize: %d", dst_bufsize);
                    FILE* dst_file = fopen("raw_after.mic", "ab");
                    fwrite(final_frame->data[0], 1, dst_bufsize, dst_file);
                    fclose(dst_file);

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
        // TODO: Add interrupt like in node module (when changing network while cop_sender is running
        // it will remain in state: 3)
        int ret = av_read_frame(pCamFormatCtx, &camPacket);
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

                // This is mpegts specific
                int64_t pts_diff = (1.0 / cfg_framerate) * 90000;
                ost->next_pts = ost->next_pts + pts_diff;
                ost->frame->pts = ost->next_pts;
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
        ret = write_frame(oc, &c->time_base, ost->st, &pkt);
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
        house_keeping(get_video_file_name());
    }
}

/*Uint32 periodic_cb(Uint32 interval, void *param) {
    logStats();
    return(interval);
}*/

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

    // Encode the image
    ret = encode_video(c, frame, &pkt, &got_packet);
    
    if (ret < 0) {
        cop_error("[write_video_frame] Error encoding video frame: %s.", av_err2str(ret));
        return STATUS_CODE_NOK;
    }
    
    if (got_packet) {
        ret = write_frame(oc, &c->time_base, ost->st, &pkt);
        av_packet_unref(&pkt);
    } else {
        ret = 0;
    }

    if (ret < 0) {
        cop_error("[write_video_frame] Error while writing video frame: %s.", av_err2str(ret));
        return STATUS_CODE_NOK;
    }

    return (frame || got_packet) ? 0 : 1;
}

int write_audio(void *arg) {
    Container *container = (Container *)arg;
    while (isAudioQuit == 0) {
        isAudioProcessing = 1;
        write_audio_frame(container->formatContext, container->outputStreamAudio);
        isAudioProcessing = 0;
    }
    return STATUS_CODE_OK;
}

int write_video(void *arg) {
    Container *container = (Container *)arg;
    while (isVideoQuit == 0) {
        isVideoProcessing = 1;
        write_video_frame(container->formatContext, container->outputStreamVideo);
        isVideoProcessing = 0;
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
    cop_debug("[close_stream] %p.", &ost->enc);
    avcodec_free_context(&ost->enc);
    cop_debug("[close_stream] Calling av_frame_free(): %p.", &ost->frame);
    av_frame_free(&ost->frame);
    //cop_debug("[close_stream] Calling sws_freeContext(): %p.", ost->sws_ctx);
    //sws_freeContext(ost->sws_ctx);
}

void sender_stop() {
    cop_debug("[sender_stop].");

    changeState(3);

    isAudioQuit = 1;
    isVideoQuit = 1;

    proxy_close();

    if (outputContext == NULL) {
        cop_debug("[sender_stop] Output context not set. Do nothing.");
        changeState(0);
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
    av_write_trailer(outputContext);

    if (have_video) {
        cop_debug("[sender_stop] Close video stream.");
        close_stream(outputContext, video_st);
        avformat_close_input(&pCamFormatCtx);
    }
    if (have_audio) {
        cop_debug("[sender_stop] Close audio stream.");
        close_stream(outputContext, audio_st);
        // Attention: We may not close the mic context since it's started in main()
        //avformat_close_input(&pMicFormatCtx);
    }

    cop_debug("[sender_stop] Close avio.");

    //if (!(outputFormat->flags & AVFMT_NOFILE))
    avio_closep(&outputContext->pb);

    cop_debug("[sender_stop] Free context.");

    /* free the stream */
    avformat_free_context(outputContext);

    cop_debug("[sender_stop] Change state.");

    changeState(0);

    cop_debug("[sender_stop] Done.");
}

int sender_initialize(char* url, int width, int height, int framerate) {
    cop_debug("[sender_initialize].");

    changeState(1);

    isAudioQuit = 0;
    isVideoQuit = 0;
    
    // Config section
    cfg_framerate = framerate;
    cfg_width = width;
    cfg_height = height;
    
    Container *container = NULL;
    
    AVOutputFormat *outputFormat = NULL;
    AVCodec *audio_codec;
    AVCodec *video_codec;
    int ret;
    have_video = 0;
    have_audio = 0;
    AVDictionary *opt = NULL;

    // Allocate the output media context (mpeg-ts container)
    avformat_alloc_output_context2(&outputContext, NULL, "mpegts", url);
    if (!outputContext) {
        cop_error("[sender_initialize] Can't allocate output context.");
        changeState(0);
        return STATUS_CODE_CANT_ALLOCATE;
    }

    outputFormat = outputContext->oformat;

    // Add the audio and video streams.
    if (outputFormat->video_codec != AV_CODEC_ID_NONE && pCamName != NULL) {
        // Default: outputFormat->video_codec (=2, AV_CODEC_ID_MPEG2VIDEO) instead of AV_CODEC_ID_H264 (=28)
        //add_stream(&video_st, outputContext, &video_codec, AV_CODEC_ID_H264);
        add_stream(video_st, outputContext, &video_codec, AV_CODEC_ID_MPEG2VIDEO);
        have_video = 1;
    }
    if (outputFormat->audio_codec != AV_CODEC_ID_NONE && pMicName != NULL) {
        // Default: outputFormat->audio_codec (=86016) is equal to AV_CODEC_ID_MP2 (=86016)
        add_stream(audio_st, outputContext, &audio_codec, AV_CODEC_ID_MP2);
        have_audio = 1;
    }

    cop_debug("[sender_initialize] Video: %d, Audio: %d", have_video, have_audio);

    // Now that all the parameters are set, we can open the audio and
    // video codecs and allocate the necessary encode buffers.
    if (have_video) {
        open_video(outputContext, video_codec, video_st, opt);
    }
    if (have_audio) {
        open_audio(outputContext, audio_codec, audio_st, opt);
    }

    av_dump_format(outputContext, 0, url, 1);

    // Open the output
    //if (!(outputFormat->flags & AVFMT_NOFILE)) {
    ret = avio_open(&outputContext->pb, url, AVIO_FLAG_WRITE);
    if (ret < 0) {
        cop_error("[sender_initialize] Can't open '%s': %s.", url, av_err2str(ret));
        changeState(0);
        return STATUS_CODE_CANT_ALLOCATE;
    }
    //}

    // Write the stream header
    ret = avformat_write_header(outputContext, &opt);
    if (ret < 0) {
        cop_error("[sender_initialize] Can't write header: %s.", av_err2str(ret));
        changeState(0);
        return STATUS_CODE_NOK;
    }
    
    /*
     * Video
     */
    cop_debug("[sender_initialize] Setup video.");
    pCamFormatCtx = avformat_alloc_context();
    //pCamFormatCtx->interrupt_callback.callback = interrupt_cb;
    //pCamFormatCtx->interrupt_callback.opaque = pCamFormatCtx;

    pCamInputFormat = av_find_input_format("avfoundation");
    av_dict_set(&pCamOpt, "video_size", concat(concat(int_to_str(width), "x"), int_to_str(height)), 0);
    av_dict_set(&pCamOpt, "framerate", int_to_str(framerate), 0);
    //av_dict_set(&pCamOpt, "timeout", "5", 0); 
    //av_dict_set(&pCamOpt, "stimeout", "5", 0); 

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
        if (pCamCodec==NULL) {
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
    container->formatContext = outputContext;
    if (have_video) {
        video_thread = SDL_CreateThread(write_video, "write_video", container);
    }
    if (have_audio) {
        audio_thread = SDL_CreateThread(write_audio, "write_audio", container);
    }
    
    changeState(2);

    return STATUS_CODE_OK;
}

static int receive_command(void* arg) {
    cop_debug("[receive_command].");
    while (quit == 0) {
        command_data* command_data = network_receive_udp(PORT_COMMAND_CAMERA);

        if (command_data != NULL) {
            cop_debug("[receive_command] Received COMMAND: %s", command_data->cmd);

            if (equals(command_data->cmd, "CONNECT")) {

                if (USE_PROXY) {
                    proxy_init(command_data->ip, command_data->port, encryptionPwd);
                    SDL_CreateThread(proxy_receive_udp, "proxy_receive_udp", NULL);
                    char* url = concat("udp://", LOCALHOST_IP);
                    url = concat(url, ":");
                    url = concat(url, int_to_str(PORT_PROXY_LISTEN));
                    url = concat(url, MPEG_TS_OPTIONS);
                    sender_initialize(url, CFG_WIDTH, CFG_HEIGHT, CFG_FRAME_RATE);
                } else {
                    char* url = concat("udp://", command_data->ip);
                    url = concat(url, ":");
                    url = concat(url, int_to_str(command_data->port));
                    url = concat(url, MPEG_TS_OPTIONS);
                    sender_initialize(url, CFG_WIDTH, CFG_HEIGHT, CFG_FRAME_RATE);
                }
            } else if (equals(command_data->cmd, "DISCONNECT")) {
                cop_debug("[receive_command] Do disconnect");
                sender_stop();
            } else {
                cop_debug("[receive_command] IGNORE: %s", command_data->cmd);
            }
        } else {
            // Sleep a little bit so we don't flood the logs
            sleep(1);
        }
    }
    cop_debug("[receive_command] Done.");
    return 0;
}

void list_devices(char* platform) {

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
 * ./cop_sender -platform=mac -cmd=start -cam="FaceTime HD Camera"
 * ./cop_sender -platform=mac -cmd=start -cam="Capture screen 0" -mic=":Built-in Microphone"
 */
int main(int argc, char* argv[]) {

    // Register all devices (camera, microphone, screen, etc.)
    avdevice_register_all();
    // Initialize networking
    avformat_network_init();

    char* platform = NULL;
    char* cmd = NULL;
    char* cam = NULL;
    char* mic = NULL;
    char* pwd = NULL;

    bool isPlatform = false;
    bool isCmd = false;
    bool isCam = false;
    bool isMic = false;
    bool isPwd = false;

    for (int i = 1; i < argc; i++) {
        char* param = argv[i];

        char* token;
        while ((token = strsep(&param, "=")) != NULL) {

            if (isPlatform) {
                platform = token;
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
        }
    }

    if (platform == NULL || cmd == NULL) {
        cop_debug("[main] ./cop_sender -platform=mac|linux|win -cmd=start|list -cam=[name] -mic=[name].");
        return STATUS_CODE_OK;
    }

    cop_debug("[main] Arguments: %s %s.", platform, cmd);

    if (equals(cmd, "list")) {
        list_devices(platform);
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

    encryptionPwd = pwd;

    if (encryptionPwd == NULL) {
        cop_debug("[main] No password set.");    
    } else {
        cop_debug("[main] Encryption password set.");
    }

    changeState(0);

    senderId = rand_str(32);

    audio_st = malloc(sizeof(OutputStream));
    video_st = malloc(sizeof(OutputStream));

    if (pMicName != NULL) {
        /*
        * Calling av_find_input_format() needs to be done on main thread
        * since otherwise you'll get the error 'audio format not found'.
        */
        cop_debug("[main] Open audio format.");
        pMicFormatCtx = avformat_alloc_context();
        pMicInputFormat = av_find_input_format("avfoundation");
        if (avformat_open_input(&pMicFormatCtx, pMicName, pMicInputFormat, &pMicOpt) != 0) {
            cop_error("[sender_initialize] Mic: Can't open format.");
            return STATUS_CODE_NOK;
        }

        cop_debug("[main] Calling avformat_find_stream_info().");
        if (avformat_find_stream_info(pMicFormatCtx, NULL) < 0) {
            cop_error("[main] Mic: Can't find stream information.");
            return STATUS_CODE_NOK;
        }
        cop_debug("[main] Setup audio: Done");
    }

    //SDL_AddTimer(1000, periodic_cb, NULL);

    signal(SIGINT, intHandler);

    if (TEST_LOCAL) {
        if (USE_PROXY) {
            proxy_init("192.168.0.24", 1234, encryptionPwd);
            SDL_CreateThread(proxy_receive_udp, "proxy_receive_udp", NULL);

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
                        concat(":", int_to_str(PORT_PROXY_LISTEN))
                    ),
                    MPEG_TS_OPTIONS
                )
                , CFG_WIDTH, CFG_HEIGHT, CFG_FRAME_RATE);
        } else {
            sender_initialize(
                concat("udp://192.168.0.24:1234", MPEG_TS_OPTIONS),
                CFG_WIDTH, CFG_HEIGHT, CFG_FRAME_RATE
            );
        }
    } else {
        SDL_CreateThread(receive_broadcast, "receive_broadcast", NULL);
        SDL_CreateThread(receive_command, "receive_command", NULL);

        // Store stream by using proxy when starting app

        // TODO: Make port more reasonable
        proxy_init(LOCALHOST_IP, 9999, encryptionPwd);
        SDL_CreateThread(proxy_receive_udp, "proxy_receive_udp", NULL);
        char* url = "udp://";
        url = concat(url, LOCALHOST_IP);
        url = concat(url, ":");
        url = concat(url, int_to_str(PORT_PROXY_LISTEN));
        url = concat(url, MPEG_TS_OPTIONS);
        sender_initialize(url, CFG_WIDTH, CFG_HEIGHT, CFG_FRAME_RATE);
    }

    while (quit == 0) {
        cop_debug("[main] Waiting for quit signal. State: %d.", state);
        logStats();
        sleep(1);
    }

    return STATUS_CODE_OK;
}
