#pragma once
// Shared-memory contract between the IDD driver (WUDFHost, session 0) and the compositor.
//
// The driver creates the section and the command event. The compositor/controller opens them,
// writes the desired config and sets desiredPlugged, then signals the command event.
// config is guarded by the configGeneration seqlock (odd = being written); always go through
// SdWriteConfig / SdReadConfig, never assign SdShared::config directly. Bumping
// modesGeneration (after a config write) asks the driver to refresh the plugged monitors
// mode lists in place (IddCxMonitorUpdateModes) instead of re-plugging them.
//
// Frames: per virtual monitor the driver keeps SD_BUFFERS shared textures plus one shared fence.
// Handle values are valid inside driverPid; the consumer duplicates them with DuplicateHandle.
// Buffer selection protocol (lock-free, single producer / single consumer per monitor):
//   producer writes into w != latest && w != readerIndex, signals fence, then publishes latest = w.
//   consumer sets readerIndex = latest, re-reads latest; if unchanged the buffer is safe to read
//   until the consumer resets readerIndex to -1 (after its GPU copy has completed).
// handleGeneration is a seqlock: odd while the driver is replacing handles. Consumers must key
// the handles they hold on (driverEpoch, handleGeneration): a restarted driver resets the
// generation counter, so generation alone can collide with handles from the dead instance.
//
// Dirty rects: every published frame appends one journal entry (indexed by frameSeq) listing
// what changed relative to the previous frame. A consumer bringing a back buffer from seq A to
// seq B copies the union of entries (A, B]; entries are validated with seq == seqEnd because an
// entry may be mid-rewrite for a frame 16 ahead. full != 0 means "everything changed".

#include <Windows.h>

#define SD_SHM_NAME L"Global\\SplitDisplay.Shm.v5"
#define SD_CMD_EVENT_NAME L"Global\\SplitDisplay.Cmd.v5"

constexpr UINT32 SD_MAGIC = 0x35445053; // 'SPD5'
constexpr UINT32 SD_VERSION = 5;
constexpr int SD_MAX_MONITORS = 16;
constexpr int SD_BUFFERS = 3;

constexpr int SD_MAX_RATES = 8;
constexpr int SD_MAX_DIRTY = 24;     // rects kept per journal entry (more collapse to a bounding box)
constexpr int SD_DIRTY_JOURNAL = 16; // frames of dirty history per monitor

struct SdRect
{
    INT32 left, top, right, bottom;
};

struct SdDirtyFrame
{
    UINT64 seq;    // frame this entry describes
    UINT64 seqEnd; // == seq once the entry is fully written (torn-read tripwire)
    UINT32 count;  // valid rects in rects[]
    UINT32 full;   // nonzero: the whole frame changed, rects are meaningless
    SdRect rects[SD_MAX_DIRTY];
};

struct SdMonitorConfig
{
    UINT32 width;
    UINT32 height;
    UINT32 refreshMilliHz;            // current rate of the physical panel (preferred mode), milli-Hz
    UINT32 rateCount;                 // other rates the panel supports, offered as alternatives; changing
    UINT32 ratesMilliHz[SD_MAX_RATES]; // one of them makes SplitDisplay switch the whole panel to it
    UINT32 widthMm;                   // physical size of this region on the real panel (EDID), 0 = guess
    UINT32 heightMm;
};

struct SdConfig
{
    UINT32 count;                          // virtual monitors to plug (1..SD_MAX_MONITORS)
    UINT32 reserved;
    LUID renderAdapter;                    // GPU the physical panels are attached to
    SdMonitorConfig mon[SD_MAX_MONITORS];  // monitor i is "Split <i+1>"
};

struct SdMonitorFrames
{
    volatile LONG plugged;          // driver: monitor currently reported to the OS
    volatile LONG handleGeneration; // seqlock, see above
    volatile LONG latest;           // newest complete buffer, -1 = none
    volatile LONG readerIndex;      // buffer the consumer is reading, -1 = none
    volatile LONG64 frameSeq;       // sequence number of the newest published frame (0 = none)
    UINT64 fenceValue[SD_BUFFERS];  // fence value marking buffer i complete
    UINT64 texHandle[SD_BUFFERS];   // NT handles in driver process
    UINT64 fenceHandle;
    UINT64 presentQpc[SD_BUFFERS];  // DWM target display time of buffer i (PresentDisplayQPCTime)
    UINT64 slotSeq[SD_BUFFERS];     // frameSeq written into buffer i
    LUID adapter;                   // adapter the textures live on
    UINT32 width, height, format;   // DXGI_FORMAT
    UINT32 reserved;
    SdDirtyFrame journal[SD_DIRTY_JOURNAL];
};

struct SdShared
{
    UINT32 magic;
    UINT32 version;
    UINT32 driverPid;
    UINT32 driverEpoch;              // nonzero, changes every time the driver (re)creates this state
    volatile LONG desiredPlugged;    // controller -> driver
    volatile LONG configGeneration;  // seqlock over config
    volatile LONG modesGeneration;   // controller -> driver: refresh mode lists in place
    SdConfig config;                 // controller -> driver, read when (re)plugging; seqlock-guarded
    volatile LONG64 driverHeartbeat; // GetTickCount64 of the driver's command thread
    SdMonitorFrames mon[SD_MAX_MONITORS];
};

inline void SdWriteConfig(SdShared* s, const SdConfig& cfg)
{
    InterlockedIncrement(&s->configGeneration); // odd: write in progress
    MemoryBarrier();
    s->config = cfg;
    MemoryBarrier();
    InterlockedIncrement(&s->configGeneration); // even: stable
}

inline bool SdReadConfig(SdShared* s, SdConfig& out)
{
    for (int attempt = 0; attempt < 1000; attempt++)
    {
        LONG g1 = s->configGeneration;
        if (g1 & 1) continue;
        MemoryBarrier();
        out = s->config;
        MemoryBarrier();
        if (s->configGeneration == g1) return true;
    }
    out = SdConfig{};
    return false;
}
