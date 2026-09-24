/* Pure C17 example: a finite demonstration, NOT a desktop event loop.
 * The SDK itself never polls or waits. A desktop should register client_fd in
 * its existing event loop and dispatch receive only when readable.
 */
#include "c1_ime_client.h"
#include <errno.h>
#include <poll.h>
#include <stdio.h>

static int exchange(struct c1_ime_client *client, struct c1_ime_request request,
                    struct c1_ime_response *response) {
    int result;
    struct pollfd fd = { c1_ime_client_fd(client), POLLOUT, 0 };
    for (;;) {
        result = c1_ime_send(client, &request);
        if (result == 1) break;
        if (result < 0) return -1;
        do { result = poll(&fd, 1, 30000); } while (result < 0 && errno == EINTR);
        if (result <= 0 || (fd.revents & (POLLERR | POLLHUP | POLLNVAL))) return -1;
    }
    fd.events = POLLIN;
    for (;;) {
        do { result = poll(&fd, 1, 30000); } while (result < 0 && errno == EINTR);
        if (result <= 0) return -1;
        /* Drain a possible final response even if POLLHUP is also reported. */
        if (fd.revents & POLLIN) {
            result = c1_ime_receive(client, response);
            if (result == 1) return 0;
            if (result < 0) return -1;
        }
        if (fd.revents & (POLLERR | POLLHUP | POLLNVAL)) return -1;
    }
}

int main(int argc, char **argv) {
    struct c1_ime_client client = C1_IME_CLIENT_INIT;
    struct c1_ime_response response;
    struct c1_ime_request request = { C1_IME_OP_STATUS, 0, 0, 0 };
    const char text[] = "nihao ";
    unsigned i;
    if (argc > 2) { fprintf(stderr, "usage: %s [/absolute/socket]\n", argv[0]); return 2; }
    if (c1_ime_connect(&client, argc == 2 ? argv[1] : NULL) < 0) {
        perror("IME connect; caller should use ASCII fallback for future keys"); return 1;
    }
    if (exchange(&client, request, &response) < 0 || !(response.flags & C1_IME_READY)) goto failed;
    request.operation = C1_IME_OP_MODE; request.value = C1_IME_MODE_CHINESE;
    if (exchange(&client, request, &response) < 0) goto failed;
    request.operation = C1_IME_OP_KEY; request.value = 0;
    for (i = 0; text[i]; ++i) {
        request.keysym = (unsigned char)text[i];
        if (exchange(&client, request, &response) < 0) goto failed;
        if (response.commit[0]) printf("commit: %s\n", response.commit);
        if (response.preedit[0]) printf("preedit: %s\n", response.preedit);
        if (!(response.flags & C1_IME_CONSUMED))
            printf("forward original keysym=%u modifiers=%u\n", response.request.keysym, response.request.modifiers);
    }
    c1_ime_close(&client);
    return 0;
failed:
    fprintf(stderr, "IME response failed; discard preedit, never replay a pending key\n");
    c1_ime_close(&client);
    return 1;
}
