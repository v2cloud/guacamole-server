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

#include "channels/disp.h"
#include "plugins/channels.h"
#include "fs.h"
#include "rdp.h"
#include "settings.h"

#include <freerdp/client/disp.h>
#include <freerdp/freerdp.h>
#include <freerdp/event.h>
#include <guacamole/client.h>
#include <guacamole/display.h>
#include <guacamole/mem.h>
#include <guacamole/protocol.h>
#include <guacamole/protocol-constants.h>
#include <guacamole/rect.h>
#include <guacamole/timestamp.h>

#include <limits.h>
#include <stdlib.h>
#include <string.h>

guac_rdp_disp* guac_rdp_disp_alloc(guac_client* client) {

    guac_rdp_disp* disp = guac_mem_alloc(sizeof(guac_rdp_disp));
    if (disp == NULL)
        return NULL;

    disp->client = client;

    /* Not yet connected */
    disp->disp = NULL;

    /* No requests have been made */
    disp->last_request = guac_timestamp_current();
    disp->last_set_size = 0;
    disp->reconnect_needed = 0;
    disp->resize_needed = false;
    disp->reported_monitors_count = 0;

    /* Init first monitor */
    disp->monitors = guac_mem_alloc(sizeof(guac_rdp_disp_monitor));
    if (disp->monitors == NULL) {
        guac_mem_free(disp);
        return NULL;
    }
    disp->monitors[0].requested_width  = 0;
    disp->monitors[0].requested_height = 0;
    disp->monitors[0].x_position  = 0;
    disp->monitors[0].top_offset  = 0;
    disp->monitors[0].left_offset = 0;
    disp->monitors_count = 1;

    return disp;

}

void guac_rdp_disp_free(guac_rdp_disp* disp) {
    if (disp == NULL)
        return;
    guac_mem_free(disp->monitors);
    guac_mem_free(disp);
}

/**
 * Identify which monitor contains the given point, returning its index, or
 * -1 if the point falls outside every known monitor's region. Coordinates
 * are in buffer space — the (0, 0)-anchored coordinate space of the default
 * display layer, which is what display-plan.c hands us for SCRBLT
 * rectangles. Monitor positions on `disp` are in MS-RDPEDISP *wire*
 * space (primary at (0, 0), secondaries possibly negative), so the
 * caller must pass the normalization offsets — the min left_offset and
 * min top_offset across all monitors — so this function can translate
 * each monitor's wire position into the same buffer space.
 *
 * Caller must hold the monitors lock (message_lock).
 */
static int guac_rdp_disp_monitor_at(const guac_rdp_disp* disp,
        int x, int y, int min_left, int min_top) {

    for (int i = 0; i < disp->monitors_count; i++) {
        const guac_rdp_disp_monitor* m = &disp->monitors[i];
        int norm_left = m->left_offset - min_left;
        int norm_top  = m->top_offset  - min_top;
        if (x >= norm_left && y >= norm_top
                && x < norm_left + m->requested_width
                && y < norm_top  + m->requested_height) {
            return i;
        }
    }

    return -1;

}

