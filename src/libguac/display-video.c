/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include "config.h"
#include "display-priv.h"
#include "display-video.h"
#include "guacamole/client.h"
#include "guacamole/display.h"
#include "guacamole/mem.h"
#include "guacamole/protocol.h"
#include "guacamole/rect.h"
#include "guacamole/stream.h"
#include "guacamole/timestamp.h"
#include "guacamole/user.h"

#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/**
 * The x264 preset used for encoding. "superfast" keeps CABAC and the deblocking
 * filter (unlike "ultrafast"), which matters for text, at a small CPU cost.
 */
#define GUAC_DISPLAY_VIDEO_PRESET "superfast"

/**
 * Default constant rate factor for frames with motion. Lower is better
 * quality. Overridable with the GUACD_VIDEO_CRF environment variable.
 */
#define GUAC_DISPLAY_VIDEO_CRF 23

/**
 * Default bitrate cap (VBV max rate), in kbit/s. Overridable with
 * GUACD_VIDEO_MAXRATE.
 */
#define GUAC_DISPLAY_VIDEO_MAXRATE 30000

/**
 * Maximum number of changed regions sent with a frame. Beyond this, the
 * bounding box of all changes is sent instead.
 */
#define GUAC_DISPLAY_VIDEO_MAX_REGIONS 128

/**
 * Maximum number of frames between keyframes. Keyframes are otherwise only
 * sent when the stream (re)starts.
 */
#define GUAC_DISPLAY_VIDEO_GOP 3600

struct guac_display_video {

    /**
     * The encoder context, or NULL if the encoder has not been opened yet (or
     * must be reopened, as after a change in layer size).
     */
    AVCodecContext* context;

    /**
     * Conversion from the layer's 32-bit BGRX buffer to YUV 4:2:0.
     */
    struct SwsContext* sws;

    /**
     * The frame passed to the encoder.
     */
    AVFrame* frame;

    /**
     * Reusable packet for encoder output.
     */
    AVPacket* packet;

    /**
     * Width and height of the layer currently being encoded (the encoded
     * dimensions are these rounded up to even numbers).
     */
    int width;
    int height;

    /**
     * Copy of the layer as of the last encoded frame (32-bit pixels, rows of
     * width * 4 bytes), used to find exactly which cells changed.
     */
    unsigned char* previous;

    /**
     * The regions that changed in the frame being encoded, and how many.
     */
    guac_rect regions[GUAC_DISPLAY_VIDEO_MAX_REGIONS];
    int region_count;

    /**
     * The stream carrying the video, or NULL if no stream has been started.
     */
    guac_stream* stream;

    /**
     * Timestamp of the start of the stream; frame timestamps are relative to
     * this.
     */
    guac_timestamp stream_start;

    /**
     * The timestamp of the last frame given to the encoder, relative to
     * stream_start. Timestamps must be strictly increasing.
     */
    int64_t last_pts;

    /**
     * Whether the next frame must be a keyframe.
     */
    int force_keyframe;

    /**
     * When the stream should be restarted for a joining user, or zero if no
     * restart is pending. Guarded by restart_lock.
     */
    guac_timestamp restart_due;

    /**
     * Lock guarding restart_due, which is set by joining users outside the
     * frame locks.
     */
    pthread_mutex_t restart_lock;

    /**
     * Encoding settings.
     */
    int crf;
    int maxrate;

    /**
     * Number of frames encoded and bytes sent since the last statistics log.
     */
    int stats_frames;
    int64_t stats_bytes;
    guac_timestamp stats_start;

};

/**
 * Returns the integer value of the given environment variable if it is set
 * to a value within the given range, or the given default otherwise.
 */
static int guac_display_video_env_int(const char* name, int def, int min, int max) {

    const char* value = getenv(name);
    if (value == NULL || *value == '\0')
        return def;

    int parsed = atoi(value);
    if (parsed < min || parsed > max)
        return def;

    return parsed;

}

/**
 * Callback for guac_client_for_owner() which returns non-NULL if the owner
 * supports the video mimetype.
 */
static void* guac_display_video_owner_supported(guac_user* user, void* data) {

    if (user == NULL || user->info.video_mimetypes == NULL)
        return NULL;

    for (const char** mimetype = user->info.video_mimetypes; *mimetype != NULL; mimetype++) {
        if (strcmp(*mimetype, GUAC_DISPLAY_VIDEO_MIMETYPE) == 0)
            return (void*) user;
    }

    return NULL;

}

