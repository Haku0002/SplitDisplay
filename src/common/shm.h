#pragma once
#include "protocol.h"

// Consumer-side view of the driver's shared memory.
class SharedView
{
public:
    ~SharedView();
    bool Open();               // read-write view + command event (elevated compositor / controller)
    bool OpenReadOnly();       // read-only view (unprivileged GUI / status query)
    void Close();
    SdShared* operator->() const { return m_Shm; }
    SdShared* get() const { return m_Shm; }
    void Kick() const;         // wake the driver's command thread

    // True if a shared-memory section from an earlier protocol version (v1..v4) is present in
    // the kernel namespace, meaning an older driver is loaded and needs an upgrade.
    static bool IsOlderVersionLoaded();

private:
    HANDLE m_Section = nullptr;
    HANDLE m_Event = nullptr;
    SdShared* m_Shm = nullptr;
};