bool guac_rdp_disp_should_decompose_copy(void* closure,
        int src_x, int src_y, int width, int height,
        int dst_x, int dst_y) {

    guac_rdp_client* rdp_client = (guac_rdp_client*) closure;
    if (rdp_client == NULL || rdp_client->disp == NULL)
        return false;

    /* Fast path: no multi-monitor → no cross-monitor concern. */
    if (rdp_client->disp->monitors_count <= 1)
        return false;

    /* Check whether the source and destination rects both lie entirely
     * within the same monitor. We probe the four corners of each rect
     * (-1 on width/height because the rect is half-open [x, x+w) ×
     * [y, y+h)); if any corner falls on a different monitor than the one
     * containing the source's first corner, the copy crosses a boundary.
     *
     * Probing the corners is intentionally conservative: a same-monitor
     * copy whose corners graze a monitor edge may be decomposed
     * needlessly, costing one extra image transmission. Missing a genuine
     * cross-monitor copy would leave a visible artifact, so erring toward
     * decomposition is the safer trade-off. */

    int w1 = width  > 0 ? width  - 1 : 0;
    int h1 = height > 0 ? height - 1 : 0;

    /* This handler is invoked by guac_display_plan_apply while libguac
     * holds the display->ops FIFO lock around the call. Acquiring a
     * blocking lock here closes a deadlock cycle with any thread that
     * holds message_lock briefly and may need a libguac lock (the FIFO
     * lock or pending_frame.lock). Use trylock so
     * the handler never blocks; if message_lock is contended, fall
     * back to decompose=true. Decomposition is a per-copy optimisation
     * — emitting the destination region as fresh image data is always
     * a correct alternative, costing one extra IMG transmission. */
    if (pthread_mutex_trylock(&(rdp_client->message_lock)) != 0)
        return true;

    /* Compute normalization offsets so wire-space monitor coords (where
     * primary is fixed at (0, 0) per MS-RDPEDISP and secondaries can be
     * negative) translate into the same (0, 0)-anchored buffer space the
     * probe coordinates live in. Without this, the monitor_at lookup is
     * wrong for left-of-primary and above-of-primary layouts: a probe at
     * buffer-x=500 would match primary (wire-left=0) instead of the
     * secondary (wire-left=-1920, normalized to buffer-left=0). The
     * cross-monitor check would silently miss and the SCRBLT would be
     * sent verbatim, leaving stale pixels on the destination monitor
     * when a window is dragged onto or from a left- or above-positioned
     * secondary. */
    int min_left = 0;
    int min_top  = 0;
    for (int i = 0; i < rdp_client->disp->monitors_count; i++) {
        const guac_rdp_disp_monitor* m = &rdp_client->disp->monitors[i];
        if (m->left_offset < min_left) min_left = m->left_offset;
        if (m->top_offset  < min_top)  min_top  = m->top_offset;
    }

    int first = guac_rdp_disp_monitor_at(rdp_client->disp,
            src_x, src_y, min_left, min_top);
    bool decompose = false;

    const int probes[8][2] = {
        { src_x,         src_y         },
        { src_x + w1,    src_y         },
        { src_x,         src_y + h1    },
        { src_x + w1,    src_y + h1    },
        { dst_x,         dst_y         },
        { dst_x + w1,    dst_y         },
        { dst_x,         dst_y + h1    },
        { dst_x + w1,    dst_y + h1    },
    };

    for (int i = 0; i < 8; i++) {
        int m = guac_rdp_disp_monitor_at(rdp_client->disp,
                probes[i][0], probes[i][1], min_left, min_top);
        if (m != first) {
            decompose = true;
            break;
        }
    }

    pthread_mutex_unlock(&(rdp_client->message_lock));

    return decompose;

}

/**
 * Callback which associates handlers specific to Guacamole with the
 * DispClientContext instance allocated by FreeRDP to deal with received
 * Display Update (client-initiated dynamic display resizing) messages.
 *
 * This function is called whenever a channel connects via the PubSub event
 * system within FreeRDP, but only has any effect if the connected channel is
 * the Display Update channel. This specific callback is registered with the
 * PubSub system of the relevant rdpContext when guac_rdp_disp_load_plugin() is
 * called.
 *
 * @param context
 *     The rdpContext associated with the active RDP session.
 *
 * @param args
 *     Event-specific arguments, mainly the name of the channel, and a
 *     reference to the associated plugin loaded for that channel by FreeRDP.
 */
static void guac_rdp_disp_channel_connected(rdpContext* context,
        ChannelConnectedEventArgs* args) {

    guac_client* client = ((rdp_freerdp_context*) context)->client;
    guac_rdp_client* rdp_client = (guac_rdp_client*) client->data;
    guac_rdp_disp* guac_disp = rdp_client->disp;

    /* Ignore connection event if it's not for the Display Update channel */
    if (strcmp(args->name, DISP_DVC_CHANNEL_NAME) != 0)
        return;

    /* Init module with current display size. The primary monitor is
     * always at (0, 0); request explicit left_offset = 0 (rather than
     * INT_MIN sentinel) so the channel-connected init does not depend
     * on the cumulative-sum fallback path. */
    guac_rdp_disp_set_size(guac_disp, rdp_client->settings,
            context->instance, guac_rdp_get_width(context->instance),
            guac_rdp_get_height(context->instance),
            /* x_position */ 0, /* top_offset */ 0, /* left_offset */ 0);

    /* Store reference to the display update plugin once it's connected */
    DispClientContext* disp = (DispClientContext*) args->pInterface;
    guac_disp->disp = disp;

    guac_client_log(client, GUAC_LOG_DEBUG, "Display update channel "
            "will be used for display size changes.");

}

