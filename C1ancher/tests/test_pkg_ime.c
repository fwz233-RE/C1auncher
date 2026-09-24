/* Hardware-free IME regression: production GUI, adapter and C SDK, with a
 * SOCK_SEQPACKET mock service. No Rime, device, deployment or network access.
 * Reuse only the storage/repository/exec fixture from the workflow harness. */
#define main pkg_gui_workflow_main
#include "test_pkg_gui.c"
#undef main
#include "protocol.h"
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

static const struct c1pkg_config ime_config = {"http://example.invalid", "/unused"};
static uint64_t ime_now = 100U;
static int ime_peer = -1;

static void ime_fixture(struct gui_state *s)
{
    gui_ime_close(s);
    if (ime_peer >= 0) close(ime_peer);
    ime_peer = -1;
    reset_gui(s);
    local.count = fixture.count;
    for (size_t i = 0U; i < local.count; ++i) {
        strcpy(local.items[i].id, fixture.packages[i].id);
        strcpy(local.items[i].version, "1.2.0");
    }
    s->index = fixture;
    reload_local(s);
    launch_calls = 0;
    ime_now = 100U;
}

static void ime_attach(struct gui_state *s)
{
    char directory[] = "/tmp/c1pkg-ime-XXXXXX";
    struct sockaddr_un address = {0};
    int listener;
    if (mkdtemp(directory) == NULL) { expect(0, "create private socket directory"); exit(2); }
    expect(chmod(directory, 0700) == 0, "socket parent has required private mode");
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s/socket", directory);
    listener = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listener < 0 || bind(listener, (struct sockaddr *)&address, sizeof(address)) ||
        chmod(address.sun_path, 0600) || listen(listener, 1)) {
        expect(0, "create mock service socket"); exit(2);
    }
    gui_ime_toggle(s, address.sun_path, ++ime_now);
    ime_peer = accept(listener, NULL, NULL);
    expect(ime_peer >= 0 && s->input_method.client.fd >= 0,
           "production SDK connects to private same-UID SOCK_SEQPACKET peer");
    if (ime_peer >= 0) expect(fcntl(ime_peer, F_SETFL, O_NONBLOCK) == 0, "mock peer is nonblocking");
    close(listener);
    expect(unlink(address.sun_path) == 0 && rmdir(directory) == 0, "temporary socket namespace cleaned");
}

static struct c1_ime_request ime_receive_request(struct gui_state *s, uint32_t *sequence)
{
    unsigned char packet[C1_WIRE_HEADER];
    struct c1_ime_request request = {0};
    ssize_t length;
    (void)gui_ime_pump(s, &ime_config, ++ime_now);
    length = recv(ime_peer, packet, sizeof(packet), MSG_DONTWAIT);
    expect(length == (ssize_t)sizeof(packet) &&
           c1_decode_request(packet, length > 0 ? (size_t)length : 0U, &request, sequence),
           "GUI sends one valid SDK request without blocking");
    return request;
}

static void ime_send_candidates(const struct c1_ime_request *request, uint32_t sequence,
                                uint32_t flags, uint32_t status, const char *commit,
                                const char *preedit, const char *const *candidates, unsigned count)
{
    unsigned char packet[C1_IME_MAX_PACKET] = {0};
    size_t position = C1_WIRE_REPLY_HEADER;
    size_t commit_size = strlen(commit), preedit_size = strlen(preedit);
    c1_put32(packet, C1_WIRE_MAGIC);
    c1_put16(packet + 4, C1_IME_PROTOCOL_VERSION);
    c1_put16(packet + 6, (uint16_t)(request->operation | C1_WIRE_REPLY));
    c1_put32(packet + 8, sequence);
    c1_put32(packet + 16, request->keysym);
    c1_put32(packet + 20, flags);
    c1_put32(packet + 24, status);
    c1_put16(packet + 32, (uint16_t)preedit_size);
    c1_put16(packet + 34, (uint16_t)commit_size);
    c1_put16(packet + 36, (uint16_t)count);
    memcpy(packet + position, preedit, preedit_size); position += preedit_size;
    memcpy(packet + position, commit, commit_size); position += commit_size;
    for (unsigned i = 0; i < count; ++i) {
        size_t size = strlen(candidates[i]);
        c1_put16(packet + position, (uint16_t)size); position += 2U;
        memcpy(packet + position, candidates[i], size); position += size;
    }
    c1_put32(packet + 12, (uint32_t)position);
    expect(send(ime_peer, packet, position, MSG_DONTWAIT | MSG_NOSIGNAL) == (ssize_t)position,
           "mock service sends a complete framed response");
}

