#define _GNU_SOURCE
#include "c1_ime_client.h"
#include "protocol.h"

/* Tests must execute their checks and calls in Release builds too. */
#undef NDEBUG
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static struct c1_ime_request op(uint32_t operation, uint32_t value) {
    struct c1_ime_request request = { operation, 0, 0, value };
    return request;
}

static struct c1_ime_response request(struct c1_ime_client *client, struct c1_ime_request req) {
    struct c1_ime_response response;
    struct pollfd fd;
    int result;
    memset(&response, 0, sizeof(response));
    assert(c1_ime_send(client, &req) == 1);
    assert(c1_ime_send(client, &req) == -1 && errno == EBUSY);
    fd.fd = c1_ime_client_fd(client); fd.events = POLLIN; fd.revents = 0;
    do { result = poll(&fd, 1, 30000); } while (result < 0 && errno == EINTR);
    assert(result == 1 && (fd.revents & POLLIN));
    assert(c1_ime_receive(client, &response) == 1);
    assert(response.flags & C1_IME_READY);
    assert(response.request.operation == req.operation);
    assert(response.request.keysym == req.keysym);
    assert(response.request.modifiers == req.modifiers);
    assert(response.request.value == req.value);
    assert(response.candidate_count <= C1_IME_MAX_CANDIDATES);
    return response;
}

static struct c1_ime_response key(struct c1_ime_client *client, uint32_t keysym) {
    struct c1_ime_request req = op(C1_IME_OP_KEY, 0);
    req.keysym = keysym;
    return request(client, req);
}

/* Synthetic peer tests exercise the decoder without loading C++ or librime. */
static void sdk_unit_tests(void) {
    struct c1_ime_client client = C1_IME_CLIENT_INIT;
    struct c1_ime_response response;
    struct c1_ime_request req = op(C1_IME_OP_KEY, 0);
    unsigned char data[C1_IME_MAX_PACKET + 1];
    int pair[2];
    unsigned trial;
    assert(c1_ime_connect(&client, "relative") < 0 && errno == EINVAL);
    assert(c1_ime_connect(&client, "/socket") < 0 && errno == EINVAL);
    assert(c1_ime_client_fd(&client) == -1);
    c1_ime_close(&client); c1_ime_close(&client);
    req.keysym = 'x'; req.modifiers = C1_IME_MOD_CONTROL;
    for (trial = 0; trial < 9; ++trial) {
        assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair) == 0);
        client.fd = pair[0];
        assert(c1_ime_receive(&client, &response) == 0);
        assert(c1_ime_send(&client, &req) == 1);
        assert(recv(pair[1], data, sizeof(data), 0) == C1_WIRE_HEADER);
        assert(c1_get32(data + 16) == req.keysym && c1_get32(data + 20) == req.modifiers);
        assert(client.pending_sequence == 1);
        memset(data, 0, sizeof(data));
        c1_put32(data, C1_WIRE_MAGIC); c1_put16(data + 4, C1_IME_PROTOCOL_VERSION);
        c1_put16(data + 6, C1_IME_OP_KEY | C1_WIRE_REPLY);
        c1_put32(data + 8, 1); c1_put32(data + 12, C1_WIRE_REPLY_HEADER);
        c1_put32(data + 16, req.keysym); c1_put32(data + 20, C1_IME_READY);
        if (trial == 1) c1_put32(data + 8, 2); /* wrong sequence */
        if (trial == 2) c1_put32(data + 16, 'y'); /* wrong original keysym */
        if (trial == 3) { /* invalid UTF-8 */
            c1_put32(data + 12, C1_WIRE_REPLY_HEADER + 1);
            c1_put16(data + 32, 1); data[C1_WIRE_REPLY_HEADER] = 0xff;
        }
        if (trial == 4) c1_put16(data + 36, 6); /* too many candidates */
        if (trial == 5) c1_put32(data + 12, sizeof(data)); /* MSG_TRUNC */
        if (trial == 7) c1_put32(data + 28, 1); /* Local highlight is NOT a wire extension. */
        if (trial == 8) c1_put16(data + 38, 1); /* Both reserved fields retain v1 validation. */
        if (trial == 6) {
            close(pair[1]);
            assert(c1_ime_receive(&client, &response) == -1 && errno == ECONNRESET);
        } else {
            size_t length = trial == 5 ? sizeof(data) : (trial == 3 ? C1_WIRE_REPLY_HEADER + 1 : C1_WIRE_REPLY_HEADER);
            assert(send(pair[1], data, length, 0) == (ssize_t)length);
            if (trial == 0) {
                assert(c1_ime_receive(&client, &response) == 1);
                assert(!(response.flags & C1_IME_CONSUMED));
                assert(response.highlighted_candidate == 0); /* v1 reserved bytes remain zero. */
                assert(response.request.keysym == 'x' && response.request.modifiers == C1_IME_MOD_CONTROL);
                assert(client.pending_sequence == 0);
            } else {
                assert(c1_ime_receive(&client, &response) == -1 && errno == EPROTO);
                assert(client.fd == -1 && client.pending_sequence == 0);
                assert(c1_ime_send(&client, &req) == -1 && errno == ENOTCONN);
            }
            close(pair[1]);
        }
        c1_ime_close(&client);
    }
}