/**
 * Callback which disassociates Guacamole from the DispClientContext instance
 * that was originally allocated by FreeRDP and is about to be deallocated.
 *
 * This function is called whenever a channel disconnects via the PubSub event
 * system within FreeRDP, but only has any effect if the disconnected channel
 * is the Display Update channel. This specific callback is registered with the
 * PubSub system of the relevant rdpContext when guac_rdp_disp_load_plugin() is
 * called.
 *
 * @param context
 *     The rdpContext associated with the active RDP session.
 *
 * @param args
 *     Event-specific arguments, mainly the name of the channel, and a
 *     reference to the associated plugin loaded for that channel by FreeRDP.
 */
static void guac_rdp_disp_channel_disconnected(rdpContext* context,
        ChannelDisconnectedEventArgs* args) {

    guac_client* client = ((rdp_freerdp_context*) context)->client;
    guac_rdp_client* rdp_client = (guac_rdp_client*) client->data;
    guac_rdp_disp* guac_disp = rdp_client->disp;

    /* Ignore disconnection event if it's not for the Display Update channel */
    if (strcmp(args->name, DISP_DVC_CHANNEL_NAME) != 0)
        return;

    /* Channel is no longer connected */
    guac_disp->disp = NULL;

    guac_client_log(client, GUAC_LOG_DEBUG, "Display update channel "
            "disconnected.");

}

void guac_rdp_disp_load_plugin(rdpContext* context) {

    /* Subscribe to and handle channel connected events */
    PubSub_SubscribeChannelConnected(context->pubSub,
        (pChannelConnectedEventHandler) guac_rdp_disp_channel_connected);

    /* Subscribe to and handle channel disconnected events */
    PubSub_SubscribeChannelDisconnected(context->pubSub,
            (pChannelDisconnectedEventHandler) guac_rdp_disp_channel_disconnected);

    /* Add "disp" channel */
    guac_freerdp_dynamic_channel_collection_add(context->settings, "disp", NULL);

}

/**
 * Reallocates the monitors array to the given size.
 *
 * On allocation failure, the existing array and `monitors_count` are left
 * unchanged so the caller can continue operating with the previous layout.
 *
 * @param disp
 *     The display update module to reallocate.
 *
 * @param requested_monitors
 *     The number of monitors to allocate. Must be at least 1.
 *
 * @return
 *     Non-zero if the reallocation succeeded (or no-op was needed),
 *     zero on allocation failure.
 */
static int guac_rdp_disp_realloc_monitors(guac_rdp_disp* disp,
        int requested_monitors) {

    /* No need to reallocate if the number of monitors is unchanged */
    if (disp->monitors_count == requested_monitors)
        return 1;

    /* At least one monitor must remain */
    if (requested_monitors < 1)
        return 0;

    /* Use a temporary so the original pointer survives a realloc failure
     * (standard realloc semantics: NULL return means original is unchanged). */
    guac_rdp_disp_monitor* resized = guac_mem_realloc(disp->monitors,
            requested_monitors * sizeof(guac_rdp_disp_monitor));

    if (resized == NULL)
        return 0;

    disp->monitors = resized;
    disp->monitors_count = requested_monitors;
    return 1;

}

/**
 * Returns the x-offset of the monitor at the given position from the left edge
 * of the screen.
 *
 * @param disp
 *     The display update module to query.
 *
 * @param x_position
 *     The position of the monitor to query.
 *
 * @return
 *     The offset of the monitor at the given position from the left edge of
 *     the screen, in pixels.
 */
static int guac_rdp_disp_get_left_offset(const guac_rdp_disp* disp, int x_position) {

    int x_offset = 0;

    /* Calculate the offset of the monitor from the left edge of the screen */
    for (int i = 0; i < x_position; i++)
        x_offset += disp->monitors[i].requested_width;

    return x_offset;

}

/**
 * Returns the "total" height of all monitors. This is not the sum of the
 * heights of all monitors, but rather the height of the entire screen.
 * It is the subtraction of the lowest point by the highest point.
 *
 * @param disp
 *     The display update module to query.
 *
 * @return
 *     The total height of the display, in pixels.
 */