static void ime_send_response(const struct c1_ime_request *request, uint32_t sequence,
                              uint32_t flags, uint32_t status, const char *commit,
                              const char *preedit, const char *candidate)
{
    ime_send_candidates(request, sequence, flags, status, commit, preedit, &candidate, candidate ? 1U : 0U);
}

static void ime_reply(struct gui_state *s, const struct c1_ime_request *request,
                      uint32_t sequence, uint32_t flags, const char *commit,
                      const char *preedit, const char *candidate)
{
    ime_send_response(request, sequence, flags, C1_IME_STATUS_OK, commit, preedit, candidate);
    expect(!gui_ime_pump(s, &ime_config, ++ime_now), "IME response stays on list");
}

static void ime_enable(struct gui_state *s)
{
    uint32_t sequence = 0U;
    struct c1_ime_request request;
    ime_attach(s);
    request = ime_receive_request(s, &sequence);
    expect(request.operation == C1_IME_OP_STATUS, "STATUS precedes mode and user keys after real connect");
    ime_reply(s, &request, sequence, C1_IME_READY, "", "", NULL);
    request = ime_receive_request(s, &sequence);
    expect(request.operation == C1_IME_OP_MODE && request.value == C1_IME_MODE_CHINESE,
           "Shift+Space enables public Chinese mode");
    ime_reply(s, &request, sequence, C1_IME_READY | C1_IME_CHINESE, "", "", NULL);
    expect(s->ime_ready && gui_rows(s) == 2U, "ready IME reserves two list rows");
}

static void test_chinese_commit(struct gui_state *s)
{
    uint32_t sequence = 0U;
    struct c1_ime_request request;
    ime_fixture(s); ime_enable(s);
    (void)gui_ime_input(s, &ime_config, 'd', ++ime_now);
    request = ime_receive_request(s, &sequence);
    ime_reply(s, &request, sequence, C1_IME_READY | C1_IME_CHINESE | C1_IME_CONSUMED | C1_IME_COMPOSING,
              "", "dian", "电子");
    expect(!s->query[0] && !strcmp(s->ime_view.preedit, "dian") &&
           s->ime_view.candidate_count == 1U, "preedit and candidates never become committed prefix");
    strcpy(s->query, "电");
    (void)gui_list_key(s, &ime_config, G_NONE, 50000U);
    expect(!strcmp(s->query, "电"), "search is persistent during composition and idle");
    (void)gui_ime_input(s, &ime_config, G_REFRESH, ++ime_now);
    request = ime_receive_request(s, &sequence);
    expect(request.operation == C1_IME_OP_SELECT && request.value == 0U && !refresh_calls,
           "Space selects the acknowledged highlight before any refresh");
    ime_reply(s, &request, sequence, C1_IME_READY | C1_IME_CHINESE | C1_IME_CONSUMED,
              "子书", "", NULL);
    expect(!strcmp(s->query, "电子书") && s->selected[0] == 0U && s->count == 1U && !refresh_calls && !launch_calls,
           "complete UTF-8 commit appends once and selects Chinese application name");
    (void)gui_ime_pump(s, &ime_config, ++ime_now);
    expect(!strcmp(s->query, "电子书"), "repeated idle tick cannot duplicate a commit");
    (void)gui_ime_input(s, &ime_config, C1PKG_KEY_ERASE, ++ime_now);
    request = ime_receive_request(s, &sequence);
    ime_reply(s, &request, sequence, C1_IME_READY | C1_IME_CHINESE, "", "", NULL);
    expect(!strcmp(s->query, "电子") && c1_valid_utf8((unsigned char *)s->query, strlen(s->query)),
           "unconsumed Backspace deletes one complete Chinese codepoint");
    gui_ime_close(s);
    expect(s->input_method.client.fd == -1 && !s->ime_view.preedit[0] && !s->ime_view.candidate_count,
           "focus close discards composition and disconnects");
}

