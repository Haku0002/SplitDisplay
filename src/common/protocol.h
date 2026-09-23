#pragma once
// Shared-memory contract between the IDD driver (WUDFHost, session 0) and the compositor.
//
// The driver creates the section and the command event. The compositor/controller opens them,
// writes the desired config and sets desiredPlugged, then signals the command event.
//
// Frames: per virtual monitor the driver keeps SD_BUFFERS shared textures plus one shared fence.
// Handle values are valid inside driverPid; the consumer duplicates them with DuplicateHandle.
// Buffer selection protocol (lock-free, single producer / single consumer per monitor):
//   producer writes into w != latest && w != readerIndex, signals fence, then publishes latest = w.
//   consumer sets readerIndex = latest, re-reads latest; if unchanged the buffer is safe to read
//   until the consumer resets readerIndex to -1 (after its GPU copy has completed).
// handleGeneration is a seqlock: odd while the driver is replacing handles.

#include <Windows.h>

#define SD_SHM_NAME L"Global\\SplitDisplay.Shm.v3"
#define SD_CMD_EVENT_NAME L"Global\\SplitDisplay.Cmd.v3"

constexpr UINT32 SD_MAGIC = 0x33445053; // 'SPD3'
constexpr UINT32 SD_VERSION = 3;
constexpr int SD_MAX_MONITORS = 16;
constexpr int SD_BUFFERS = 3;

struct SdMonitorConfig
{
    UINT32 width;
    UINT32 height;
    UINT32 refreshHz; // the refresh rate of the physical panel this monitor is part of
    UINT32 reserved;
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
    volatile LONG64 frameSeq;       // number of frames published
    UINT64 fenceValue[SD_BUFFERS];  // fence value marking buffer i complete
    UINT64 texHandle[SD_BUFFERS];   // NT handles in driver process
    UINT64 fenceHandle;
    LUID adapter;                   // adapter the textures live on
    UINT32 width, height, format;   // DXGI_FORMAT
    UINT32 reserved;
};

struct SdShared
{
    UINT32 magic;
    UINT32 version;
    UINT32 driverPid;
    volatile LONG desiredPlugged;   // controller -> driver
    SdConfig config;                // controller -> driver, read when plugging
    volatile LONG64 driverHeartbeat; // GetTickCount64 of the driver's command thread
    SdMonitorFrames mon[SD_MAX_MONITORS];
};
