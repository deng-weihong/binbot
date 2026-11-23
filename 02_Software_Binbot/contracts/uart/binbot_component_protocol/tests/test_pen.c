#include "binbot/component/pen.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    bb_component_pen_t state = {.mode=1, .face=0, .flags=3, .unix_time=1770000000,
        .utc_offset=-18000, .temperature=-35, .low=-80, .high=45, .weather_time=1769999000};
    uint8_t bytes[BB_COMPONENT_PEN_BYTES], again[BB_COMPONENT_PEN_BYTES];
    bb_component_pen_encode(&state, bytes);
    bb_component_pen_t decoded;
    assert(bb_component_pen_decode(bytes, sizeof(bytes), &decoded));
    bb_component_pen_encode(&decoded, again); assert(!memcmp(bytes, again, sizeof(bytes)));
    assert(decoded.temperature == -35 && decoded.utc_offset == -18000);
    assert(!bb_component_pen_decode(bytes, sizeof(bytes)-1, &decoded));
    bytes[22]=1; assert(!bb_component_pen_decode(bytes, sizeof(bytes), &decoded)); bytes[22]=0;
    bytes[0]=2; assert(!bb_component_pen_decode(bytes, sizeof(bytes), &decoded)); bytes[0]=1;
    state.utc_offset = 50401; assert(!bb_component_pen_valid(&state)); state.utc_offset = -18000;
    state.low = 46; assert(!bb_component_pen_valid(&state)); state.low = -80;
    bb_component_pen_advance(&state, 6201);
    assert(state.flags == BB_PEN_TIME_VALID && state.unix_time == 1770006201);
    state.unix_time = UINT32_MAX; bb_component_pen_advance(&state, 1); assert(!state.flags);
    uint8_t *frame = malloc(240*240*2+32); assert(frame);
    memset(frame, 0xa5, 240*240*2+32);
    state = (bb_component_pen_t){.flags=3,.unix_time=1770000000,.temperature=220,.low=180,.high=260};
    unsigned checksums[3] = {0};
    for(unsigned face=0;face<3;++face) {
        state.face=face; bb_component_pen_render(&state, frame);
        for(unsigned i=0;i<240*240*2;++i) checksums[face] += frame[i];
        for(unsigned i=0;i<32;++i) assert(frame[240*240*2+i] == 0xa5);
        assert(checksums[face] > 1000000);
    }
    assert(checksums[0] != checksums[1] && checksums[1] != checksums[2]);
    uint32_t hashes[BB_PEN_CONDITION_COUNT] = {0};
    state.face = BB_PEN_WEATHER;
    for(unsigned condition=0;condition<BB_PEN_CONDITION_COUNT;++condition) {
        state.condition = condition;
        assert(bb_component_pen_valid(&state));
        bb_component_pen_encode(&state, bytes);
        assert(bb_component_pen_decode(bytes, sizeof(bytes), &decoded));
        assert(decoded.condition == condition);
        bb_component_pen_render(&state, frame);
        uint32_t hash = 2166136261U;
        for(unsigned i=0;i<240*240*2;++i) hash = (hash ^ frame[i]) * 16777619U;
        hashes[condition] = hash;
        for(unsigned i=0;i<condition;++i) assert(hashes[i] != hash);
        for(unsigned i=0;i<32;++i) assert(frame[240*240*2+i] == 0xa5);
        state.face = BB_PEN_BOTH;
        bb_component_pen_render_face(&state, frame, false);
        for(unsigned i=0;i<32;++i) assert(frame[240*240*2+i] == 0xa5);
        state.face = BB_PEN_WEATHER;
    }
    bytes[3] = BB_PEN_CONDITION_COUNT;
    assert(!bb_component_pen_decode(bytes, sizeof(bytes), &decoded));
    free(frame);
    puts("pen wire validation, signed values, stale weather and all face rendering passed");
    return 0;
}