static void test_candidate_pages(struct gui_state *s)
{
    const char *first[] = {"你", "呢", "泥", "拟", "逆"};
    const char *last[] = {"电子", "电子书"};
    uint32_t sequence;
    const uint32_t composing = C1_IME_READY | C1_IME_CHINESE | C1_IME_COMPOSING | C1_IME_CONSUMED;
    struct c1_ime_request request;
    ime_fixture(s); ime_enable(s);
    /* This entire burst precedes the first composition response. */
    const int burst[] = {'n', G_RIGHT, G_PAGE_DOWN, G_RIGHT, G_RIGHT, G_ENTER};
    for (size_t i = 0; i < sizeof(burst) / sizeof(burst[0]); ++i)
        (void)gui_ime_input(s, &ime_config, burst[i], ++ime_now);
    request = ime_receive_request(s, &sequence);
    ime_send_candidates(&request, sequence, composing, C1_IME_STATUS_OK, "", "n", first, 5);
    (void)gui_ime_pump(s, &ime_config, ++ime_now);
    (void)gui_ime_pump(s, &ime_config, ++ime_now);
    expect(s->ime_view.highlighted_candidate == 1U && !strcmp(s->ime_view.candidates[1], "呢"),
           "right highlights second candidate only after the composition response");
    expect(s->tab == 0U && !launch_calls && !s->query[0], "candidate arrow cannot change tabs or search");
    render_gui(s);
    expect((s->frame[(112U / 8U) * GUI_WIDTH + 62U] & 0x80U) &&
           !(s->frame[(112U / 8U) * GUI_WIDTH + 5U] & 0x80U),
           "rendered candidate highlight follows second slot rather than always painting first");
    request = ime_receive_request(s, &sequence);
    expect(request.operation == C1_IME_OP_PAGE && request.value == C1_IME_PAGE_NEXT,
           "volume plus remains distinct from Right and sends old-v1 next-page operation");
    (void)gui_ime_pump(s, &ime_config, ++ime_now);
    expect(s->ime_view.highlighted_candidate == 1U && !s->query[0], "delayed page does not execute queued selection");
    ime_send_candidates(&request, sequence, composing, C1_IME_STATUS_OK, "", "n", last, 2);
    (void)gui_ime_pump(s, &ime_config, ++ime_now);
    expect(s->ime_view.highlighted_candidate == 0U, "new page resets highlight to first visible candidate");
    (void)gui_ime_pump(s, &ime_config, ++ime_now);
    (void)gui_ime_pump(s, &ime_config, ++ime_now);
    expect(s->ime_view.highlighted_candidate == 1U, "right clamps to short last-page candidate count");
    request = ime_receive_request(s, &sequence);
    expect(request.operation == C1_IME_OP_SELECT && request.value == 1U,
           "queued Enter selects the new acknowledged page, not an old candidate");
    ime_reply(s, &request, sequence, C1_IME_READY | C1_IME_CHINESE | C1_IME_CONSUMED, last[1], "", NULL);
    expect(!strcmp(s->query, "电子书") && s->search_editing && !launch_calls && !refresh_calls,
           "Enter commits highlighted Chinese name without launching or filtering focus prematurely");
    (void)gui_ime_pump(s, &ime_config, ++ime_now);
    expect(!strcmp(s->query, "电子书"), "highlight/select responses never duplicate the commit");
    /* Non-composing volume and arrows retain tab navigation. */
    (void)gui_ime_input(s, &ime_config, G_PAGE_DOWN, ++ime_now);
    expect(s->tab == 1U, "non-composing volume plus keeps store-tab navigation");
    (void)gui_ime_input(s, &ime_config, G_LEFT, ++ime_now);
    (void)gui_ime_pump(s, &ime_config, ++ime_now);
    expect(s->tab == 0U, "non-composing joystick left still navigates normally");

    (void)gui_ime_input(s, &ime_config, 'n', ++ime_now);
    (void)gui_ime_input(s, &ime_config, G_PAGE_UP, ++ime_now);
    (void)gui_ime_input(s, &ime_config, '5', ++ime_now);
    request = ime_receive_request(s, &sequence);
    ime_send_candidates(&request, sequence, composing, C1_IME_STATUS_OK, "", "n", last, 2);
    (void)gui_ime_pump(s, &ime_config, ++ime_now);
    request = ime_receive_request(s, &sequence);
    expect(request.operation == C1_IME_OP_PAGE && request.value == C1_IME_PAGE_PREVIOUS,
           "volume minus uses the previous-page operation");
    ime_send_candidates(&request, sequence, composing, C1_IME_STATUS_OK, "", "n", first, 5);
    (void)gui_ime_pump(s, &ime_config, ++ime_now);
    request = ime_receive_request(s, &sequence);
    expect(request.operation == C1_IME_OP_SELECT && request.value == 4U,
           "numeric keycap directly selects slot five after previous-page completion");
    ime_reply(s, &request, sequence, C1_IME_READY | C1_IME_CHINESE | C1_IME_CONSUMED, "逆", "", NULL);
    expect(!strcmp(s->query, "电子书逆") && !launch_calls, "numeric direct choice appends once");
}

