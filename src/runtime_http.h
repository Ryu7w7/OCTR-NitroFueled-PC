#ifndef RUNTIME_HTTP_H
#define RUNTIME_HTTP_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int32_t runtime_http_init(void);
int32_t runtime_http_term(void);

int32_t runtime_http_create_template(int32_t http_ctx, const char *name, int32_t version, int32_t auto_redirect);
int32_t runtime_http_delete_template(int32_t tmpl);

int32_t runtime_http_create_connection(int32_t tmpl, const char *url, int32_t keep_alive);
int32_t runtime_http_delete_connection(int32_t conn);

int32_t runtime_http_create_request(int32_t conn, int32_t method, const char *path, uint64_t content_length);
int32_t runtime_http_create_request2(int32_t conn, const char *method, const char *path, uint64_t content_length);
int32_t runtime_http_delete_request(int32_t req);

int32_t runtime_http_set_request_content_length(int32_t req, uint64_t length);
int32_t runtime_http_add_request_header(int32_t req, const char *name, const char *value, int32_t mode);
int32_t runtime_http_send_request(int32_t req, const void *post_data, size_t size);
int32_t runtime_http_abort_request(int32_t req);

int32_t runtime_http_get_status_code(int32_t req, int32_t *status_code);
int32_t runtime_http_get_response_content_length(int32_t req, int32_t *result, uint64_t *content_length);
int32_t runtime_http_read_data(int32_t req, void *data, uint64_t size);
int32_t runtime_http_get_all_response_headers(int32_t req, char **headers, uint64_t *length);
/* PS4-faithful: parses a header blob (as returned by get_all_response_headers).
 * Sets *value to a pointer INSIDE the blob (not NUL-terminated) and *length to
 * the trimmed value length. Returns >0 (end offset) on success, or a negative
 * 0x8043xxxx error when the header is missing/invalid. */
int32_t runtime_http_parse_response_header(const char *blob, uint64_t blob_len, const char *name,
                                           const char **value, uint64_t *length);

int32_t runtime_http_create_epoll(int32_t ctx, void **handle);
int32_t runtime_http_destroy_epoll(void *handle);
int32_t runtime_http_set_epoll(void *handle, int32_t req, void *user_data);
int32_t runtime_http_unset_epoll(void *handle, int32_t req);
int32_t runtime_http_wait_request(void *handle, void *events, int32_t max_events, int64_t timeout_usec);

/* The port's own signed requests to the RyuNET server (runtime_friends.c). */
int runtime_ryunet_proof(const char *method, const char *target, char *out, size_t out_size);
int runtime_ryunet_request(const char *method, const char *target, char *body, size_t body_size);

#ifdef __cplusplus
}
#endif

#endif /* RUNTIME_HTTP_H */