static int guac_rdp_disp_get_total_height(const guac_rdp_disp* disp) {

    int min_offset = 0;
    int max_bottom = 0;

    /* Loop through all monitors to find the maximum height */
    for (int i = 0; i < disp->monitors_count; i++) {

        int requested_height = disp->monitors[i].requested_height;
        int top_offset = disp->monitors[i].top_offset;

        /* Find the highest point of the screen (the lowest top offset) */
        if (top_offset < min_offset)
            min_offset = top_offset;

        /* Find the lowest point of the screen */
        if (top_offset + requested_height > max_bottom)
            max_bottom = top_offset + requested_height;

    }

    return max_bottom - min_offset;

}

/**
 * Returns the "total" width of all monitors — the bounding-box width of
 * the combined desktop, not the sum of monitor widths. Symmetric with
 * guac_rdp_disp_get_total_height: subtracts the lowest left edge from
 * the highest right edge so non-linear layouts (e.g. secondary to the
 * left of primary with a negative left_offset) produce the correct
 * combined dimension.
 *
 * @param disp
 *     The display update module to query.
 *
 * @return
 *     The total width of the combined display, in pixels.
 */
static int guac_rdp_disp_get_total_width(const guac_rdp_disp* disp) {

    int min_left = 0;
    int max_right = 0;

    for (int i = 0; i < disp->monitors_count; i++) {

        int requested_width = disp->monitors[i].requested_width;
        int left_offset = disp->monitors[i].left_offset;

        if (left_offset < min_left)
            min_left = left_offset;

        if (left_offset + requested_width > max_right)
            max_right = left_offset + requested_width;

    }

    return max_right - min_left;

}

/**
 * Closes the secondary monitor at the given position, shifting any monitors
 * positioned after it down to fill the gap. The primary monitor (position 0)
 * cannot be closed and out-of-range positions are rejected.
 *
 * @param disp
 *     The display update module to close the monitor of.
 *
 * @param x_position
 *     The position of the monitor to close.
 *
 * @return
 *     true if the monitor was closed, false if the request was rejected
 *     (primary, out-of-range, or reallocation failure).
 */
static bool guac_rdp_disp_close_monitor(guac_rdp_disp* disp, int x_position) {

    int max_position = disp->monitors_count - 1;

    /* Primary monitor or invalid position */
    if (x_position <= 0 || x_position > max_position)
        return false;

    /* The monitor to close is not the last one, so shift later monitors
     * down to preserve them */
    if (x_position != max_position) {
        int move_count = max_position - x_position;
        memmove(&disp->monitors[x_position],
                &disp->monitors[x_position + 1],
                move_count * sizeof(guac_rdp_disp_monitor));
    }

    /* Deallocate a monitor; refuse close if realloc fails (caller will
     * see false return and original layout is preserved). */
    if (!guac_rdp_disp_realloc_monitors(disp, max_position))
        return false;
    disp->resize_needed = true;
    disp->last_set_size = guac_timestamp_current();

    return true;

}