static void test_unconsumed_order(struct gui_state *s)
{
    static const int keys[] = {'A', 'P', 'P', '1', G_DOWN};
    uint32_t sequence = 0U;
    struct c1_ime_request request;
    ime_fixture(s); ime_enable(s);
    for (size_t i = 0U; i < sizeof(keys) / sizeof(keys[0]); ++i)
        (void)gui_ime_input(s, &ime_config, keys[i], ++ime_now);
    expect(s->query[0] == '\0' && s->selected[0] == 0U,
           "queued keys cannot run original commands before acknowledgement");
    for (size_t i = 0U; i < 4U; ++i) {
        request = ime_receive_request(s, &sequence);
        expect(request.keysym == (uint32_t)keys[i], "rapid ASCII keys preserve wire order");
        ime_reply(s, &request, sequence, C1_IME_READY | C1_IME_CHINESE, "", "", NULL);
        if (i == 3U) expect(!strcmp(s->query, "app1") && s->selected[0] == 0U && s->count == 1U,
                           "unconsumed ASCII falls back to case-insensitive exact ID selection");
    }
    (void)gui_ime_pump(s, &ime_config, ++ime_now); /* Ordered local unconsumed arrow. */
    expect(s->selected[0] == 0U && s->count == 1U && !strcmp(s->query, "app1") && !refresh_calls && !launch_calls,
           "unconsumed navigation follows earlier text once and preserves its filter");
}

static void test_transport_failure(struct gui_state *s)
{
    uint32_t sequence = 0U;
    struct c1_ime_request request;
    ime_fixture(s); ime_enable(s);
    (void)gui_ime_input(s, &ime_config, G_ENTER, ++ime_now);
    (void)gui_ime_input(s, &ime_config, G_REFRESH, ++ime_now);
    request = ime_receive_request(s, &sequence); (void)request;
    close(ime_peer); ime_peer = -1;
    (void)gui_ime_pump(s, &ime_config, ++ime_now);
    expect(s->input_method.client.fd == -1 && !s->input_method.count && s->ime_notice &&
           !launch_calls && !refresh_calls && !install_calls,
           "disconnect exposes warning and never replays ambiguous Enter or queued Space");
    refresh_gui(s, &ime_config); finish_refresh(s);
    draw_count = 0; render_gui(s);
    expect(s->verified && s->ime_notice && strcmp(s->status, s->ime_error),
           "successful asynchronous refresh cannot overwrite the still-unacknowledged input error");
    expect_footer(s, GUI_LABEL(s, "输入法失败 ASCII输入", "IME failed; ASCII input"));
    (void)gui_ime_input(s, &ime_config, 'a', ++ime_now);
    expect(!strcmp(s->query, "a"), "fresh ASCII remains usable after a transport failure");

    ime_fixture(s); ime_enable(s);
    (void)gui_ime_input(s, &ime_config, G_ENTER, ++ime_now);
    request = ime_receive_request(s, &sequence);
    (void)gui_ime_pump(s, &ime_config, ime_now + 5001U);
    expect(s->input_method.client.fd == -1 && s->ime_notice && !launch_calls,
           "deadline expiration fails closed rather than forwarding a timed-out command");

    ime_fixture(s); ime_enable(s);
    for (unsigned int i = 0U; i < C1_INPUT_QUEUE_SIZE + 1U; ++i)
        (void)gui_ime_input(s, &ime_config, G_ENTER, ++ime_now);
    expect(s->input_method.client.fd == -1 && s->ime_notice && !launch_calls,
           "queue saturation is visibly failed closed without launching an application");
}

