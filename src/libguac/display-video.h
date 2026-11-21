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

#ifndef GUAC_DISPLAY_VIDEO_H
#define GUAC_DISPLAY_VIDEO_H

/**
 * Streaming of the default layer of a guac_display as H.264 video, sent to
 * the client through the "video" instruction instead of as image updates.
 *
 * Each encoded access unit is sent within the video stream (split across as
 * many blobs as needed) prefixed by a header:
 *
 *     bytes 0-3    length of the access unit that follows (uint32, big endian)
 *     byte  4      flags (bit 0 set if the access unit is a keyframe)
 *     bytes 5-8    timestamp in milliseconds (uint32, big endian)
 *     bytes 9-10   number of changed regions N (uint16, big endian)
 *     then N * 8   each region: x, y, width, height (uint16 each, big endian)
 *
 * The access unit itself is H.264 in Annex B format, with SPS/PPS repeated
 * before every keyframe. The regions are the 64x64 cells that actually
 * changed (merged into rectangles), and the client draws only those parts of
 * each decoded frame. Everything drawn from video is marked lossy, so the regular
 * lossless refresh resends it as images once it stops changing: motion is
 * video, and static content (text, icons) ends up pixel-exact.
 *
 * @file display-video.h
 */

#include "guacamole/display.h"
#include "guacamole/timestamp.h"

/**
 * The mimetype of the video stream, as advertised by the client during the
 * handshake and sent with the "video" instruction.
 */
#define GUAC_DISPLAY_VIDEO_MIMETYPE "video/h264"

/**
 * The size of the fixed part of the header preceding each access unit, in
 * bytes (8 more bytes follow per changed region).
 */
#define GUAC_DISPLAY_VIDEO_HEADER_SIZE 11

/**
 * The amount of time to wait after a new user joins before restarting the
 * video stream with a keyframe, in milliseconds. Joining users are pending
 * (and do not receive broadcast instructions) for up to 250 ms.
 */
#define GUAC_DISPLAY_VIDEO_RESTART_DELAY 600

/**
 * Opaque state of the video encoder of a guac_display.
 */
typedef struct guac_display_video guac_display_video;

/**
 * Encodes and sends the current contents of the default layer as video, if
 * that layer has changed since the last frame or the stream must be
 * restarted. Must be called while holding the write locks of both the pending
 * and last frames, after the pending frame has been committed to the last
 * frame and BEFORE the lossless refresh is added (the refresh extends the
 * layer's dirty region, which must not be mistaken for a change).
 *
 * @param display
 *     The guac_display whose default layer should be encoded.
 *
 * @return
 *     Non-zero if anything was sent (such that the frame must be marked with
 *     a "sync"), zero otherwise.
 */
int PFW_LFW_guac_display_video_flush(guac_display* display);

/**
 * Returns when the video encoder next needs the display to end a frame even
 * if nothing changes (for a stream restart), or zero if it doesn't.
 *
 * @param display
 *     The guac_display to check.
 *
 * @return
 *     The timestamp at which a frame should next be ended, or zero.
 */
guac_timestamp guac_display_video_due(guac_display* display);

/**
 * Requests that the video stream be restarted with a keyframe shortly, for
 * the benefit of a user that is joining the connection. The caller should
 * then wake the render thread so that the restart is scheduled.
 *
 * @param display
 *     The guac_display whose video stream should be restarted.
 */
void guac_display_video_request_restart(guac_display* display);

/**
 * Frees the video encoder of the given display, if any, ending its stream.
 *
 * @param display
 *     The guac_display whose video encoder should be freed.
 */
void guac_display_video_free(guac_display* display);

#endif
