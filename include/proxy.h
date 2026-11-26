#pragma once

#ifdef __cplusplus
extern "C" {
#endif

bool start_sponsorblock_proxy();
void stop_sponsorblock_proxy();
void set_update_status(const char* status);

#ifdef __cplusplus
}
#endif