int guac_display_enable_video(guac_display* display, const char* codec) {

    guac_client* client = display->client;

    if (codec == NULL || strcmp(codec, "h264") != 0) {
        guac_client_log(client, GUAC_LOG_WARNING, "Unsupported video codec "
                "\"%s\". Graphical updates will be sent as images.",
                codec != NULL ? codec : "");
        return 1;
    }

    if (guac_client_for_owner(client, guac_display_video_owner_supported, NULL) == NULL) {
        guac_client_log(client, GUAC_LOG_INFO, "Client does not support "
                "\"%s\". Graphical updates will be sent as images.",
                GUAC_DISPLAY_VIDEO_MIMETYPE);
        return 1;
    }

    if (avcodec_find_encoder_by_name("libx264") == NULL) {
        guac_client_log(client, GUAC_LOG_WARNING, "H.264 encoder (libx264) "
                "not available. Graphical updates will be sent as images.");
        return 1;
    }

    guac_display_video* video = guac_mem_zalloc(sizeof(guac_display_video));
    pthread_mutex_init(&video->restart_lock, NULL);
    video->force_keyframe = 1;
    video->crf = guac_display_video_env_int("GUACD_VIDEO_CRF",
            GUAC_DISPLAY_VIDEO_CRF, 0, 51);
    video->maxrate = guac_display_video_env_int("GUACD_VIDEO_MAXRATE",
            GUAC_DISPLAY_VIDEO_MAXRATE, 1000, 500000);

    guac_rwlock_acquire_write_lock(&display->pending_frame.lock);
    guac_rwlock_acquire_write_lock(&display->last_frame.lock);
    display->video = video;
    guac_rwlock_release_lock(&display->last_frame.lock);
    guac_rwlock_release_lock(&display->pending_frame.lock);

    guac_client_log(client, GUAC_LOG_INFO, "Graphical updates of the default "
            "layer will be streamed as H.264 video (crf %i, max %i kbit/s), "
            "with lossless refresh of regions that stop changing.",
            video->crf, video->maxrate);

    return 0;

}

/**
 * Closes the encoder and frees everything tied to the current layer size,
 * leaving the stream itself alone.
 */
static void guac_display_video_close_encoder(guac_display_video* video) {

    avcodec_free_context(&video->context);
    av_frame_free(&video->frame);
    av_packet_free(&video->packet);
    sws_freeContext(video->sws);
    video->sws = NULL;
    guac_mem_free(video->previous);
    video->previous = NULL;
    video->width = 0;
    video->height = 0;

}

/**
 * Ends the current video stream, if any.
 */
static void guac_display_video_end_stream(guac_display* display) {

    guac_display_video* video = display->video;
    guac_client* client = display->client;

    if (video->stream != NULL) {
        guac_protocol_send_end(client->socket, video->stream);
        guac_client_free_stream(client, video->stream);
        video->stream = NULL;
    }

}

/**
 * Opens the encoder for a layer of the given size.
 *
 * @return
 *     Zero on success, non-zero on failure.
 */
