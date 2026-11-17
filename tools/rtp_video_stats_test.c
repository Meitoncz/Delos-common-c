// Self-contained check of the RTP_VIDEO_STATS counters. It links the real
// RtpVideoQueue.c and nanors against small stubs, so it needs no network, no
// OpenSSL and no running stream. Build and run: tools/rtp_video_stats_test.sh

#include "Limelight-internal.h"
#include "Video.h"
#include "RtpVideoQueue.h"

#include <rs.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- Stubs for the symbols RtpVideoQueue.c expects from the rest of the core ----
int AppVersionQuad[4] = {7, 1, 431, 0};
STREAM_CONFIGURATION StreamConfig;
CONNECTION_LISTENER_CALLBACKS ListenerCallbacks;
int NegotiatedVideoFormat = VIDEO_FORMAT_H264;

static int framesSubmitted;
static int framesLost;

uint64_t PltGetMicroseconds(void) { return 1000000; }
bool isReferenceFrameInvalidationEnabled(void) { return false; }
void connectionSawFrame(uint32_t frameIndex) { (void)frameIndex; }
void connectionSendFrameFecStatus(PSS_FRAME_FEC_STATUS status) { (void)status; }
void notifyFrameLost(unsigned int frameNumber, bool speculative) { (void)frameNumber; (void)speculative; framesLost++; }
void queueRtpPacket(PRTPV_QUEUE_ENTRY entry) {
    if (entry->packet->sequenceNumber != 0xFFFF) {
        framesSubmitted++;
    }
    free(entry->packet);
}

// ---- Packet construction ----
#define PACKET_SIZE 64
#define RECEIVE_SIZE (PACKET_SIZE + MAX_RTP_HEADER_SIZE)
#define HEADER_SIZE (sizeof(RTP_PACKET) + 4)

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void expectStats(PRTP_VIDEO_QUEUE q, const char* name, uint32_t video, uint32_t fec,
                        uint32_t recovered, uint32_t failed, uint32_t oos, uint32_t invalid, uint32_t fecInvalid) {
    RTP_VIDEO_STATS s;
    RtpvGetStats(q, &s);
    printf("%-22s video=%u fec=%u recovered=%u failed=%u oos=%u invalid=%u fecInvalid=%u\n", name,
           s.packetCountVideo, s.packetCountFec, s.packetCountFecRecovered, s.packetCountFecFailed,
           s.packetCountOOS, s.packetCountInvalid, s.packetCountFecInvalid);
    CHECK(s.packetCountVideo == video);
    CHECK(s.packetCountFec == fec);
    CHECK(s.packetCountFecRecovered == recovered);
    CHECK(s.packetCountFecFailed == failed);
    CHECK(s.packetCountOOS == oos);
    CHECK(s.packetCountInvalid == invalid);
    CHECK(s.packetCountFecInvalid == fecInvalid);
}

typedef struct {
    uint8_t* shards[8];
    int data, parity;
    uint16_t baseSeq;
    uint32_t frame;
} FRAME;

