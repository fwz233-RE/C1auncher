#ifndef C1_IME_CLIENT_H
#define C1_IME_CLIENT_H

/* Linux C17 SDK. No Rime/C++/X11 dependencies; all strings are UTF-8.
 * One focused client and one outstanding request per connection. This is a
 * local state object, NOT a wire struct. Initialize with C1_IME_CLIENT_INIT.
 * On transport failure do not replay an outstanding key: it may have committed.
 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define C1_IME_DEFAULT_SOCKET "/run/c1-ime/socket"
#define C1_IME_PROTOCOL_VERSION 1u
#define C1_IME_MAX_PACKET 8192u
#define C1_IME_MAX_CANDIDATES 5u
#define C1_IME_TEXT_CAPACITY 1024u

/* X11 keysym values, without a dependency on Xlib. */
#define C1_IME_KEY_BACKSPACE 0xff08u
#define C1_IME_KEY_RETURN 0xff0du
#define C1_IME_KEY_ESCAPE 0xff1bu
#define C1_IME_KEY_LEFT 0xff51u
#define C1_IME_KEY_UP 0xff52u
#define C1_IME_KEY_RIGHT 0xff53u
#define C1_IME_KEY_DOWN 0xff54u
#define C1_IME_KEY_PAGE_UP 0xff55u
#define C1_IME_KEY_PAGE_DOWN 0xff56u
#define C1_IME_KEY_SPACE 0x20u

/* Supported X11/Rime modifier bits; only key-down events are supported. */
#define C1_IME_MOD_SHIFT 1u
#define C1_IME_MOD_LOCK 2u
#define C1_IME_MOD_CONTROL 4u
#define C1_IME_MOD_ALT 8u
#define C1_IME_MOD_SUPER 64u

/* MODE value: 0 English, 1 Chinese. PURPOSE value: one of the values below.
 * SELECT value: zero-based visible candidate [0, 4].
 * PAGE value: 0 previous, 1 next. Other operations require value == 0.
 * Only KEY carries keysym/modifiers; all other operations require them zero.
 */
enum c1_ime_operation {
    C1_IME_OP_STATUS = 1,
    C1_IME_OP_MODE = 2,
    C1_IME_OP_PURPOSE = 3,
    C1_IME_OP_KEY = 4,
    C1_IME_OP_SELECT = 5,
    C1_IME_OP_PAGE = 6,
    C1_IME_OP_CANCEL = 7
};
enum c1_ime_purpose {
    C1_IME_PURPOSE_NORMAL = 0,
    C1_IME_PURPOSE_PASSWORD = 1
};
#define C1_IME_MODE_ENGLISH 0u
#define C1_IME_MODE_CHINESE 1u
#define C1_IME_PAGE_PREVIOUS 0u
#define C1_IME_PAGE_NEXT 1u
enum c1_ime_response_flags {
    C1_IME_READY = 1u << 0,
    C1_IME_CONSUMED = 1u << 1,
    C1_IME_CHINESE = 1u << 2,
    C1_IME_PASSWORD = 1u << 3,
    C1_IME_COMPOSING = 1u << 4,
    C1_IME_TRUNCATED = 1u << 5
};
enum c1_ime_status {
    C1_IME_STATUS_OK = 0,
    C1_IME_STATUS_BAD_REQUEST = 1,
    C1_IME_STATUS_LIMIT = 2
};

struct c1_ime_request {
    uint32_t operation;
    uint32_t keysym;
    uint32_t modifiers;
    uint32_t value;
};

struct c1_ime_response {
    uint32_t sequence;
    struct c1_ime_request request; /* exact original request, for key forwarding */
    uint32_t flags;
    uint32_t status;
    uint32_t candidate_count;
    char preedit[C1_IME_TEXT_CAPACITY];
    char commit[C1_IME_TEXT_CAPACITY];
    char candidates[C1_IME_MAX_CANDIDATES][C1_IME_TEXT_CAPACITY];
    /* Local UI metadata, NOT a wire field. The raw SDK initializes this to 0;
     * an ordered interaction adapter may move it within the acknowledged page.
     * Rebuild C consumers when updating this source-level SDK struct. */
    uint32_t highlighted_candidate;
};

struct c1_ime_client {
    int fd;
    uint32_t next_sequence;
    uint32_t pending_sequence;
    struct c1_ime_request pending_request;
};
#define C1_IME_CLIENT_INIT { -1, 1u, 0u, { 0u, 0u, 0u, 0u } }

/* Returns 0 or -1 with errno; never starts a service or blocks for deployment.
 * NULL path selects C1_IME_DEFAULT_SOCKET. Socket parent must be owned by the
 * effective uid and mode 0700, socket mode 0600, peer uid must match.
 * Connect success is NOT ready: send STATUS and await C1_IME_READY. A second
 * client is disconnected by the service while the first retains focus.
 */
int c1_ime_connect(struct c1_ime_client *client, const char *path);
int c1_ime_client_fd(const struct c1_ime_client *client);

/* Returns 1 if sent, 0 on EAGAIN, -1 on failure (errno).
 * Only 1 means accepted by the kernel, not processed by Rime. EBUSY means
 * another request is pending. Poll fd for POLLOUT after a 0 return.
 */
int c1_ime_send(struct c1_ime_client *client, const struct c1_ime_request *request);

/* Returns 1 for a response, 0 on EAGAIN, -1 on failure (errno).
 * Poll fd for POLLIN/HUP/ERR. Transport/protocol failure closes the client and
 * discards pending state. Never forward/replay an ambiguous pending key.
 * Forward response.request.keysym only when a valid response lacks CONSUMED;
 * text in commit is delivered exactly once, separately from the original key.
 */
int c1_ime_receive(struct c1_ime_client *client, struct c1_ime_response *response);

/* Focus loss: close immediately, discard UI preedit; reconnect starts English
 * normal-purpose with no composition. Close is safe repeatedly after init.
 * Set PASSWORD and await its response before sending any password key; better
 * bypass the SDK entirely in password fields and keep the connection closed.
 */
void c1_ime_close(struct c1_ime_client *client);

#ifdef __cplusplus
}
#endif
#endif