static void test_toggle_and_rejected_responses(struct gui_state *s)
{
    uint32_t sequence = 0U;
    struct c1_ime_request request;
    static const char *suffix = "pp1";
    ime_fixture(s); ime_enable(s);
    (void)gui_ime_input(s, &ime_config, 'a', ++ime_now);
    (void)gui_ime_input(s, &ime_config, G_IME_TOGGLE, ++ime_now);
    for (size_t i = 0U; suffix[i]; ++i) (void)gui_ime_input(s, &ime_config, suffix[i], ++ime_now);
    request = ime_receive_request(s, &sequence);
    expect(request.operation == C1_IME_OP_KEY && request.keysym == 'a', "key before toggle remains first");
    ime_reply(s, &request, sequence, C1_IME_READY | C1_IME_CHINESE, "", "", NULL);
    request = ime_receive_request(s, &sequence);
    expect(request.operation == C1_IME_OP_MODE && request.value == C1_IME_MODE_ENGLISH,
           "disable mode is queued between the surrounding keys");
    ime_reply(s, &request, sequence, C1_IME_READY, "", "", NULL);
    for (size_t i = 0U; suffix[i]; ++i) {
        request = ime_receive_request(s, &sequence);
        expect(request.operation == C1_IME_OP_KEY && request.keysym == (uint32_t)suffix[i],
               "post-toggle keys stay queued until prior replies are delivered");
        ime_reply(s, &request, sequence, C1_IME_READY, "", "", NULL);
    }
    (void)gui_ime_pump(s, &ime_config, ++ime_now);
    expect(!strcmp(s->query, "app1") && s->selected[0] == 0U && s->count == 1U && s->input_method.client.fd == -1,
           "English transition drains ordered fallbacks then releases connection");

    ime_fixture(s); ime_attach(s);
    (void)gui_ime_input(s, &ime_config, G_ENTER, ++ime_now);
    request = ime_receive_request(s, &sequence);
    ime_send_response(&request, sequence, 0U, C1_IME_STATUS_OK, "", "", NULL);
    (void)gui_ime_pump(s, &ime_config, ++ime_now);
    expect(s->ime_notice && s->input_method.client.fd == -1 && !launch_calls,
           "not-ready STATUS drops queued command without sending MODE or key");

    ime_fixture(s); ime_enable(s);
    (void)gui_ime_input(s, &ime_config, G_ENTER, ++ime_now);
    request = ime_receive_request(s, &sequence);
    ime_send_response(&request, sequence, C1_IME_READY, C1_IME_STATUS_LIMIT, "", "", NULL);
    (void)gui_ime_pump(s, &ime_config, ++ime_now);
    expect(s->ime_notice && s->input_method.client.fd == -1 && !launch_calls,
           "rejected key response cannot become an original GUI command");

    ime_fixture(s); ime_enable(s);
    (void)gui_ime_input(s, &ime_config, G_ENTER, ++ime_now);
    request = ime_receive_request(s, &sequence);
    ime_send_response(&request, sequence + 1U, C1_IME_READY, C1_IME_STATUS_OK, "", "", NULL);
    (void)gui_ime_pump(s, &ime_config, ++ime_now);
    expect(s->ime_notice && s->input_method.client.fd == -1 && !launch_calls,
           "SDK rejects wrong-sequence response without replaying Enter");

    ime_fixture(s); ime_enable(s);
    (void)gui_ime_input(s, &ime_config, G_ENTER, ++ime_now);
    request = ime_receive_request(s, &sequence);
    ime_send_response(&request, sequence, C1_IME_READY | C1_IME_TRUNCATED, C1_IME_STATUS_OK, "中", "", NULL);
    (void)gui_ime_pump(s, &ime_config, ++ime_now);
    expect(s->ime_notice && s->input_method.client.fd == -1 && !s->query[0] && !launch_calls,
           "truncated commit is not accepted as a different search or replayed Enter");

    ime_fixture(s); ime_enable(s);
    memset(s->query, 'x', C1PKG_NAME_MAX); s->query[C1PKG_NAME_MAX] = '\0';
    (void)gui_ime_input(s, &ime_config, G_ENTER, ++ime_now);
    (void)gui_ime_input(s, &ime_config, G_ENTER, ++ime_now);
    request = ime_receive_request(s, &sequence);
    ime_reply(s, &request, sequence, C1_IME_READY | C1_IME_CHINESE, "中", "", NULL);
    expect(s->ime_notice && s->input_method.client.fd == -1 && !s->input_method.count &&
           strlen(s->query) == C1PKG_NAME_MAX && !launch_calls,
           "overflow commit drops both its unconsumed Enter and later queued launch");
}