void guac_rdp_disp_set_size(guac_rdp_disp* disp, guac_rdp_settings* settings,
        freerdp* rdp_inst, int width, int height,
        int x_position, int top_offset, int left_offset) {
    
    /* Debug log: help disambiguate cases where a client size instruction never
     * reaches the server vs. cases where it reaches the server but Windows 
     * ignores it.
     */
    guac_client_log(disp->client, GUAC_LOG_DEBUG,
        "disp_set_size: x_position=%d width=%d height=%d top_offset=%d "
        "left_offset=%d",
        x_position, width, height, top_offset, left_offset);

    /* x_position is a client-supplied wire value (atoi, unclamped), so bound
     * it before computing x_position + 1: this keeps the addition from
     * overflowing (INT_MAX + 1) and ensures the value can never index
     * disp->monitors out of bounds. Reject:
     *   - invalid monitor index        : x_position < 0
     *   - too many monitors requested   : x_position > max_secondary_monitors
     *   - missing intermediate monitor  : x_position > monitors_count
     * max_secondary_monitors is already clamped to
     * [0, GUAC_RDP_MAX_SECONDARY_MONITORS], so x_position + 1 cannot overflow
     * once these checks pass. */
    if (x_position < 0
            || x_position > settings->max_secondary_monitors
            || x_position > disp->monitors_count)
        return;

    int min_monitors_requested = x_position + 1;

    guac_rect resize = {
        .left = 0,
        .top = 0,
        .right = width,
        .bottom = height
    };

    /* Fit width and height within bounds, maintaining aspect ratio */
    guac_rect_shrink(&resize, GUAC_RDP_DISP_MAX_SIZE, GUAC_RDP_DISP_MAX_SIZE);

    width = guac_rect_width(&resize);
    height = guac_rect_height(&resize);

    /* Serialize monitor-array mutations with the broadcast read path in
     * gdi.c (guac_rdp_gdi_desktop_resize) and the SendMonitorLayout call
     * in guac_rdp_disp_update_size — both run on a different thread. */
    guac_rdp_client* rdp_client = (guac_rdp_client*) disp->client->data;
    pthread_mutex_lock(&(rdp_client->message_lock));

    if (width > 0 && height > 0) {

        /* Width must be even (RDP DISPLAY_CONTROL requirement). Round down
         * to the next even multiple first, then clamp to the minimum so
         * the result never falls below GUAC_RDP_DISP_MIN_SIZE. */
        width &= ~1;

        /* A rectangle may exceed the maximum allowed dimensions yet fall
         * below the minimum after the shrink-to-max adjustment (consider a
         * rectangle like 16384x256). We don't bother preserving aspect
         * ratio in that unlikely case. */
        if (width  < GUAC_RDP_DISP_MIN_SIZE) width  = GUAC_RDP_DISP_MIN_SIZE;
        if (height < GUAC_RDP_DISP_MIN_SIZE) height = GUAC_RDP_DISP_MIN_SIZE;

        /* Reallocate monitors if needed. On failure, drop the request
         * rather than corrupting the existing layout. */
        if (disp->monitors_count < min_monitors_requested
                && !guac_rdp_disp_realloc_monitors(disp, min_monitors_requested)) {
            pthread_mutex_unlock(&(rdp_client->message_lock));
            return;
        }

        guac_rdp_disp_monitor* monitor = &disp->monitors[x_position];

        /* Resolve the monitor's left offset. If the client supplied an
         * explicit value (anything other than the INT_MIN sentinel), use
         * it directly — this permits non-linear layouts (secondary to the
         * left of primary, gaps between monitors, etc.) by letting the
         * client report absolute positions normalized so the leftmost
         * monitor sits at left_offset = 0. Otherwise fall back to the
         * legacy horizontal-row layout (cumulative sum of widths by
         * x_position) for backwards compatibility with older clients
         * that send only 4 wire arguments. */
        int new_left_offset = (left_offset != INT_MIN) ? left_offset
                : guac_rdp_disp_get_left_offset(disp, x_position);

        /* Skip the broadcast if nothing about this monitor's geometry
         * changed. (monitor->x_position is always equal to x_position
         * because we looked the monitor up by index, so we don't include
         * it in the comparison.) */
        if (monitor->requested_width == width
                && monitor->requested_height == height
                && monitor->top_offset == top_offset
                && monitor->left_offset == new_left_offset) {
            pthread_mutex_unlock(&(rdp_client->message_lock));
            return;
        }

        disp->resize_needed       = true;
        disp->last_set_size       = guac_timestamp_current();
        monitor->requested_width  = width;
        monitor->requested_height = height;
        monitor->x_position       = x_position;
        monitor->top_offset       = top_offset;
        monitor->left_offset      = new_left_offset;
    }

    /* width == 0 or height == 0 is treated as a request to close this
     * monitor. Refuse if the monitor can't be closed (primary or invalid). */
    else if (!guac_rdp_disp_close_monitor(disp, x_position)) {
        pthread_mutex_unlock(&(rdp_client->message_lock));
        return;
    }

    pthread_mutex_unlock(&(rdp_client->message_lock));

    /* Send display update notification if possible */
    guac_rdp_disp_update_size(disp, settings, rdp_inst);

}

/* Maximum size of the multimon-layout JSON broadcast. Sized so the JSON
 * for the maximum supported monitor count stays well within a 2KB chunk. */
#define GUAC_RDP_DISP_LAYOUT_JSON_SIZE 2048