static int guac_display_video_open_encoder(guac_display* display, int width, int height) {

    guac_display_video* video = display->video;
    guac_client* client = display->client;

    /* 4:2:0 requires even dimensions */
    int encoded_width = (width + 1) & ~1;
    int encoded_height = (height + 1) & ~1;

    const AVCodec* codec = avcodec_find_encoder_by_name("libx264");
    if (codec == NULL)
        return 1;

    AVCodecContext* context = avcodec_alloc_context3(codec);
    if (context == NULL)
        return 1;

    context->width = encoded_width;
    context->height = encoded_height;
    context->pix_fmt = AV_PIX_FMT_YUV420P;
    context->time_base = (AVRational) { 1, 1000 };
    context->framerate = (AVRational) { 60, 1 };
    context->gop_size = GUAC_DISPLAY_VIDEO_GOP;
    context->max_b_frames = 0;
    context->rc_max_rate = (int64_t) video->maxrate * 1000;
    context->rc_buffer_size = (int) ((int64_t) video->maxrate * 1000 / 2);
    context->thread_count = 0;
    context->colorspace = AVCOL_SPC_BT709;
    context->color_primaries = AVCOL_PRI_BT709;
    context->color_trc = AVCOL_TRC_BT709;
    context->color_range = AVCOL_RANGE_MPEG;

    av_opt_set(context->priv_data, "preset", GUAC_DISPLAY_VIDEO_PRESET, 0);
    av_opt_set(context->priv_data, "tune", "zerolatency", 0);
    av_opt_set(context->priv_data, "profile", "high", 0);
    av_opt_set_double(context->priv_data, "crf", video->crf, 0);
    av_opt_set_int(context->priv_data, "forced-idr", 1, 0);

    int err = avcodec_open2(context, codec, NULL);
    if (err < 0) {
        char message[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(err, message, sizeof(message));
        guac_client_log(client, GUAC_LOG_ERROR, "Unable to open H.264 "
                "encoder for %ix%i: %s", encoded_width, encoded_height, message);
        avcodec_free_context(&context);
        return 1;
    }

    AVFrame* frame = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();
    if (frame == NULL || packet == NULL) {
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&context);
        return 1;
    }

    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = encoded_width;
    frame->height = encoded_height;
    frame->colorspace = AVCOL_SPC_BT709;
    frame->color_range = AVCOL_RANGE_MPEG;
    if (av_frame_get_buffer(frame, 0) < 0) {
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&context);
        return 1;
    }

    /* Black padding for odd dimensions (luma 16, chroma 128 in limited range) */
    memset(frame->data[0], 16, (size_t) frame->linesize[0] * encoded_height);
    memset(frame->data[1], 128, (size_t) frame->linesize[1] * (encoded_height / 2));
    memset(frame->data[2], 128, (size_t) frame->linesize[2] * (encoded_height / 2));

    struct SwsContext* sws = sws_getContext(width, height, AV_PIX_FMT_BGR0,
            width, height, AV_PIX_FMT_YUV420P, SWS_POINT, NULL, NULL, NULL);
    if (sws == NULL) {
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&context);
        return 1;
    }

    /* Full-range RGB in, BT.709 limited-range YUV out (matches the VUI) */
    const int* coefficients = sws_getCoefficients(SWS_CS_ITU709);
    sws_setColorspaceDetails(sws, coefficients, 1, coefficients, 0,
            0, 1 << 16, 1 << 16);

    video->previous = guac_mem_zalloc(guac_mem_ckd_mul_or_die(width, height, 4));
    video->context = context;
    video->frame = frame;
    video->packet = packet;
    video->sws = sws;
    video->width = width;
    video->height = height;

    guac_client_log(client, GUAC_LOG_INFO, "H.264 encoder opened for %ix%i "
            "(encoded as %ix%i).", width, height, encoded_width, encoded_height);

    return 0;

}

/**
 * Starts a new video stream on the default layer. The next frame will be a
 * keyframe.
 */
static void guac_display_video_start_stream(guac_display* display) {

    guac_display_video* video = display->video;
    guac_client* client = display->client;

    video->stream = guac_client_alloc_stream(client);
    if (video->stream == NULL) {
        guac_client_log(client, GUAC_LOG_ERROR, "Unable to allocate a stream "
                "for H.264 video.");
        return;
    }

    guac_protocol_send_video(client->socket, video->stream,
            display->default_layer->layer, GUAC_DISPLAY_VIDEO_MIMETYPE);

    video->stream_start = guac_timestamp_current();
    video->last_pts = -1;
    video->force_keyframe = 1;

}

/**
 * Sends one encoded access unit within the current stream, prefixed by the
 * header described in display-video.h.
 */
static void guac_display_video_send_packet(guac_display* display, AVPacket* packet) {

    guac_display_video* video = display->video;
    guac_client* client = display->client;

    size_t header = GUAC_DISPLAY_VIDEO_HEADER_SIZE + (size_t) video->region_count * 8;
    size_t total = header + (size_t) packet->size;
    unsigned char* data = guac_mem_alloc(total);

    uint32_t length = (uint32_t) packet->size;
    uint32_t timestamp = (uint32_t) packet->pts;

    data[0] = (length >> 24) & 0xFF;
    data[1] = (length >> 16) & 0xFF;
    data[2] = (length >> 8) & 0xFF;
    data[3] = length & 0xFF;
    data[4] = (packet->flags & AV_PKT_FLAG_KEY) ? 1 : 0;
    data[5] = (timestamp >> 24) & 0xFF;
    data[6] = (timestamp >> 16) & 0xFF;
    data[7] = (timestamp >> 8) & 0xFF;
    data[8] = timestamp & 0xFF;
    data[9] = (video->region_count >> 8) & 0xFF;
    data[10] = video->region_count & 0xFF;

    /* Regions of the frame that changed (the only parts the client draws) */
    unsigned char* current = data + GUAC_DISPLAY_VIDEO_HEADER_SIZE;
    for (int i = 0; i < video->region_count; i++) {
        const guac_rect* rect = &video->regions[i];
        int values[4] = { rect->left, rect->top,
            guac_rect_width(rect), guac_rect_height(rect) };
        for (int v = 0; v < 4; v++) {
            *(current++) = (values[v] >> 8) & 0xFF;
            *(current++) = values[v] & 0xFF;
        }
    }

    memcpy(data + header, packet->data, packet->size);

    guac_protocol_send_blobs(client->socket, video->stream, data, (int) total);
    guac_mem_free(data);

    video->stats_bytes += total;

}

