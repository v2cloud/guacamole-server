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
#include "rdp.h"

#include <CUnit/CUnit.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/*
 * Tests for guac_rdp_disp_should_decompose_copy.
 *
 * The function decides whether a default-layer `copy` operation must be
 * decomposed into a fresh image transmission because it crosses between
 * monitors. These tests drive the function directly with hand-built
 * monitor layouts, assert the verdict, and exercise the fast paths
 * (NULL closure, NULL disp, single-monitor) along with the cross-monitor
 * cases that motivated the work in the first place.
 *
 * Fixture strategy: allocate a real guac_rdp_client (the function casts
 * its closure to that pointer type), zero-init it, and only populate the
 * two fields it actually reads — `disp` and `message_lock`.
 */

/**
 * Allocates a freshly zeroed guac_rdp_client with its message_lock
 * initialized. Caller must free with destroy_rdp_client().
 */
static guac_rdp_client* create_rdp_client(guac_rdp_disp* disp) {
    guac_rdp_client* rdp_client = calloc(1, sizeof(guac_rdp_client));
    CU_ASSERT_PTR_NOT_NULL_FATAL(rdp_client);
    pthread_mutex_init(&rdp_client->message_lock, NULL);
    rdp_client->disp = disp;
    return rdp_client;
}

static void destroy_rdp_client(guac_rdp_client* rdp_client) {
    pthread_mutex_destroy(&rdp_client->message_lock);
    free(rdp_client);
}

/**
 * Test that a NULL closure returns false (fast path, defensive).
 */
void test_disp__should_decompose_copy_null_closure(void) {
    CU_ASSERT_FALSE(guac_rdp_disp_should_decompose_copy(NULL,
            0, 0, 100, 100, 200, 200));
}

/**
 * Test that a closure whose `disp` is NULL returns false. This can
 * happen briefly during teardown.
 */
void test_disp__should_decompose_copy_null_disp(void) {
    guac_rdp_client* rdp_client = create_rdp_client(NULL);
    CU_ASSERT_FALSE(guac_rdp_disp_should_decompose_copy(rdp_client,
            0, 0, 100, 100, 200, 200));
    destroy_rdp_client(rdp_client);
}

/**
 * Test the single-monitor fast path: with only the primary monitor,
 * copy operations are always safe so the function must return false.
 */
void test_disp__should_decompose_copy_single_monitor(void) {

    guac_rdp_disp_monitor monitors[1] = {
        { .requested_width = 1920, .requested_height = 1080,
          .x_position = 0, .top_offset = 0, .left_offset = 0 },
    };
    guac_rdp_disp disp = { .monitors = monitors, .monitors_count = 1 };
    guac_rdp_client* rdp_client = create_rdp_client(&disp);

    CU_ASSERT_FALSE(guac_rdp_disp_should_decompose_copy(rdp_client,
            0, 0, 500, 500, 1000, 500));

    destroy_rdp_client(rdp_client);
}

/**
 * Test a two-monitor side-by-side layout where the copy stays entirely
 * within the primary monitor. Must return false — copy is safe.
 */
void test_disp__should_decompose_copy_same_monitor(void) {

    guac_rdp_disp_monitor monitors[2] = {
        { .requested_width = 1920, .requested_height = 1080,
          .x_position = 0, .top_offset = 0, .left_offset = 0 },
        { .requested_width = 1920, .requested_height = 1080,
          .x_position = 1, .top_offset = 0, .left_offset = 1920 },
    };
    guac_rdp_disp disp = { .monitors = monitors, .monitors_count = 2 };
    guac_rdp_client* rdp_client = create_rdp_client(&disp);

    /* Both src (100,100)+200x200 and dst (500,500)+200x200 are within
     * monitor 0's region [0,1920)×[0,1080). */
    CU_ASSERT_FALSE(guac_rdp_disp_should_decompose_copy(rdp_client,
            100, 100, 200, 200, 500, 500));

    destroy_rdp_client(rdp_client);
}

/**
 * Test the core cross-monitor case: a copy from monitor 0 to monitor 1. Must
 * return true so the plan rewrites it as IMG.
 */
void test_disp__should_decompose_copy_cross_monitor(void) {

    guac_rdp_disp_monitor monitors[2] = {
        { .requested_width = 1920, .requested_height = 1080,
          .x_position = 0, .top_offset = 0, .left_offset = 0 },
        { .requested_width = 1920, .requested_height = 1080,
          .x_position = 1, .top_offset = 0, .left_offset = 1920 },
    };
    guac_rdp_disp disp = { .monitors = monitors, .monitors_count = 2 };
    guac_rdp_client* rdp_client = create_rdp_client(&disp);

    /* src (100,100)+200x200 is in monitor 0; dst (2000,100)+200x200
     * is in monitor 1 (left_offset=1920). */
    CU_ASSERT_TRUE(guac_rdp_disp_should_decompose_copy(rdp_client,
            100, 100, 200, 200, 2000, 100));

    destroy_rdp_client(rdp_client);
}

/**
 * Test a copy whose source rectangle straddles the seam between two
 * monitors. The 8-probe verdict sees the src corners falling in
 * different monitors and must return true even though the destination
 * lies entirely within monitor 1.
 */
