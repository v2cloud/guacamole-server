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

#ifndef GUAC_RDP_CHANNELS_RDPECAM_SINK_H
#define GUAC_RDP_CHANNELS_RDPECAM_SINK_H

#include <guacamole/client.h>
#include <guacamole/timestamp.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

/**
 * The maximum number of video frames buffered by the RDPECAM sink. The
 * queue depth bounds the latency added by buffering. When the queue is
 * full, the backlog is discarded and frames are dropped until the next
 * keyframe (see guac_rdpecam_push()).
 */
#define GUAC_RDPECAM_MAX_FRAMES 5

/**
 * The maximum size of a single video frame in bytes.
 */
#define GUAC_RDPECAM_MAX_FRAME_SIZE (1024 * 1024) // 1MB

/**
 * Minimum interval, in milliseconds, between keyframe requests signaled to
 * the browser (see guac_rdpecam_take_keyframe_request()).
 */
#define GUAC_RDPECAM_KEYFRAME_REQUEST_MIN_INTERVAL_MS 500

/**
 * RDPECAM frame header structure (little-endian).
 */
typedef struct guac_rdpecam_frame_header {

    /**
     * Version number (must be 1).
     */
    uint8_t version;

    /**
     * Flags (bit 0: keyframe).
     */
    uint8_t flags;

    /**
     * Reserved field (must be 0).
     */
    uint16_t reserved;

    /**
     * Presentation timestamp in milliseconds.
     */
    uint32_t pts_ms;

    /**
     * Length of the following H.264 payload in bytes.
     */
    uint32_t payload_len;

} __attribute__((packed)) guac_rdpecam_frame_header;

/**
 * A single video frame in the RDPECAM queue.
 */
typedef struct guac_rdpecam_frame {

    /**
     * Presentation timestamp in milliseconds.
     */
    uint32_t pts_ms;

    /**
     * Whether this is a keyframe.
     */
    bool keyframe;

    /**
     * The frame data (H.264 Annex-B format).
     */
    uint8_t* data;

    /**
     * The length of the frame data in bytes.
     */
    size_t length;

    /**
     * Pointer to the next frame in the queue.
     */
    struct guac_rdpecam_frame* next;

} guac_rdpecam_frame;

/**
 * RDPECAM sink for buffering and managing video frames from the client.
 */
typedef struct guac_rdpecam_sink {

    /**
     * Lock for thread-safe access to the sink.
     */
    pthread_mutex_t lock;

    /**
     * Condition variable for signaling frame availability.
     */
    pthread_cond_t frame_available;

    /**
     * The guac_client instance handling the relevant RDP connection.
     */
    guac_client* client;

    /**
     * Head of the frame queue.
     */
    guac_rdpecam_frame* queue_head;

    /**
     * Tail of the frame queue.
     */
    guac_rdpecam_frame* queue_tail;

    /**
     * Current number of frames in the queue.
     */
    int queue_size;


    /**
     * Whether the sink is being destroyed.
     */
    bool stopping;

    /**
     * Whether streaming has been started (shared across all device channels).
     */
    bool streaming;

    /**
     * Number of available credits for sending frames (shared across all channels).
     * Protected by the sink lock.
     */
    uint32_t credits;

    /**
     * Stream index for the active stream (shared across all channels).
     * This is the stream index from StartStreamsRequest (typically 0).
     * Protected by the sink lock.
     */
    uint8_t stream_index;

    /**
     * Whether a device channel has claimed the sender role. Only one
     * channel should actively dequeue and transmit frames at a time.
     */
    bool has_active_sender;

    /**
     * Opaque pointer to the channel currently authorized to transmit samples.
     * If NULL, no sender is active. Protected by the sink lock.
     */
    void* active_sender_channel;

    /**
     * Whether non-keyframes are currently being dropped following a queue
     * overflow. Queueing resumes at the next keyframe, as frames which
     * depend on discarded frames cannot be decoded. Protected by the sink
     * lock.
     */
    bool awaiting_keyframe;

    /**
     * Whether an immediate keyframe should be requested from the browser
     * (consumed via guac_rdpecam_take_keyframe_request()). Protected by the
     * sink lock.
     */
    bool keyframe_needed;

    /**
     * The time the last keyframe request was signaled, for rate limiting.
     * Protected by the sink lock.
     */
    guac_timestamp last_keyframe_request;

} guac_rdpecam_sink;

/**
 * Creates a new RDPECAM sink for the given client.
 *
 * @param client
 *     The guac_client instance handling the relevant RDP connection.
 *
 * @return
 *     A newly-allocated RDPECAM sink, or NULL if the given client is NULL or
 *     allocation fails.
 */
guac_rdpecam_sink* guac_rdpecam_create(guac_client* client);

/**
 * Destroys the given RDPECAM sink, freeing all associated resources.
 *
 * @param sink
 *     The RDPECAM sink to destroy.
 */
void guac_rdpecam_destroy(guac_rdpecam_sink* sink);

/**
 * Signals any threads waiting on the sink that shutdown is in progress, waking
 * them so they can terminate gracefully. The sink itself is not freed.
 *
 * @param sink
 *     The RDPECAM sink to signal.
 */
void guac_rdpecam_signal_stop(guac_rdpecam_sink* sink);

/**
 * Queues a fully-assembled RDPECAM frame within the sink. The frame data is
 * copied into an internal buffer. The call fails if the sink is stopping or
 * if validation of the header/payload fails. If the queue is full, the
 * backlog is discarded, and frames are dropped until the next keyframe,
 * with a keyframe request flagged for the browser (see
 * guac_rdpecam_take_keyframe_request()).
 *
 * @param sink
 *     The sink receiving the frame.
 *
 * @param data
 *     Pointer to the frame header followed by the H.264 payload.
 *
 * @param len
 *     Total number of bytes available at @p data.
 *
 * @return
 *     Non-zero if the frame was queued successfully, zero otherwise.
 */
bool guac_rdpecam_push(guac_rdpecam_sink* sink, const void* data, size_t len);

/**
 * Retrieves the next queued frame from the sink. Ownership of the returned
 * payload buffer is transferred to the caller.
 *
 * @param sink
 *     The sink to dequeue from.
 *
 * @param out_buf
 *     Receives a pointer to the payload buffer (must be freed by the caller).
 *
 * @param out_len
 *     Receives the payload length in bytes.
 *
 * @param out_keyframe
 *     Receives whether the frame represents a keyframe.
 *
 * @param out_pts_ms
 *     Receives the presentation timestamp, in milliseconds.
 *
 * @return
 *     Non-zero if a frame was returned, zero if the sink is stopping or empty.
 */
bool guac_rdpecam_pop(guac_rdpecam_sink* sink, uint8_t** out_buf, size_t* out_len,
                      bool* out_keyframe, uint32_t* out_pts_ms);

int guac_rdpecam_get_queue_size(guac_rdpecam_sink* sink);

/**
 * Atomically consumes any pending request for an immediate keyframe from the
 * browser-side encoder, rate-limited to one request per
 * GUAC_RDPECAM_KEYFRAME_REQUEST_MIN_INTERVAL_MS.
 *
 * @param sink
 *     The sink to check.
 *
 * @return
 *     Non-zero if a keyframe request should be signaled to the browser now,
 *     zero otherwise.
 */
bool guac_rdpecam_take_keyframe_request(guac_rdpecam_sink* sink);
 

#endif