// Builds data+parity shards (full RTP buffers, as the receiver sees them).
// firstFlags overrides the SOF flag of the first packet when 0 (to corrupt a recovery).
static void buildFrame(FRAME* f, uint32_t frame, uint16_t baseSeq, int data, int fecPercent, uint8_t firstFlags) {
    int parity = (data * fecPercent + 99) / 100;
    f->data = data;
    f->parity = parity;
    f->baseSeq = baseSeq;
    f->frame = frame;
    for (int i = 0; i < data + parity; i++) {
        uint8_t* b = calloc(1, RECEIVE_SIZE + sizeof(RTPV_QUEUE_ENTRY));
        PRTP_PACKET rtp = (PRTP_PACKET)b;
        rtp->header = FLAG_EXTENSION;
        rtp->sequenceNumber = (uint16_t)(baseSeq + i);
        rtp->timestamp = frame * 1500;
        PNV_VIDEO_PACKET nv = (PNV_VIDEO_PACKET)(b + HEADER_SIZE);
        nv->frameIndex = frame;
        nv->streamPacketIndex = (uint32_t)((baseSeq + i) << 8);
        nv->multiFecFlags = 0x10;
        nv->multiFecBlocks = 0;
        nv->fecInfo = ((uint32_t)data << 22) | ((uint32_t)i << 12) | ((uint32_t)fecPercent << 4);
        if (i < data) {
            nv->flags = FLAG_CONTAINS_PIC_DATA;
            if (i == 0) nv->flags = firstFlags;
            if (i == data - 1) nv->flags = FLAG_EOF;
            memset(b + HEADER_SIZE + sizeof(NV_VIDEO_PACKET), 0x30 + i, PACKET_SIZE - sizeof(NV_VIDEO_PACKET));
        }
        f->shards[i] = b;
    }
    if (parity) {
        reed_solomon* rs = reed_solomon_new(data, parity);
        // Parity shards are computed over the entire buffers; encode in place on a pointer table
        uint8_t* table[8];
        for (int i = 0; i < data + parity; i++) table[i] = f->shards[i];
        reed_solomon_encode(rs, table, data + parity, RECEIVE_SIZE);
        reed_solomon_release(rs);
        // Parity packets must keep their own RTP/NV header fields
        for (int i = data; i < data + parity; i++) {
            PRTP_PACKET rtp = (PRTP_PACKET)f->shards[i];
            // encode overwrote the headers; rewrite them
            rtp->header = FLAG_EXTENSION;
            rtp->sequenceNumber = (uint16_t)(baseSeq + i);
            rtp->timestamp = frame * 1500;
            PNV_VIDEO_PACKET nv = (PNV_VIDEO_PACKET)(f->shards[i] + HEADER_SIZE);
            nv->frameIndex = frame;
            nv->multiFecFlags = 0x10;
            nv->multiFecBlocks = 0;
            nv->fecInfo = ((uint32_t)data << 22) | ((uint32_t)i << 12) | ((uint32_t)fecPercent << 4);
        }
    }
}

// Hands shard i to the queue. The queue owns it when queued; otherwise we free it.
static int feed(PRTP_VIDEO_QUEUE q, FRAME* f, int i) {
    int r = RtpvAddPacket(q, (PRTP_PACKET)f->shards[i], RECEIVE_SIZE,
                          (PRTPV_QUEUE_ENTRY)(f->shards[i] + RECEIVE_SIZE));
    if (getenv("TRACE")) printf("  feed %d -> %d (frame %u submitted %d)\n", i, r, RtpvGetCurrentFrameNumber(q), framesSubmitted);
    if (r != RTPF_RET_QUEUED) free(f->shards[i]);
    f->shards[i] = NULL;
    return r;
}

static void freeRest(FRAME* f) {
    for (int i = 0; i < f->data + f->parity; i++) { free(f->shards[i]); f->shards[i] = NULL; }
}

static void newQueue(PRTP_VIDEO_QUEUE q) {
    RtpvInitializeQueue(q);
    framesSubmitted = framesLost = 0;
}

static void logSink(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("  [core] ");
    vprintf(fmt, ap);
    va_end(ap);
}