static void test_lifecycle(struct gui_state *s, int input_writer)
{
    uint32_t sequence = 0U;
    struct c1_ime_request request;
    ime_fixture(s); ime_enable(s);
    begin_operation(s);
    expect(s->input_method.client.fd == -1 && !s->input_method.enabled,
           "refresh/download operation releases IME before taking non-list focus");
    end_operation(s);

    ime_fixture(s); ime_enable(s);
    (void)gui_ime_input(s, &ime_config, G_ENTER, ++ime_now);
    request = ime_receive_request(s, &sequence);
    ime_send_response(&request, sequence, C1_IME_READY | C1_IME_CHINESE, C1_IME_STATUS_OK, "", "", NULL);
    (void)gui_ime_pump(s, &ime_config, ++ime_now);
    expect(!launch_calls && !s->search_editing && s->input_method.client.fd == -1,
           "unconsumed search Enter applies filter and closes IME without launching");
    (void)gui_ime_input(s, &ime_config, G_ENTER, ++ime_now);
    expect(launch_calls == 1, "second explicit Enter launches the filtered application");

    ime_fixture(s); ime_enable(s); s->tab = 0U; select_visible(s);
    expect(write(input_writer, "q", 1U) == 1, "enqueue detail cancel");
    manage_gui(s, &ime_config, 1);
    expect(s->input_method.client.fd == -1 && !launch_calls && !install_calls,
           "detail focus closes IME and keeps safe cancel default");

    ime_fixture(s); ime_enable(s);
    expect(gui_ime_input(s, &ime_config, G_HOME, ++ime_now) && s->input_method.client.fd == -1,
           "Home leaves list and disconnects immediately");
}

static void test_commit_bounds(struct gui_state *s)
{
    char full[C1PKG_NAME_MAX + 1U];
    ime_fixture(s);
    expect(gui_append_commit(s, "电子", ++ime_now) && s->selected[0] == 0U && s->count == 1U,
           "Chinese application-name prefix selects without changing metadata");
    expect(gui_append_commit(s, "书", ++ime_now) && !strcmp(s->query, "电子书"),
           "separate commits append rather than replace the search");
    expect(!gui_append_commit(s, "\xe4\xb8", ++ime_now) && !strcmp(s->query, "电子书"),
           "incomplete UTF-8 commit is rejected atomically");
    expect(!gui_append_commit(s, "\xed\xa0\x80", ++ime_now) && !strcmp(s->query, "电子书"),
           "surrogate UTF-8 cannot enter the prefix");
    expect(!gui_append_commit(s, "\xc0\xaf", ++ime_now) && !strcmp(s->query, "电子书"),
           "overlong UTF-8 cannot enter the prefix");
    expect(!gui_append_commit(s, "\n", ++ime_now) && !strcmp(s->query, "电子书"),
           "commit controls cannot act as package commands");
    memset(full, 'x', C1PKG_NAME_MAX); full[C1PKG_NAME_MAX] = '\0';
    s->query[0] = '\0';
    expect(gui_append_commit(s, full, ++ime_now), "full byte-budget prefix is accepted");
    expect(!gui_append_commit(s, "中", ++ime_now) && !strcmp(s->query, full),
           "overflow keeps old prefix and never truncates a Chinese commit");
    s->query[0] = '\0';
    expect(gui_append_commit(s, "APP1", ++ime_now) && !strcmp(s->query, "app1") && s->selected[0] == 0U && s->count == 1U,
           "ASCII commits retain case-insensitive ID selection");
}