int main(void) {
    const char *socket_path = getenv("C1_IME_TEST_SOCKET");
    struct c1_ime_client client = C1_IME_CLIENT_INIT;
    struct c1_ime_response response;
    struct c1_ime_request req;
    unsigned i;
    sdk_unit_tests();
    if (socket_path == NULL) return 0;
    assert(c1_ime_connect(&client, socket_path) == 0);
    assert(fcntl(client.fd, F_GETFD) & FD_CLOEXEC);
    assert(fcntl(client.fd, F_GETFL) & O_NONBLOCK);
    response = request(&client, op(C1_IME_OP_STATUS, 0));
    assert(response.status == C1_IME_STATUS_OK && !(response.flags & C1_IME_CHINESE));
    response = request(&client, op(C1_IME_OP_MODE, 1));
    assert((response.flags & C1_IME_CHINESE) && !(response.flags & C1_IME_COMPOSING));
    for (i = 0; i < 5; ++i) {
        response = key(&client, (uint32_t)"nihao"[i]);
        assert(response.flags & C1_IME_CONSUMED);
    }
    assert(response.candidate_count > 0 && strcmp(response.candidates[0], "你好") == 0);
    response = key(&client, C1_IME_KEY_SPACE);
    assert((response.flags & C1_IME_CONSUMED) && strcmp(response.commit, "你好") == 0);
    response = key(&client, 'n');
    assert(response.flags & C1_IME_COMPOSING);
    response = request(&client, op(C1_IME_OP_MODE, 0));
    assert(!(response.flags & (C1_IME_CHINESE | C1_IME_COMPOSING)) && !response.preedit[0] && !response.commit[0]);
    req = op(C1_IME_OP_KEY, 0); req.keysym = 'x'; req.modifiers = C1_IME_MOD_CONTROL;
    response = request(&client, req);
    assert(!(response.flags & C1_IME_CONSUMED) && !response.commit[0]);
    response = request(&client, op(C1_IME_OP_MODE, 1));
    response = key(&client, 'n');
    response = request(&client, op(C1_IME_OP_PURPOSE, C1_IME_PURPOSE_PASSWORD));
    assert((response.flags & C1_IME_PASSWORD) && !response.preedit[0]);
    /* Password bypass while Chinese remains ON, not merely English fallback. */
    for (i = 0; i < 6; ++i) {
        response = key(&client, (uint32_t)"nihao "[i]);
        assert((response.flags & C1_IME_CHINESE) && (response.flags & C1_IME_PASSWORD));
        assert(!(response.flags & (C1_IME_CONSUMED | C1_IME_COMPOSING)));
        assert(!response.commit[0] && !response.preedit[0] && !response.candidate_count);
    }
    response = request(&client, op(C1_IME_OP_PURPOSE, C1_IME_PURPOSE_NORMAL));
    assert(!response.preedit[0] && !response.commit[0] && !response.candidate_count);
    c1_ime_close(&client);
    puts("C17 SDK real-Rime nihao/commit, cancellation and password bypass passed");
    return 0;
}