/**
 * Returns whether the given cell-aligned rectangle of the layer differs
 * between the layer's last frame and the copy of the last encoded frame.
 */
static int guac_display_video_rect_differs(guac_display* display, const guac_rect* rect) {

    guac_display_video* video = display->video;
    guac_display_layer* layer = display->default_layer;

    size_t row_length = (size_t) guac_rect_width(rect) * 4;
    size_t previous_stride = (size_t) video->width * 4;

    for (int y = rect->top; y < rect->bottom; y++) {
        const unsigned char* a = layer->last_frame.buffer
            + (size_t) y * layer->last_frame.buffer_stride + (size_t) rect->left * 4;
        const unsigned char* b = video->previous
            + (size_t) y * previous_stride + (size_t) rect->left * 4;
        if (memcmp(a, b, row_length) != 0)
            return 1;
    }

    return 0;

}

/**
 * Adds a horizontal run of changed cells to video->regions, extending a
 * region from the row above if it spans exactly the same columns.
 *
 * @return
 *     Non-zero if there was no room left for the run, zero otherwise.
 */
static int guac_display_video_add_region(guac_display_video* video, const guac_rect* run) {

    for (int i = video->region_count - 1; i >= 0; i--) {
        guac_rect* region = &video->regions[i];
        if (region->bottom == run->top && region->left == run->left
                && region->right == run->right) {
            region->bottom = run->bottom;
            return 0;
        }
    }

    if (video->region_count >= GUAC_DISPLAY_VIDEO_MAX_REGIONS)
        return 1;

    video->regions[video->region_count++] = *run;
    return 0;

}

/**
 * Finds the 64x64 cells within the given region that actually changed since
 * the last encoded frame, and stores them in video->regions as rectangles
 * (horizontal runs of changed cells, merged vertically where they line up).
 * The copy of the last encoded frame is then updated.
 */
static void guac_display_video_find_regions(guac_display* display,
        const guac_rect* dirty, const guac_rect* bounds, int full) {

    guac_display_video* video = display->video;
    guac_display_layer* layer = display->default_layer;
    size_t previous_stride = (size_t) video->width * 4;

    video->region_count = 0;

    if (full) {
        video->regions[0] = *bounds;
        video->region_count = 1;
    }

    else {

        guac_rect area = *dirty;
        guac_rect_align(&area, GUAC_DISPLAY_CELL_SIZE_EXPONENT);
        guac_rect_constrain(&area, bounds);

        int overflow = 0;

        for (int top = area.top; top < area.bottom; top += GUAC_DISPLAY_CELL_SIZE) {

            int bottom = top + GUAC_DISPLAY_CELL_SIZE;
            if (bottom > area.bottom)
                bottom = area.bottom;

            int run_left = -1;
            for (int left = area.left; left < area.right; left += GUAC_DISPLAY_CELL_SIZE) {

                int right = left + GUAC_DISPLAY_CELL_SIZE;
                if (right > area.right)
                    right = area.right;

                guac_rect cell = { .left = left, .top = top, .right = right, .bottom = bottom };
                int differs = guac_display_video_rect_differs(display, &cell);

                if (differs && run_left < 0)
                    run_left = left;

                /* End of a run of changed cells */
                else if (!differs && run_left >= 0) {
                    guac_rect run = { .left = run_left, .top = top, .right = left, .bottom = bottom };
                    overflow |= guac_display_video_add_region(video, &run);
                    run_left = -1;
                }

            }

            /* Run reaching the right edge of the area */
            if (run_left >= 0) {
                guac_rect run = { .left = run_left, .top = top, .right = area.right, .bottom = bottom };
                overflow |= guac_display_video_add_region(video, &run);
            }

        }

        /* Too many separate regions: send their bounding box instead */
        if (overflow) {
            guac_rect box = video->regions[0];
            for (int i = 1; i < video->region_count; i++)
                guac_rect_extend(&box, &video->regions[i]);
            guac_rect_extend(&box, dirty);
            guac_rect_constrain(&box, bounds);
            video->regions[0] = box;
            video->region_count = 1;
        }

    }

    /* Remember the encoded contents of what changed */
    for (int i = 0; i < video->region_count; i++) {
        const guac_rect* rect = &video->regions[i];
        size_t row_length = (size_t) guac_rect_width(rect) * 4;
        for (int y = rect->top; y < rect->bottom; y++)
            memcpy(video->previous + (size_t) y * previous_stride + (size_t) rect->left * 4,
                    layer->last_frame.buffer + (size_t) y * layer->last_frame.buffer_stride
                        + (size_t) rect->left * 4,
                    row_length);
    }

}

