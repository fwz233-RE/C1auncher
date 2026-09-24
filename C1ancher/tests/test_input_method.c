#include "ui/input_method.h"
#include <assert.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>

/* Deterministic transport; real Unix socket/Rime tests live in integration/. */
static int send_result = 1, receive_result, connect_result;
static struct c1_ime_request sent[128];
static unsigned sent_count;
static struct c1_ime_response next_reply;
int c1_ime_connect(struct c1_ime_client *client, const char *path)
{ (void)path; if (connect_result < 0) return -1; client->fd = 10; return 0; }
void c1_ime_close(struct c1_ime_client *client)
{ *client = (struct c1_ime_client)C1_IME_CLIENT_INIT; }
int c1_ime_send(struct c1_ime_client *client, const struct c1_ime_request *request)
{
    assert(!client->pending_sequence);
    if (send_result == 1) {
        sent[sent_count++] = *request;
        client->pending_request = *request;
        client->pending_sequence = client->next_sequence++;
    }
    return send_result;
}
int c1_ime_receive(struct c1_ime_client *client, struct c1_ime_response *response)
{
    if (receive_result == 1) {
        assert(client->pending_sequence);
        *response = next_reply;
        response->request = client->pending_request;
        client->pending_sequence = 0;
        receive_result = 0;
        return 1;
    }
    return receive_result;
}
static void reset(c1_input_method *input)
{
    c1_input_method_init(input);
    send_result = 1; receive_result = 0; connect_result = 0; sent_count = 0;
    memset(&next_reply, 0, sizeof(next_reply));
}
static void acknowledge(c1_input_method *input, struct c1_ime_response *reply,
                        unsigned candidates, bool composing, int64_t now)
{
    memset(&next_reply, 0, sizeof(next_reply));
    next_reply.flags = C1_IME_READY | C1_IME_CHINESE | C1_IME_CONSUMED |
                       (composing ? C1_IME_COMPOSING : 0U);
    next_reply.candidate_count = candidates;
    if (composing) strcpy(next_reply.preedit, "ni");
    for (unsigned i = 0; i < candidates; ++i)
        snprintf(next_reply.candidates[i], sizeof(next_reply.candidates[i]), "candidate-%u", i);
    receive_result = 1;
    assert(c1_input_method_tick(input, reply, now) == 1);
}

