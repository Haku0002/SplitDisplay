#pragma once
#include "protocol.h"

// Consumer-side view of the driver's shared memory.
class SharedView
{
public:
    ~SharedView();
    bool Open();       // false if the driver is not loaded
    void Close();
    SdShared* operator->() const { return m_Shm; }
    SdShared* get() const { return m_Shm; }
    void Kick() const; // wake the driver's command thread

private:
    HANDLE m_Section = nullptr;
    HANDLE m_Event = nullptr;
    SdShared* m_Shm = nullptr;
};