int main(void) {
    ListenerCallbacks.logMessage = logSink;
    reed_solomon_init();
    StreamConfig.packetSize = PACKET_SIZE;
    RTP_VIDEO_QUEUE q;
    FRAME f;

    // 1. Clean frame: nothing but the totals move.
    newQueue(&q);
    buildFrame(&f, 1, 100, 4, 25, FLAG_SOF | FLAG_CONTAINS_PIC_DATA);
    for (int i = 0; i < 5; i++) feed(&q, &f, i);
    expectStats(&q, "clean", 4, 1, 0, 0, 0, 0, 0);
    CHECK(framesSubmitted == 4);
    RtpvCleanupQueue(&q);

    // 2. One data packet lost, rebuilt from parity: FecRecovered == 1.
    newQueue(&q);
    buildFrame(&f, 1, 100, 4, 25, FLAG_SOF | FLAG_CONTAINS_PIC_DATA);
    free(f.shards[2]); f.shards[2] = NULL;
    feed(&q, &f, 0); feed(&q, &f, 1); feed(&q, &f, 3); feed(&q, &f, 4);
    expectStats(&q, "recovered", 4, 1, 1, 0, 0, 0, 0);
    CHECK(framesSubmitted == 4);
    RtpvCleanupQueue(&q);

    // 3. Reordered packet counts once; a duplicate counts nowhere.
    newQueue(&q);
    buildFrame(&f, 1, 100, 4, 0, FLAG_SOF | FLAG_CONTAINS_PIC_DATA);
    feed(&q, &f, 0); feed(&q, &f, 2);
    expectStats(&q, "before late packet", 4, 0, 0, 0, 0, 0, 0);
    feed(&q, &f, 1);
    expectStats(&q, "oos", 4, 0, 0, 0, 1, 0, 0);
    uint8_t* dup = malloc(RECEIVE_SIZE + sizeof(RTPV_QUEUE_ENTRY));
    memcpy(dup, f.shards[3], RECEIVE_SIZE);
    PRTP_PACKET dupRtp = (PRTP_PACKET)dup;
    dupRtp->sequenceNumber = 101;
    CHECK(RtpvAddPacket(&q, dupRtp, RECEIVE_SIZE, (PRTPV_QUEUE_ENTRY)(dup + RECEIVE_SIZE)) == RTPF_RET_REJECTED);
    free(dup);
    feed(&q, &f, 3);
    expectStats(&q, "after duplicate", 4, 0, 0, 0, 1, 0, 0);
    RtpvCleanupQueue(&q);

    // 4. Truncated packet: Invalid.
    newQueue(&q);
    buildFrame(&f, 1, 100, 4, 25, FLAG_SOF | FLAG_CONTAINS_PIC_DATA);
    CHECK(RtpvAddPacket(&q, (PRTP_PACKET)f.shards[0], (int)HEADER_SIZE + 4,
                        (PRTPV_QUEUE_ENTRY)(f.shards[0] + RECEIVE_SIZE)) == RTPF_RET_REJECTED);
    expectStats(&q, "invalid", 0, 0, 0, 0, 0, 1, 0);
    freeRest(&f);
    RtpvCleanupQueue(&q);

    // 5. Too many packets lost: when the next frame starts, FecFailed == 1.
    newQueue(&q);
    buildFrame(&f, 1, 100, 4, 25, FLAG_SOF | FLAG_CONTAINS_PIC_DATA);
    feed(&q, &f, 0); feed(&q, &f, 1);
    freeRest(&f);
    FRAME g;
    buildFrame(&g, 2, 105, 4, 25, FLAG_SOF | FLAG_CONTAINS_PIC_DATA);
    feed(&q, &g, 0);
    expectStats(&q, "fec failed", 8, 2, 0, 1, 0, 0, 0);
    CHECK(framesSubmitted == 0);
    freeRest(&g);
    RtpvCleanupQueue(&q);

    // 6. Recovered first packet lacks SOF (corrupt recovery): FecInvalid == 1, nothing counted as recovered.
    newQueue(&q);
    buildFrame(&f, 1, 100, 4, 25, FLAG_CONTAINS_PIC_DATA);
    free(f.shards[0]); f.shards[0] = NULL;
    feed(&q, &f, 1); feed(&q, &f, 2); feed(&q, &f, 3); feed(&q, &f, 4);
    expectStats(&q, "fec invalid", 4, 1, 0, 0, 0, 0, 1);
    RtpvCleanupQueue(&q);

    printf(failures ? "FAILED (%d)\n" : "OK\n", failures);
    return failures ? 1 : 0;
}