void guac_rdp_disp_broadcast_monitor_layout(guac_rdp_disp* disp) {

    guac_client* client = disp->client;
    guac_rdp_client* rdp_client = (guac_rdp_client*) client->data;
    guac_display_layer* default_layer =
            guac_display_default_layer(rdp_client->display);

    char json[GUAC_RDP_DISP_LAYOUT_JSON_SIZE];
    int pos = 0;
    int emitted = 0;
    int truncated = 0;
    int written;

    /* Build the JSON under message_lock so disp->monitors can't be mutated
     * (set_size or realloc on the input thread) mid-iteration. */
    pthread_mutex_lock(&(rdp_client->message_lock));

    written = snprintf(json, GUAC_RDP_DISP_LAYOUT_JSON_SIZE, "{");
    if (written > 0 && written < GUAC_RDP_DISP_LAYOUT_JSON_SIZE)
        pos = written;

    int monitors_count = disp->monitors_count;
    for (int i = 0; i < monitors_count && !truncated; i++) {

        /* Skip monitors that have not been initialized yet */
        if (disp->monitors[i].requested_width == 0 ||
            disp->monitors[i].requested_height == 0)
            continue;

        const char* sep = (emitted == 0) ? "" : ",";
        int remaining = GUAC_RDP_DISP_LAYOUT_JSON_SIZE - pos;
        written = snprintf(json + pos, remaining,
            "%s\"%d\":{\"left\":%d,\"top\":%d,\"width\":%d,\"height\":%d}",
            sep, i,
            disp->monitors[i].left_offset,
            disp->monitors[i].top_offset,
            disp->monitors[i].requested_width,
            disp->monitors[i].requested_height);

        if (written < 0 || written >= remaining) {
            truncated = 1;
            break;
        }

        pos += written;
        emitted++;
    }

    int remaining = GUAC_RDP_DISP_LAYOUT_JSON_SIZE - pos;
    written = snprintf(json + pos, remaining, "}");
    if (written < 0 || written >= remaining) {
        json[GUAC_RDP_DISP_LAYOUT_JSON_SIZE - 2] = '}';
        json[GUAC_RDP_DISP_LAYOUT_JSON_SIZE - 1] = '\0';
        truncated = 1;
    }

    pthread_mutex_unlock(&(rdp_client->message_lock));

    if (truncated)
        guac_client_log(client, GUAC_LOG_WARNING,
                "multimon-layout JSON was truncated; client may see a partial "
                "layout. Consider increasing GUAC_RDP_DISP_LAYOUT_JSON_SIZE or "
                "reducing the configured maximum monitor count.");

    /* Inform connected clients of the committed monitor layout so secondary
     * windows can position/size their canvases. Sent for every committed
     * layout — not only on desktop-dimension changes — because moving a
     * monitor without changing the bounding-box size produces no
     * DesktopResize callback, and a stale layout leaves secondaries drawing
     * at the wrong offset (e.g. a left-placed monitor painting over the
     * primary). */
    guac_protocol_send_set(client->socket, (const guac_layer*) default_layer,
            GUAC_PROTOCOL_LAYER_PARAMETER_MULTIMON_LAYOUT, json);

}

