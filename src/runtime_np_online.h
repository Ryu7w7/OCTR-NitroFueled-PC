#ifndef RUNTIME_NP_ONLINE_H
#define RUNTIME_NP_ONLINE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ORBIS_NP_STATE_SIGNED_OUT 1
#define ORBIS_NP_STATE_SIGNED_IN  2

int32_t runtime_np_get_state(int32_t user, int32_t *state);
int32_t runtime_np_get_online_id(int32_t user, void *online_id);
int32_t runtime_np_get_np_id(int32_t user, void *np_id);
int32_t runtime_np_get_account_id(int32_t user, uint64_t *account_id);
int32_t runtime_np_get_user_id_by_account_id(uint64_t account_id, int32_t *user_id);
int32_t runtime_np_check_plus(int32_t req_id, const void *param, void *result);
int32_t runtime_np_check_availability(void);
int32_t runtime_np_parental_control(int32_t req_id, void *online_or_user, int8_t *age, void *info);
int32_t runtime_np_poll(int32_t req_id, int32_t *result);

int32_t runtime_np_auth_get_id_token(int32_t req_id, const void *param, void *token);
int32_t runtime_np_auth_get_auth_code(int32_t req_id, const void *param, void *code, int32_t *issuer_id);

int32_t runtime_np_register_state_cb(void *cb, void *arg);
int32_t runtime_np_register_state_cb_toolkit(void *cb, void *arg);
int32_t runtime_np_unregister_state_cb(void *cb, void *arg);
int32_t runtime_np_unregister_state_cb_toolkit(void *cb, void *arg);
int32_t runtime_np_check_callback(void);

uint64_t runtime_np_calculate_account_id(const char *name);
const char *runtime_np_get_online_name(void);
const char *runtime_np_get_friends_csv(void);

#ifdef __cplusplus
}
#endif

#endif /* RUNTIME_NP_ONLINE_H */