static void candidate_interaction(void)
{
    c1_input_method input;
    struct c1_ime_response reply;
    reset(&input);
    assert(c1_input_method_toggle(&input, NULL, 0));
    /* All keys arrive before any view is ready. Resolve against their own
     * preceding response, not an empty or previous page at enqueue time. */
    assert(c1_input_method_key(&input, 'n', 0));
    assert(c1_input_method_key(&input, C1_IME_KEY_RIGHT, 0));
    assert(c1_input_method_key(&input, C1_IME_KEY_RIGHT, 0));
    assert(c1_input_method_key(&input, C1_IME_KEY_LEFT, 0));
    assert(c1_input_method_key(&input, C1_IME_KEY_SPACE, 0));
    assert(c1_input_method_tick(&input, &reply, 0) == 0);
    acknowledge(&input, &reply, 0, false, 1);
    assert(c1_input_method_tick(&input, &reply, 2) == 0);
    assert(sent_count == 2 && sent[1].keysym == 'n');
    assert(c1_input_method_tick(&input, &reply, 3) == 0 && sent_count == 2);
    acknowledge(&input, &reply, 5, true, 4);
    for (unsigned i = 0; i < 3; ++i) {
        assert(c1_input_method_tick(&input, &reply, 5 + i) == 1);
        assert(reply.highlighted_candidate == (i == 0 ? 1U : i == 1 ? 2U : 1U));
        assert((reply.flags & C1_IME_CONSUMED) && !reply.commit[0] && sent_count == 2);
    }
    assert(c1_input_method_tick(&input, &reply, 8) == 0);
    assert(sent[2].operation == C1_IME_OP_SELECT && sent[2].value == 1);
    acknowledge(&input, &reply, 0, false, 9);
    assert(reply.request.operation == C1_IME_OP_KEY && reply.request.keysym == C1_IME_KEY_SPACE);
    assert(!c1_input_method_key(&input, C1_IME_KEY_PAGE_DOWN, 0)); /* Keep idle volume repeat. */

    assert(c1_input_method_key(&input, 'n', 0));
    assert(c1_input_method_key(&input, C1_IME_KEY_PAGE_DOWN, 0));
    for (unsigned i = 0; i < 6; ++i) assert(c1_input_method_key(&input, C1_IME_KEY_RIGHT, 0));
    assert(c1_input_method_key(&input, C1_IME_KEY_RETURN, 0));
    assert(c1_input_method_tick(&input, &reply, 10) == 0);
    acknowledge(&input, &reply, 5, true, 11);
    assert(c1_input_method_tick(&input, &reply, 12) == 0);
    assert(sent[4].operation == C1_IME_OP_PAGE && sent[4].value == C1_IME_PAGE_NEXT);
    assert(c1_input_method_tick(&input, &reply, 13) == 0); /* Delayed page response. */
    acknowledge(&input, &reply, 2, true, 14);
    assert(reply.highlighted_candidate == 0);
    for (unsigned i = 0; i < 6; ++i) {
        assert(c1_input_method_tick(&input, &reply, 15 + i) == 1);
        assert(reply.highlighted_candidate == 1); /* Clamp to short final page. */
    }
    assert(c1_input_method_tick(&input, &reply, 21) == 0);
    assert(sent[5].operation == C1_IME_OP_SELECT && sent[5].value == 1);
    acknowledge(&input, &reply, 0, false, 22);

    /* No composition: a queued arrow/page is forwarded exactly once, after
     * earlier input. A valid committed prefix is never repeated locally. */
    assert(c1_input_method_key(&input, 'X', 0));
    assert(c1_input_method_key(&input, C1_IME_KEY_LEFT, 0));
    assert(c1_input_method_key(&input, C1_IME_KEY_PAGE_UP, 0));
    assert(c1_input_method_tick(&input, &reply, 23) == 0);
    next_reply.flags = C1_IME_READY | C1_IME_CHINESE;
    strcpy(next_reply.commit, "你"); receive_result = 1;
    assert(c1_input_method_tick(&input, &reply, 24) == 1 && !strcmp(reply.commit, "你"));
    for (unsigned i = 0; i < 2; ++i) {
        assert(c1_input_method_tick(&input, &reply, 25 + i) == 1);
        assert(!(reply.flags & C1_IME_CONSUMED) && !reply.commit[0]);
    }

    assert(c1_input_method_key(&input, 'n', 0));
    assert(c1_input_method_tick(&input, &reply, 27) == 0);
    acknowledge(&input, &reply, 5, true, 28);
    assert(c1_input_method_key(&input, C1_IME_KEY_LEFT, 0));
    assert(c1_input_method_tick(&input, &reply, 29) == 1 && reply.highlighted_candidate == 0);
    assert(c1_input_method_key(&input, '5', C1_IME_MOD_SHIFT));
    assert(c1_input_method_tick(&input, &reply, 30) == 0);
    assert(sent[sent_count - 1].operation == C1_IME_OP_SELECT && sent[sent_count - 1].value == 4);
    acknowledge(&input, &reply, 0, false, 31);
    assert(reply.request.keysym == '5' && reply.request.modifiers == C1_IME_MOD_SHIFT);

    assert(c1_input_method_key(&input, 'n', 0));
    assert(c1_input_method_tick(&input, &reply, 32) == 0);
    acknowledge(&input, &reply, 0, true, 33);
    assert(c1_input_method_key(&input, '5', C1_IME_MOD_SHIFT));
    assert(c1_input_method_tick(&input, &reply, 34) == 1 && (reply.flags & C1_IME_CONSUMED));
    assert(!reply.commit[0] && !input.client.pending_sequence);
    assert(c1_input_method_key(&input, C1_IME_KEY_PAGE_UP, 0));
    assert(c1_input_method_tick(&input, &reply, 35) == 0);
    assert(sent[sent_count - 1].operation == C1_IME_OP_PAGE && sent[sent_count - 1].value == C1_IME_PAGE_PREVIOUS);
    acknowledge(&input, &reply, 5, true, 36);
    assert(c1_input_method_key(&input, C1_IME_KEY_RIGHT, 0));
    assert(c1_input_method_tick(&input, &reply, 37) == 1 && reply.highlighted_candidate == 1);
    assert(c1_input_method_key(&input, C1_IME_KEY_BACKSPACE, 0));
    assert(c1_input_method_tick(&input, &reply, 38) == 0);
    acknowledge(&input, &reply, 3, true, 39);
    assert(reply.highlighted_candidate == 0); /* Editing rebuilds the page. */
    c1_input_method_close(&input);
    assert(!input.view.candidate_count && !input.view.highlighted_candidate);

    /* Password purpose must not use even a malicious/stale candidate view. */
    reset(&input);
    assert(c1_input_method_toggle(&input, NULL, 0));
    assert(c1_input_method_tick(&input, &reply, 0) == 0);
    next_reply.flags = C1_IME_READY | C1_IME_CHINESE | C1_IME_PASSWORD | C1_IME_COMPOSING;
    next_reply.candidate_count = 5; receive_result = 1;
    assert(c1_input_method_tick(&input, &reply, 1) == 1);
    assert(c1_input_method_key(&input, C1_IME_KEY_RIGHT, 0));
    assert(c1_input_method_tick(&input, &reply, 2) == 0);
    assert(sent[1].operation == C1_IME_OP_KEY && sent[1].keysym == C1_IME_KEY_RIGHT);
    c1_input_method_close(&input);
}