void guac_rdp_disp_update_size(guac_rdp_disp* disp,
        guac_rdp_settings* settings, freerdp* rdp_inst) {

    guac_timestamp now = guac_timestamp_current();

    /* Limit display update frequency */
    if (now - disp->last_request <= GUAC_RDP_DISP_UPDATE_INTERVAL)
        return;

    /* For a multi-monitor layout, wait for the incoming request burst to
     * settle before sending. A multi-monitor layout change arrives as a rapid
     * burst of per-monitor size requests; sending after the first would emit a
     * half-applied (gap/overlap) layout followed by the rest as two separate
     * PDUs. Holding off until GUAC_RDP_DISP_SETTLE_INTERVAL of quiet collapses
     * the burst into a single atomic PDU. Single-monitor resizes are exempt.
     * last_set_size/monitors_count are simple reads; a benign race here only
     * defers to the next poll — the layout is re-read under the lock below. */
    if (disp->monitors_count > 1
            && now - disp->last_set_size < GUAC_RDP_DISP_SETTLE_INTERVAL)
        return;

    guac_rdp_client* rdp_client = (guac_rdp_client*) disp->client->data;

    /* Acquire message_lock to safely read monitors_count and copy the
     * monitors array. This serializes with set_size mutations on the input
     * thread and with the JSON broadcast read in gdi.c. The lock is released
     * before calling SendMonitorLayout to avoid a deadlock with the FreeRDP
     * processing thread (see else-if branch below). */
    pthread_mutex_lock(&(rdp_client->message_lock));

    /* Re-check resize_needed under the lock — set_size may have cleared
     * it concurrently (e.g. duplicate request collapsed). */
    if (rdp_inst != NULL && !disp->resize_needed) {
        pthread_mutex_unlock(&(rdp_client->message_lock));
        return;
    }

    int monitors_count = disp->monitors_count;
    int width = guac_rdp_disp_get_total_width(disp);
    int height = guac_rdp_disp_get_total_height(disp);

    if (settings->resize_method == GUAC_RESIZE_RECONNECT) {

        /* Update settings with new dimensions */
        settings->width = width;
        settings->height = height;

        /* Signal reconnect */
        disp->reconnect_needed = 1;

        /* Mark as sent only now that we've actually committed the request */
        disp->last_request = now;
        disp->resize_needed = false;

        pthread_mutex_unlock(&(rdp_client->message_lock));

    }

    /* Send display update notification if display channel is connected */
    else if (settings->resize_method == GUAC_RESIZE_DISPLAY_UPDATE
                && disp->disp != NULL) {

        /* Init monitors layout */
        DISPLAY_CONTROL_MONITOR_LAYOUT* monitors = guac_mem_alloc(
                monitors_count * sizeof(DISPLAY_CONTROL_MONITOR_LAYOUT));

        /* Allocation failed — skip this update rather than dereferencing
         * NULL. A subsequent layout change will retry. */
        if (monitors == NULL) {
            pthread_mutex_unlock(&(rdp_client->message_lock));
            return;
        }

        for (int i = 0; i < monitors_count; i++) {

            /* First monitor is the primary */
            int primary_monitor = (i == 0 ? 1 : 0);

            /* Set current monitor properties */
            monitors[i].Flags = primary_monitor;
            monitors[i].Left = disp->monitors[i].left_offset;
            monitors[i].Top = disp->monitors[i].top_offset;
            monitors[i].Width = disp->monitors[i].requested_width;
            monitors[i].Height = disp->monitors[i].requested_height;
            monitors[i].Orientation = 0;
            monitors[i].PhysicalWidth = 0;
            monitors[i].PhysicalHeight = 0;
        }

        /* Mark as sent before releasing the lock and calling SendMonitorLayout.
         * The monitors data is already copied to the local array above, so
         * disp->monitors is no longer accessed after this point — releasing
         * the lock here prevents a deadlock where SendMonitorLayout blocks on
         * the FreeRDP send buffer while gdi_desktop_resize waits for this
         * same lock on the FreeRDP processing thread. */
        disp->last_request = now;
        disp->resize_needed = false;

        pthread_mutex_unlock(&(rdp_client->message_lock));

        disp->disp->SendMonitorLayout(disp->disp, monitors_count, monitors);

        guac_mem_free(monitors);

        /* Broadcast the committed layout to connected clients. This MUST
         * happen here, not only in gdi_desktop_resize, because a layout
         * change that doesn't alter the desktop bounding-box size produces
         * no DesktopResize callback — yet the client still needs the new
         * monitor offsets, or secondary windows render at stale positions. */
        guac_rdp_disp_broadcast_monitor_layout(disp);

    }

    else {
        /* disp->disp is not yet connected or resize_method is none — nothing
         * was actually sent. Do NOT update last_request or resize_needed so
         * the next call to update_size will retry once the channel is ready. */
        pthread_mutex_unlock(&(rdp_client->message_lock));
    }

}

int guac_rdp_disp_reconnect_needed(guac_rdp_disp* disp) {
    guac_rdp_client* rdp_client = (guac_rdp_client*) disp->client->data;

    /* Do not reconnect if files are open. */
    if (rdp_client->filesystem != NULL
            && rdp_client->filesystem->open_files > 0)
        return 0;

    /* Do not reconnect if an active print job is present */
    if (rdp_client->active_job != NULL)
        return 0;
    
    
    return disp->reconnect_needed;
}

void guac_rdp_disp_reconnect_complete(guac_rdp_disp* disp) {
    disp->reconnect_needed = 0;
    disp->last_request = guac_timestamp_current();
}