static void test_keyboard(int input_writer)
{
    static const char burst[] = "\033[32;2uA\033[27;2;32~\033[A\033[D \177\025\r";
    static const int expected[] = {G_IME_TOGGLE, 'A', G_IME_TOGGLE, G_UP, G_LEFT,
                                   G_REFRESH, C1PKG_KEY_ERASE, C1PKG_KEY_CLEAR, G_ENTER};
    interrupted = 0;
    memset(&gui_keyboard, 0, sizeof(gui_keyboard));
    expect(write(input_writer, burst, sizeof(burst) - 1U) == (ssize_t)sizeof(burst) - 1,
           "enqueue modifier/ASCII/navigation burst");
    for (size_t i = 0U; i < sizeof(expected) / sizeof(expected[0]); ++i)
        expect(gui_key(0) == expected[i], "incremental PTY decoder preserves every key in burst order");
    expect(write(input_writer, "\033[32;", 5U) == 5, "enqueue fragmented Shift+Space prefix");
    expect(gui_key(0) == G_NONE, "fragmented key never blocks awaiting suffix");
    expect(write(input_writer, "2u", 2U) == 2 && gui_key(0) == G_IME_TOGGLE,
           "fragmented CSI-u suffix completes one toggle");
    expect(write(input_writer, "\033[32;", 5U) == 5 && gui_key(0) == G_NONE,
           "start delayed redraw fault injection with a partial modifier sequence");
    gui_keyboard.deadline = 0U; /* UI was stalled beyond 40ms, not the PTY producer. */
    expect(write(input_writer, "2u\033[5~\033[6~A", 11U) == 11,
           "suffix, both volume page keys and text are already readable after redraw stall");
    expect(gui_key(0) == G_IME_TOGGLE && gui_key(0) == G_PAGE_UP && gui_key(0) == G_PAGE_DOWN && gui_key(0) == 'A',
           "readable suffix wins over elapsed deadline without leaking numeric commands");
    expect(write(input_writer, "\033", 1U) == 1 && gui_key(0) == G_NONE,
           "begin Escape before suffix is queued");
    gui_keyboard.deadline = 0U;
    expect(write(input_writer, "[C", 2U) == 2 && gui_key(0) == G_RIGHT,
           "readable arrow suffix cannot become Back merely because the UI was delayed");
    for (unsigned int number = 1U; number <= 5U; ++number) {
        char csi[24], xterm[24];
        snprintf(csi, sizeof(csi), "\033[%u;2u", '0' + number);
        snprintf(xterm, sizeof(xterm), "\033[27;2;%u~", '0' + number);
        expect(write(input_writer, csi, strlen(csi)) == (ssize_t)strlen(csi) && gui_key(0) == (int)('0' + number),
               "CSI-u Shift numeric cap preserves direct selection");
        expect(write(input_writer, xterm, strlen(xterm)) == (ssize_t)strlen(xterm) && gui_key(0) == (int)('0' + number),
               "xterm Shift numeric cap preserves direct selection");
    }
    expect(write(input_writer, "\033", 1U) == 1 && gui_key(0) == G_NONE, "bare Escape starts finite deadline");
    gui_keyboard.deadline = 0U;
    expect(gui_key(0) == G_BACK, "idle Escape deadline becomes Back without a blocking read");
    {
        char oversized[80];
        memset(oversized, '2', sizeof(oversized));
        oversized[0] = '\033'; oversized[1] = '[';
        oversized[sizeof(oversized) - 2U] = 'u'; oversized[sizeof(oversized) - 1U] = 'A';
        expect(write(input_writer, oversized, sizeof(oversized)) == (ssize_t)sizeof(oversized),
               "enqueue overlong unknown CSI followed by a valid key");
        expect(gui_key(0) == G_NONE && gui_key(0) == 'A',
               "overlong sequence is drained without leaking suffix as commands");
    }
}

static void test_poll_deadlines(struct gui_state *s)
{
    ime_fixture(s); memset(&gui_keyboard, 0, sizeof(gui_keyboard));
    expect(gui_poll_timeout(s, 100U) == 1000, "idle GUI wakes only for status clock, not 50Hz redraw");
    strcpy(s->query, "a");
    expect(gui_poll_timeout(s, 200U) == 1000, "search has no expiry wakeup");
    gui_keyboard.used = 1U; gui_keyboard.deadline = 225U;
    expect(gui_poll_timeout(s, 200U) == 25, "partial escape deadline takes priority");
    gui_keyboard.used = 0U; s->input_method.enabled = true; s->input_method.deadline = 500;
    expect(gui_poll_timeout(s, 200U) == 300, "IME deadline replaces paused prefix expiration");
    expect(gui_poll_timeout(s, 500U) == 0, "overdue IME request is processed immediately");
    gui_ime_close(s);
}

static void real_gui_drain(struct gui_state *s)
{
    uint64_t deadline = c1pkg_input_now_ms() + 10000U;
    while ((s->input_method.count || s->input_method.client.pending_sequence) && c1pkg_input_now_ms() < deadline) {
        (void)gui_ime_pump(s, &ime_config, c1pkg_input_now_ms());
        struct pollfd fd = {s->input_method.client.fd, c1_input_method_poll_events(&s->input_method), 0};
        if (s->input_method.count || s->input_method.client.pending_sequence) (void)poll(&fd, 1U, 100);
    }
    expect(!s->input_method.count && !s->input_method.client.pending_sequence && !s->ime_notice,
           "real-service ordered work completes within the finite harness deadline");
}

