#pragma once

#include "AsstPort.h"

#include <stdint.h>

struct AsstScreencapAPI;
typedef struct AsstScreencapAPI* AsstScreencapHandle;

typedef uint8_t AsstBool;
typedef uint64_t AsstSize;

#ifdef __cplusplus
extern "C"
{
#endif
    AsstScreencapHandle ASSTAPI
        AsstCreateMumuScreencap(const char* mumu_path, int32_t mumu_index, const char* package_name);

#ifdef _WIN32
    AsstScreencapHandle ASSTAPI
        AsstCreateWin32Screencap(void* hwnd, uint64_t screencap_method, const char* control_unit_dll_path);
#endif

    void ASSTAPI AsstDestroyScreencap(AsstScreencapHandle handle);
    AsstBool ASSTAPI AsstScreencapCapture(AsstScreencapHandle handle);
    AsstBool ASSTAPI AsstGetScreencapImageInfo(
        AsstScreencapHandle handle,
        int32_t* width,
        int32_t* height,
        int32_t* channels,
        AsstSize* data_size);
    AsstSize ASSTAPI AsstGetScreencapImage(AsstScreencapHandle handle, void* buff, AsstSize buff_size);
#ifdef __cplusplus
}
#endif