/**
 * Converts the default layer's last frame and passes it to the encoder,
 * sending whatever access units come out.
 *
 * @param crf
 *     The constant rate factor to encode this frame with.
 *
 * @return
 *     Zero on success, non-zero on failure.
 */
static int guac_display_video_encode(guac_display* display) {

    guac_display_video* video = display->video;
    guac_display_layer* layer = display->default_layer;
    guac_client* client = display->client;

    if (av_frame_make_writable(video->frame) < 0)
        return 1;

    const uint8_t* src[4] = { layer->last_frame.buffer, NULL, NULL, NULL };
    const int src_stride[4] = { (int) layer->last_frame.buffer_stride, 0, 0, 0 };
    sws_scale(video->sws, src, src_stride, 0, video->height,
            video->frame->data, video->frame->linesize);

    int64_t pts = guac_timestamp_current() - video->stream_start;
    if (pts <= video->last_pts)
        pts = video->last_pts + 1;
    video->last_pts = pts;

    video->frame->pts = pts;
    video->frame->pict_type = video->force_keyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    video->force_keyframe = 0;

    int err = avcodec_send_frame(video->context, video->frame);
    if (err < 0) {
        char message[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(err, message, sizeof(message));
        guac_client_log(client, GUAC_LOG_ERROR, "H.264 encoding failed: %s", message);
        return 1;
    }

    while (avcodec_receive_packet(video->context, video->packet) == 0) {
        guac_display_video_send_packet(display, video->packet);
        av_packet_unref(video->packet);
    }

    video->stats_frames++;
    return 0;

}

/**
 * Disables video for the given display after a failure, so that the default
 * layer goes back to image updates. Every cell of the layer is marked as
 * lossy, so the lossless refresh resends the whole layer as images.
 */
static void guac_display_video_fail(guac_display* display) {

    guac_client* client = display->client;
    guac_display_layer* layer = display->default_layer;

    guac_client_log(client, GUAC_LOG_WARNING, "Disabling H.264 video. "
            "Graphical updates will be sent as images.");

    guac_display_video_free(display);

    if (layer->pending_frame_cells != NULL) {
        size_t count = layer->pending_frame_cells_width * layer->pending_frame_cells_height;
        for (size_t i = 0; i < count; i++) {
            layer->pending_frame_cells[i].lossy = 1;
            layer->pending_frame_cells[i].last_frame = 0;
        }
    }

    display->lossless_refresh_due = guac_timestamp_current();

}

int PFW_LFW_guac_display_video_flush(guac_display* display) {

    guac_display_video* video = display->video;
    guac_display_layer* layer = display->default_layer;
    guac_client* client = display->client;
    guac_timestamp now = guac_timestamp_current();

    if (video == NULL)
        return 0;

    int width = layer->last_frame.width;
    int height = layer->last_frame.height;

    /* Nothing can be encoded until the layer has contents */
    if (width <= 0 || height <= 0 || layer->last_frame.buffer == NULL)
        return 0;

    int changed = !guac_rect_is_empty(&layer->last_frame.dirty);
    int full = 0;

    /* Restart the stream (new stream instruction + keyframe) once a joining
     * user has been promoted and receives broadcast instructions */
    int restart = 0;
    pthread_mutex_lock(&video->restart_lock);
    if (video->restart_due && video->restart_due <= now) {
        video->restart_due = 0;
        restart = 1;
    }
    pthread_mutex_unlock(&video->restart_lock);

    /* A change in size requires a new encoder and a new stream */
    if (width != video->width || height != video->height) {
        guac_display_video_close_encoder(video);
        if (guac_display_video_open_encoder(display, width, height)) {
            guac_display_video_end_stream(display);
            guac_display_video_fail(display);
            return 1;
        }
        restart = 1;
    }

    if (restart || video->stream == NULL) {
        guac_display_video_end_stream(display);
        guac_display_video_start_stream(display);
        if (video->stream == NULL) {
            guac_display_video_fail(display);
            return 1;
        }
        changed = 1;
        full = 1;
    }

    if (!changed)
        return 0;

    /* The parts of the layer the client should draw from this frame: the
     * whole layer on a (re)start, the cells that actually changed otherwise */
    guac_rect bounds = { .left = 0, .top = 0, .right = width, .bottom = height };
    guac_rect dirty = layer->last_frame.dirty;
    if (guac_rect_is_empty(&dirty))
        full = 1;

    guac_display_video_find_regions(display, &dirty, &bounds, full);

    /* Reported changes that left the contents identical need no frame */
    if (video->region_count == 0)
        return 0;

    if (guac_display_video_encode(display)) {
        guac_display_video_end_stream(display);
        guac_display_video_fail(display);
        return 1;
    }

    /* Whatever was just drawn from video is lossy on the client: resend it
     * losslessly (as images, through the regular lossless refresh) once it
     * stops changing */
    if (!layer->pending_frame.lossless && layer->pending_frame_cells != NULL) {

        for (int i = 0; i < video->region_count; i++) {

            const guac_rect* rect = &video->regions[i];
            int left = rect->left / GUAC_DISPLAY_CELL_SIZE;
            int top = rect->top / GUAC_DISPLAY_CELL_SIZE;
            int right = (rect->right - 1) / GUAC_DISPLAY_CELL_SIZE;
            int bottom = (rect->bottom - 1) / GUAC_DISPLAY_CELL_SIZE;

            if (right >= (int) layer->pending_frame_cells_width)
                right = layer->pending_frame_cells_width - 1;
            if (bottom >= (int) layer->pending_frame_cells_height)
                bottom = layer->pending_frame_cells_height - 1;

            for (int y = top; y <= bottom; y++) {
                guac_display_layer_cell* cell = layer->pending_frame_cells
                    + (size_t) y * layer->pending_frame_cells_width + left;
                for (int x = left; x <= right; x++, cell++) {
                    cell->lossy = 1;
                    cell->last_frame = now;
                }
            }

        }

    }

    /* Periodic statistics at debug level */
    if (!video->stats_start)
        video->stats_start = now;
    else if (now - video->stats_start >= 10000) {
        int elapsed = (int) (now - video->stats_start);
        guac_client_log(client, GUAC_LOG_DEBUG, "H.264: %i frames in %i ms "
                "(%.1f fps, %.2f Mbit/s).", video->stats_frames, elapsed,
                video->stats_frames * 1000.0 / elapsed,
                video->stats_bytes * 8.0 / 1000.0 / elapsed);
        video->stats_frames = 0;
        video->stats_bytes = 0;
        video->stats_start = now;
    }

    return 1;

}

guac_timestamp guac_display_video_due(guac_display* display) {

    guac_display_video* video = display->video;
    if (video == NULL)
        return 0;

    pthread_mutex_lock(&video->restart_lock);
    guac_timestamp due = video->restart_due;
    pthread_mutex_unlock(&video->restart_lock);

    return due;

}

void guac_display_video_request_restart(guac_display* display) {

    guac_display_video* video = display->video;
    if (video == NULL)
        return;

    pthread_mutex_lock(&video->restart_lock);
    video->restart_due = guac_timestamp_current() + GUAC_DISPLAY_VIDEO_RESTART_DELAY;
    pthread_mutex_unlock(&video->restart_lock);

}

void guac_display_video_free(guac_display* display) {

    guac_display_video* video = display->video;
    if (video == NULL)
        return;

    guac_display_video_end_stream(display);
    guac_display_video_close_encoder(video);
    pthread_mutex_destroy(&video->restart_lock);

    display->video = NULL;
    guac_mem_free(video);

}