static void test_real_service(struct gui_state *s, const char *path)
{
    ime_fixture(s);
    strcpy(s->downloads.items[1].name, "你好阅读");
    if (c1_ime_connect(&s->input_method.client, path) != 0) { expect(0, "connect real Rime"); return; }
    struct c1_ime_request ready = {C1_IME_OP_STATUS, 0, 0, 0};
    struct c1_ime_response response;
    struct pollfd fd = {s->input_method.client.fd, POLLIN, 0};
    if (c1_ime_send(&s->input_method.client, &ready) != 1 || poll(&fd, 1U, 120000) != 1 ||
        c1_ime_receive(&s->input_method.client, &response) != 1 || !(response.flags & C1_IME_READY)) {
        expect(0, "real Rime cold deployment ready"); return;
    }
    s->ime_ready = 1;
    gui_ime_toggle(s, path, c1pkg_input_now_ms());
    const char *keys = "nihao";
    for (const char *key = keys; *key; ++key) (void)gui_ime_input(s, &ime_config, *key, c1pkg_input_now_ms());
    (void)gui_ime_input(s, &ime_config, G_REFRESH, c1pkg_input_now_ms());
    real_gui_drain(s);
    expect(!s->ime_notice && !strcmp(s->query, "你好") && s->selected[0] == 0U && s->count == 1U && !launch_calls,
           "real Rime nihao commit locates Chinese application without launching it");
    s->query[0] = 0;
    (void)gui_ime_input(s, &ime_config, 'n', c1pkg_input_now_ms());
    (void)gui_ime_input(s, &ime_config, G_PAGE_DOWN, c1pkg_input_now_ms());
    (void)gui_ime_input(s, &ime_config, G_RIGHT, c1pkg_input_now_ms());
    (void)gui_ime_input(s, &ime_config, G_RIGHT, c1pkg_input_now_ms());
    real_gui_drain(s);
    expect(s->ime_view.candidate_count == 5 && s->ime_view.highlighted_candidate == 2,
           "real Rime next page and two arrows highlight slot three");
    char selected[C1_IME_TEXT_CAPACITY];
    strcpy(selected, s->ime_view.candidates[2]);
    (void)gui_ime_input(s, &ime_config, G_ENTER, c1pkg_input_now_ms());
    real_gui_drain(s);
    expect(!strcmp(s->query, selected) && s->search_editing && !launch_calls,
           "real Rime Enter commits the highlighted next-page candidate");
    s->query[0] = 0;
    (void)gui_ime_input(s, &ime_config, 'n', c1pkg_input_now_ms());
    (void)gui_ime_input(s, &ime_config, G_PAGE_DOWN, c1pkg_input_now_ms());
    (void)gui_ime_input(s, &ime_config, G_PAGE_UP, c1pkg_input_now_ms());
    real_gui_drain(s);
    strcpy(selected, s->ime_view.candidates[4]);
    (void)gui_ime_input(s, &ime_config, '5', c1pkg_input_now_ms());
    real_gui_drain(s);
    expect(!strcmp(s->query, selected) && !launch_calls,
           "real Rime previous-page numeric keycap chooses visible slot five");
    gui_ime_close(s);
    if (!failures) puts("PASS: real Rime -> package GUI Chinese-name search");
}

int main(int argc, char **argv)
{
    int input[2];
    struct gui_state *s = calloc(1U, sizeof(*s));
    if (!s || pipe(input) || dup2(input[0], STDIN_FILENO) < 0) return 2;
    close(input[0]);
    initialize_gui(s);
    if (argc == 2) {
        test_real_service(s, argv[1]);
        gui_ime_close(s); close(input[1]); free(s);
        return failures ? 1 : 0;
    }
    test_poll_deadlines(s);
    test_keyboard(input[1]);
    test_commit_bounds(s);
    test_chinese_commit(s);
    test_candidate_pages(s);
    test_unconsumed_order(s);
    test_transport_failure(s);
    test_toggle_and_rejected_responses(s);
    test_lifecycle(s, input[1]);
    gui_ime_close(s);
    if (ime_peer >= 0) close(ime_peer);
    close(input[1]); free(s);
    if (failures) return 1;
    puts("PASS: public IME GUI UTF-8 commits, ordered page/highlight selection, queued-CSI deadlines, lifecycle and fail-closed queue");
    return 0;
}
