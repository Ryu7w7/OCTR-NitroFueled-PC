#include "../src/runtime_services.c"
#include <assert.h>

static char friends_csv[NP_FRIENDS_CSV_CAPACITY];
const char *runtime_np_get_friends_csv(void) { return friends_csv; }
const char *runtime_np_get_online_name(void) { return "LocalPlayer"; }
uint64_t runtime_np_calculate_account_id(const char *name) {
    uint64_t hash = 1;
    while (*name) hash = hash * 31 + (unsigned char)*name++;
    return hash;
}
void runtime_net_trace(const char *format, ...) { (void)format; }

int main(void) {
    const int counts[] = {0, 1, 32, RF_MAX_FRIENDS};
    for (unsigned c = 0; c < sizeof counts / sizeof counts[0]; ++c) {
        size_t pos = 0;
        friends_csv[0] = 0;
        for (int i = 0; i < counts[c]; ++i)
            pos += snprintf(friends_csv + pos, sizeof friends_csv - pos,
                            "%sFriend%010d", i ? "," : "", i);
        char *responses[] = {generate_friends_json(), generate_profiles_json(""),
                             generate_friend_online_id_lists_json()};
        for (unsigned i = 0; i < sizeof responses / sizeof responses[0]; ++i) {
            assert(responses[i]);
            puts(responses[i]);
            free(responses[i]);
        }
    }
}
