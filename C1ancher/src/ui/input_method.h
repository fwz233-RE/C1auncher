#ifndef C1_UI_INPUT_METHOD_H
#define C1_UI_INPUT_METHOD_H
#include "c1_ime_client.h"
#include <stdbool.h>
#include <stdint.h>
#define C1_INPUT_QUEUE_SIZE 64U

typedef struct {
    struct c1_ime_client client;
    struct c1_ime_request queue[C1_INPUT_QUEUE_SIZE];
    /* Last acknowledged candidate page, never speculative queued input. */
    struct c1_ime_response view;
    struct c1_ime_request original_request;
    unsigned head, count;
    int64_t deadline;
    bool enabled;
    bool failed;
} c1_input_method;
void c1_input_method_init(c1_input_method *input);
void c1_input_method_close(c1_input_method *input);
bool c1_input_method_toggle(c1_input_method *input, const char *socket_path, int64_t now);
bool c1_input_method_key(c1_input_method *input, uint32_t key, uint32_t modifiers);
/* No waiting. 1=ordered response (wire or local highlight/navigation), 0=none,
 * -1=transport failure. Local replies carry the last acknowledged sequence and
 * an empty commit. Lost/ambiguous keys are never replayed; caller must present
 * a visible error, not execute them twice. */
int c1_input_method_tick(c1_input_method *input, struct c1_ime_response *response, int64_t now);
short c1_input_method_poll_events(const c1_input_method *input);
#endif
