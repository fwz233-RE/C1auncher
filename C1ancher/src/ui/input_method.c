#include "ui/input_method.h"
#include <poll.h>
#include <string.h>

void c1_input_method_init(c1_input_method *input)
{
    memset(input, 0, sizeof(*input));
    input->client = (struct c1_ime_client)C1_IME_CLIENT_INIT;
    input->deadline = -1;
}

void c1_input_method_close(c1_input_method *input)
{
    c1_ime_close(&input->client);
    c1_input_method_init(input);
}

static bool push(c1_input_method *input, struct c1_ime_request request)
{
    if (input->count == C1_INPUT_QUEUE_SIZE) {
        input->failed = true; /* Visible backpressure; never forward out of order. */
        return false;
    }
    input->queue[(input->head + input->count++) % C1_INPUT_QUEUE_SIZE] = request;
    return true;
}

bool c1_input_method_toggle(c1_input_method *input, const char *path, int64_t now)
{
    if (input->failed && input->client.fd >= 0) return false;
    if (input->client.fd < 0) {
        if (c1_ime_connect(&input->client, path) < 0) { input->failed = true; return false; }
        input->deadline = now + 5000;
    }
    bool enabled = !input->enabled;
    if (!push(input, (struct c1_ime_request){C1_IME_OP_MODE, 0, 0,
              enabled ? C1_IME_MODE_CHINESE : C1_IME_MODE_ENGLISH})) return false;
    input->enabled = enabled;
    input->failed = false;
    return true;
}

bool c1_input_method_key(c1_input_method *input, uint32_t key, uint32_t modifiers)
{
    if (input->client.fd < 0 || (!input->enabled && !input->count && !input->client.pending_sequence)) return false;
    /* With no preceding work or composition, let the physical volume path
     * retain scrollback/repeat behavior. Pending input must still be ordered. */
    if ((key == C1_IME_KEY_PAGE_UP || key == C1_IME_KEY_PAGE_DOWN) &&
        !input->count && !input->client.pending_sequence && !(input->view.flags & C1_IME_COMPOSING)) return false;
    (void)push(input, (struct c1_ime_request){C1_IME_OP_KEY, key, modifiers, 0});
    return true; /* Even a full queue must not leak the key into the shell. */
}

/* Resolve only the queue head after every earlier wire response was delivered.
 * Left/right is local page metadata; SELECT/PAGE remain the existing v1 wire
 * operations. Never select an index from the view at physical key arrival. */
static bool resolve_key(c1_input_method *input, struct c1_ime_request *request,
                        struct c1_ime_response *response)
{
    const uint32_t flags = input->view.flags;
    const uint32_t key = request->keysym;
    bool local = false, consumed = false;
    bool composing = (flags & C1_IME_COMPOSING) != 0;
    if (request->operation != C1_IME_OP_KEY || !(flags & C1_IME_READY) ||
        !(flags & C1_IME_CHINESE) || (flags & C1_IME_PASSWORD)) return false;
    if (request->modifiers && !(request->modifiers == C1_IME_MOD_SHIFT && key >= '1' && key <= '5'))
        return false;
    if (key == C1_IME_KEY_LEFT || key == C1_IME_KEY_RIGHT ||
        key == C1_IME_KEY_UP || key == C1_IME_KEY_DOWN) {
        local = true;
        consumed = composing;
        if (composing && input->view.candidate_count) {
            if (key == C1_IME_KEY_LEFT && input->view.highlighted_candidate)
                --input->view.highlighted_candidate;
            if (key == C1_IME_KEY_RIGHT && input->view.highlighted_candidate + 1U < input->view.candidate_count)
                ++input->view.highlighted_candidate;
        }
    } else if (key == C1_IME_KEY_PAGE_UP || key == C1_IME_KEY_PAGE_DOWN) {
        if (composing) {
            *request = (struct c1_ime_request){C1_IME_OP_PAGE, 0, 0,
                key == C1_IME_KEY_PAGE_DOWN ? C1_IME_PAGE_NEXT : C1_IME_PAGE_PREVIOUS};
        } else local = true; /* Caller retains its non-IME volume/navigation action. */
    } else if (composing && (key == C1_IME_KEY_SPACE || key == C1_IME_KEY_RETURN ||
                            (key >= '1' && key <= '5'))) {
        uint32_t index = key >= '1' && key <= '5' ? key - '1' : input->view.highlighted_candidate;
        if (index < input->view.candidate_count)
            *request = (struct c1_ime_request){C1_IME_OP_SELECT, 0, 0, index};
        else { local = true; consumed = true; } /* Empty/short page never commits a hidden slot. */
    }
    if (!local) return false;
    *response = input->view;
    response->request = *request;
    response->flags = (flags & ~(uint32_t)C1_IME_CONSUMED) | (consumed ? C1_IME_CONSUMED : 0U);
    response->commit[0] = '\0';
    return true;
}

static void pop(c1_input_method *input)
{
    input->head = (input->head + 1U) % C1_INPUT_QUEUE_SIZE;
    --input->count;
}

int c1_input_method_tick(c1_input_method *input, struct c1_ime_response *response, int64_t now)
{
    if (input->client.fd < 0) return 0;
    if (input->failed) goto failed;
    int received = c1_ime_receive(&input->client, response);
    if (received < 0) goto failed;
    if (received == 1) {
        input->deadline = -1;
        response->request = input->original_request;
        response->highlighted_candidate = 0;
        input->view = *response;
        input->view.commit[0] = '\0'; /* Local highlight replies never duplicate commits. */
        /* Deliver a valid commit before attempting another operation. A later
         * send failure must never discard this response. */
        return 1;
    }
    if (input->deadline >= 0 && now >= input->deadline) goto failed;
    if (!input->client.pending_sequence && input->count) {
        struct c1_ime_request original = input->queue[input->head], request = original;
        if (resolve_key(input, &request, response)) {
            pop(input);
            return 1;
        }
        if (input->deadline < 0) input->deadline = now + 5000;
        int sent = c1_ime_send(&input->client, &request);
        if (sent < 0) goto failed;
        if (sent == 1) {
            input->original_request = original;
            pop(input);
            input->deadline = now + 5000;
        }
    }
    if (!input->enabled && !input->count && !input->client.pending_sequence) c1_ime_close(&input->client);
    return received;
failed:
    c1_input_method_close(input);
    input->failed = true;
    return -1;
}

short c1_input_method_poll_events(const c1_input_method *input)
{
    return POLLIN | ((input->count && !input->client.pending_sequence) ? POLLOUT : 0);
}