int main(void)
{
    c1_input_method input;
    struct c1_ime_response reply;
    reset(&input);
    assert(!c1_input_method_key(&input, 'a', 0));
    connect_result = -1;
    assert(!c1_input_method_toggle(&input, NULL, 0));
    assert(input.failed && !input.enabled);
    reset(&input);
    assert(c1_input_method_toggle(&input, NULL, 100));
    assert(c1_input_method_key(&input, 'n', 0));
    assert(c1_input_method_key(&input, 'i', 0));
    assert(c1_input_method_tick(&input, &reply, 100) == 0);
    assert(sent_count == 1 && sent[0].operation == C1_IME_OP_MODE);
    assert(!(c1_input_method_poll_events(&input) & POLLOUT));
    receive_result = 1;
    assert(c1_input_method_tick(&input, &reply, 101) == 1);
    assert(sent_count == 1); /* Returning response never attempts a new send. */
    assert(c1_input_method_poll_events(&input) & POLLOUT);
    assert(c1_input_method_tick(&input, &reply, 102) == 0);
    assert(sent_count == 2 && sent[1].keysym == 'n');
    strcpy(next_reply.commit, "你"); receive_result = 1; send_result = -1;
    assert(c1_input_method_tick(&input, &reply, 103) == 1);
    assert(!strcmp(reply.commit, "你")); /* A later send failure cannot lose commit. */
    assert(c1_input_method_tick(&input, &reply, 104) == -1);
    assert(input.failed && input.client.fd < 0 && !input.count);
    reset(&input);
    assert(c1_input_method_toggle(&input, NULL, 0)); send_result = 0;
    assert(c1_input_method_tick(&input, &reply, 4999) == 0);
    assert(c1_input_method_tick(&input, &reply, 5000) == -1); /* EAGAIN bounded. */
    reset(&input);
    assert(c1_input_method_toggle(&input, NULL, 0));
    assert(c1_input_method_tick(&input, &reply, 1) == 0);
    assert(c1_input_method_tick(&input, &reply, 5001) == -1); /* No response bounded. */
    reset(&input);
    assert(c1_input_method_toggle(&input, NULL, 0));
    for (unsigned i = 0; i < C1_INPUT_QUEUE_SIZE + 2; ++i)
        assert(c1_input_method_key(&input, 'x', 0));
    assert(input.count == C1_INPUT_QUEUE_SIZE && input.failed); /* Never direct-forward overflow. */
    assert(!c1_input_method_toggle(&input, NULL, 1) && input.failed);
    assert(c1_input_method_tick(&input, &reply, 1) == -1 && input.client.fd < 0 && !input.count);
    c1_input_method_close(&input);
    assert(!input.count && !input.failed && !input.enabled);
    reset(&input);
    assert(c1_input_method_toggle(&input, NULL, 0));
    assert(c1_input_method_key(&input, 'n', 0));
    assert(c1_input_method_toggle(&input, NULL, 1));
    assert(!input.enabled && c1_input_method_key(&input, 'x', 0)); /* Preserve ordering across toggle. */
    for (int i = 0; i < 4; ++i) {
        assert(c1_input_method_tick(&input, &reply, 2 + i * 2) == 0);
        receive_result = 1;
        assert(c1_input_method_tick(&input, &reply, 3 + i * 2) == 1);
    }
    assert(sent_count == 4 && sent[1].keysym == 'n' && sent[2].value == C1_IME_MODE_ENGLISH && sent[3].keysym == 'x');
    assert(c1_input_method_tick(&input, &reply, 11) == 0 && input.client.fd < 0);
    candidate_interaction();
    puts("desktop input adapter tests passed: ordered highlight/page/commit, password bypass, timeout, overflow");
    return 0;
}
