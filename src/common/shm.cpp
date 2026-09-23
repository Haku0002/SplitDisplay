#include "shm.h"

SharedView::~SharedView()
{
    Close();
}

bool SharedView::Open()
{
    Close();
    m_Section = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, SD_SHM_NAME);
    m_Event = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, SD_CMD_EVENT_NAME);
    if (!m_Section || !m_Event)
    {
        Close();
        return false;
    }
    m_Shm = (SdShared*)MapViewOfFile(m_Section, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SdShared));
    if (!m_Shm || m_Shm->magic != SD_MAGIC || m_Shm->version != SD_VERSION)
    {
        Close();
        return false;
    }
    return true;
}

void SharedView::Close()
{
    if (m_Shm) UnmapViewOfFile(m_Shm);
    if (m_Section) CloseHandle(m_Section);
    if (m_Event) CloseHandle(m_Event);
    m_Shm = nullptr;
    m_Section = m_Event = nullptr;
}

void SharedView::Kick() const
{
    if (m_Event) SetEvent(m_Event);
}
