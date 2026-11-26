#pragma once
#ifdef __cplusplus
extern "C" {
#endif
void start_update_check_after_welcome_toast(void);
void stop_update_checker(void);
bool request_update_install(void);
bool request_manual_update_check(void);
#ifdef __cplusplus
}
#endif