void test_disp__should_decompose_copy_src_straddles_seam(void) {

    guac_rdp_disp_monitor monitors[2] = {
        { .requested_width = 1920, .requested_height = 1080,
          .x_position = 0, .top_offset = 0, .left_offset = 0 },
        { .requested_width = 1920, .requested_height = 1080,
          .x_position = 1, .top_offset = 0, .left_offset = 1920 },
    };
    guac_rdp_disp disp = { .monitors = monitors, .monitors_count = 2 };
    guac_rdp_client* rdp_client = create_rdp_client(&disp);

    /* src spans x=[1800, 2100), straddling the seam at x=1920.
     * dst (2200,100)+300x200 sits entirely in monitor 1. */
    CU_ASSERT_TRUE(guac_rdp_disp_should_decompose_copy(rdp_client,
            1800, 100, 300, 200, 2200, 100));

    destroy_rdp_client(rdp_client);
}

/**
 * Test a non-linear layout: secondary monitor positioned above
 * primary (negative top_offset in MS-RDPEDISP wire space). A copy
 * from the secondary into the primary must be decomposed.
 *
 * Probe coords are in buffer space — the (0, 0)-anchored coordinate
 * space the libguac display layer hands to should_decompose_copy.
 * The wire layout (primary at top_offset=0, secondary at
 * top_offset=-1920) normalizes to buffer space by subtracting
 * min_top=-1920: secondary becomes top=0, primary becomes top=1920.
 */
void test_disp__should_decompose_copy_vertical_layout(void) {

    guac_rdp_disp_monitor monitors[2] = {
        { .requested_width = 1920, .requested_height = 1080,
          .x_position = 0, .top_offset = 0, .left_offset = 0 },
        /* Portrait monitor above primary, narrower so it sits within
         * primary's horizontal extent. top_offset is negative to
         * place it above. */
        { .requested_width = 1080, .requested_height = 1920,
          .x_position = 1, .top_offset = -1920, .left_offset = 0 },
    };
    guac_rdp_disp disp = { .monitors = monitors, .monitors_count = 2 };
    guac_rdp_client* rdp_client = create_rdp_client(&disp);

    /* src buffer (100, 100)+200x200 is in monitor 1 (norm top=0).
     * dst buffer (100, 2020)+200x200 is in monitor 0 (norm top=1920). */
    CU_ASSERT_TRUE(guac_rdp_disp_should_decompose_copy(rdp_client,
            100, 100, 200, 200, 100, 2020));

    destroy_rdp_client(rdp_client);
}

/**
 * Same as test_disp__should_decompose_copy_vertical_layout but with a
 * secondary monitor positioned to the LEFT of primary (negative
 * left_offset). Covers cross-monitor copies on left-of-primary layouts,
 * where the secondary's negative wire offset must be normalized to buffer
 * space before the monitor lookup.
 */
void test_disp__should_decompose_copy_horizontal_left_layout(void) {

    guac_rdp_disp_monitor monitors[2] = {
        { .requested_width = 1920, .requested_height = 1080,
          .x_position = 0, .top_offset = 0, .left_offset = 0 },
        { .requested_width = 1920, .requested_height = 1080,
          .x_position = 1, .top_offset = 0, .left_offset = -1920 },
    };
    guac_rdp_disp disp = { .monitors = monitors, .monitors_count = 2 };
    guac_rdp_client* rdp_client = create_rdp_client(&disp);

    /* min_left = -1920. Secondary normalizes to buffer left=0,
     * primary normalizes to buffer left=1920.
     * src buffer (100, 100)+200x200 is in monitor 1 (left-of-primary).
     * dst buffer (2000, 100)+200x200 is in monitor 0. */
    CU_ASSERT_TRUE(guac_rdp_disp_should_decompose_copy(rdp_client,
            100, 100, 200, 200, 2000, 100));

    destroy_rdp_client(rdp_client);
}

/**
 * Test that a zero-size rect (width=0 or height=0) does not underflow
 * during probe computation. Behavior here is implementation-defined
 * (the 8 probes degenerate to the corner point); the contract is just
 * "must not crash, must not return a nonsense verdict that depends on
 * uninitialized memory".
 */
void test_disp__should_decompose_copy_zero_size_rect(void) {

    guac_rdp_disp_monitor monitors[2] = {
        { .requested_width = 1920, .requested_height = 1080,
          .x_position = 0, .top_offset = 0, .left_offset = 0 },
        { .requested_width = 1920, .requested_height = 1080,
          .x_position = 1, .top_offset = 0, .left_offset = 1920 },
    };
    guac_rdp_disp disp = { .monitors = monitors, .monitors_count = 2 };
    guac_rdp_client* rdp_client = create_rdp_client(&disp);

    /* width=0 — the only requirement is that the function returns
     * without dereferencing invalid memory. Both src and dst are
     * within monitor 0, so the expected verdict is false. */
    CU_ASSERT_FALSE(guac_rdp_disp_should_decompose_copy(rdp_client,
            100, 100, 0, 100, 200, 100));

    destroy_rdp_client(rdp_client);
}
