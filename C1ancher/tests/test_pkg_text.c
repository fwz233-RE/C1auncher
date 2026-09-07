#include "pkg/text.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int ink(const uint8_t *frame, int x, int y)
{
    return (frame[(y / 8) * 296 + x] & (0x80U >> (y % 8))) != 0U;
}

static void labels(void)
{
    static const char *const valid[] = {
        "App", "中文软件", "張三 / 王小明", "阅读器 Reader", "Café", "内部　空格", "𠮷野家"
    };
    static const char *const invalid[] = {
        "", " leading", "trailing ", "　中文", "中文　", "\xc2\xa0" "App",
        "bad\tfield", "bad\nfield", "bad\001", "bad\177", "bad\302\205",
        "\x80", "\xc0\xaf", "\xc1\xbf", "\xc2", "\xe4\xb8", "\xe0\x80\x80",
        "\xed\xa0\x80", "\xf0\x80\x80\x80", "\xf4\x90\x80\x80", "\xf5\x80\x80\x80",
        "\xff", "A\xe2\x80\xae" "B", "A\xe2\x81\xa6" "B", "A\xd8\x9c" "B",
        "A\xe2\x80\x8b" "B", "A\xef\xbb\xbf" "B", "A\xe2\x80\xa8" "B",
        "A\xef\xb7\x90" "B", "A\xef\xbf\xbf" "B", "A\xef\xbf\xbd" "B",
        "A\xf3\xa0\x84\x80" "B", "A\xef\xb8\x8f" "B"
    };
    char boundary[44];
    size_t i;
    assert(!c1pkg_valid_label(NULL));
    for (i = 0; i < sizeof(valid) / sizeof(valid[0]); ++i) assert(c1pkg_valid_label(valid[i]));
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) assert(!c1pkg_valid_label(invalid[i]));
    memset(boundary, 'A', 40); boundary[40] = 0;
    assert(c1pkg_valid_label(boundary));
    boundary[40] = 'A'; boundary[41] = 0;
    assert(!c1pkg_valid_label(boundary));
    for (i = 0; i < 13; ++i) memcpy(boundary + i * 3, "中", 3);
    boundary[39] = 'A'; boundary[40] = 0;
    assert(c1pkg_valid_label(boundary));
    boundary[40] = 'B'; boundary[41] = 0;
    assert(!c1pkg_valid_label(boundary));
    memcpy(boundary + 39, "中", 4);
    assert(!c1pkg_valid_label(boundary));
}

static void rendering(void)
{
    struct { uint8_t before[32], frame[C1PKG_TEXT_FRAME_BYTES], after[32]; } guarded;
    uint8_t reference[C1PKG_TEXT_FRAME_BYTES] = {0}, empty[C1PKG_TEXT_FRAME_BYTES] = {0};
    uint8_t fallback[C1PKG_TEXT_FRAME_BYTES] = {0};
    const char *const samples[] = {"中", "国", "應", "閱", "龘", "㐀", "龍", "汉", "字", "，"};
    size_t i;
    int x, y;
    memset(&guarded, 0, sizeof(guarded));
    memset(guarded.before, 0xa5, sizeof(guarded.before));
    memset(guarded.after, 0xa5, sizeof(guarded.after));
    assert(c1pkg_text_width(NULL) == 0);
    assert(c1pkg_text_width("") == 0);
    assert(c1pkg_text_width("Ab 1") == 32);
    assert(c1pkg_text_width("中文A") == 40);
    assert(c1pkg_text_width("Café") == 32);
    assert(c1pkg_text_width("中\xe4\xb8") == 16);
    assert(c1pkg_text_width("A\xe2\x80\xae" "B") == 8);
    assert(c1pkg_text_width("😀") == 8);
    c1pkg_text(fallback, 0, 0, "😀", 16, 1);
    for (i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
        memset(reference, 0, sizeof(reference));
        c1pkg_text(reference, 0, 0, samples[i], 16, 1);
        assert(memcmp(reference, empty, sizeof(reference)) != 0);
        assert(memcmp(reference, fallback, sizeof(reference)) != 0);
    }
    memset(reference, 0, sizeof(reference));
    c1pkg_text(reference, 0, 0, "中", 16, 1);
    {
        /* Independent golden bitmap from the pinned upstream U+4E2D source. */
        static const uint16_t middle[16] = {0x0100,0x0100,0x0100,0x0100,0x3ff8,0x2108,0x2108,0x2108,
            0x2108,0x2108,0x3ff8,0x2108,0x0100,0x0100,0x0100,0x0100};
        for (y = 0; y < 16; ++y) for (x = 0; x < 16; ++x)
            assert(ink(reference, x, y) == ((middle[y] & (0x8000U >> x)) != 0U));
    }
    c1pkg_text(guarded.frame, 3, 7, "中", 15, 1);
    assert(memcmp(guarded.frame, empty, sizeof(empty)) == 0);
    c1pkg_text(guarded.frame, 3, 7, "中文", 16, 1);
    for (y = 0; y < 152; ++y) for (x = 0; x < 296; ++x) {
        int expected = x >= 3 && x < 19 && y >= 7 && y < 23 ? ink(reference, x - 3, y - 7) : 0;
        assert(ink(guarded.frame, x, y) == expected);
    }
    c1pkg_text(guarded.frame, 3, 7, "中", 16, 0);
    assert(memcmp(guarded.frame, empty, sizeof(empty)) == 0);
    /* Transparent white ink changes only the original glyph's set pixels. */
    memset(guarded.frame, 0xff, sizeof(guarded.frame));
    c1pkg_text(guarded.frame, 0, 0, "中", 16, 0);
    for (i = 0; i < sizeof(reference); ++i) assert((unsigned int)guarded.frame[i] + reference[i] == 255U);
    memset(guarded.frame, 0, sizeof(guarded.frame));
    c1pkg_text(guarded.frame, -5, -3, "中", 16, 1);
    for (y = 0; y < 152; ++y) for (x = 0; x < 296; ++x) {
        int expected = x < 11 && y < 13 ? ink(reference, x + 5, y + 3) : 0;
        assert(ink(guarded.frame, x, y) == expected);
    }
    c1pkg_text(guarded.frame, 290, 147, "中文", INT_MAX, 1);
    c1pkg_text(guarded.frame, INT_MIN, INT_MIN, "中文", INT_MAX, 1);
    c1pkg_text(guarded.frame, INT_MAX, INT_MAX, "中文", INT_MAX, 1);
    c1pkg_text(guarded.frame, INT_MIN, 0, "中文", INT_MAX, 1);
    c1pkg_text(guarded.frame, 0, 0, NULL, 100, 1);
    c1pkg_text(NULL, 0, 0, "中", 16, 1);
    for (i = 0; i < sizeof(guarded.before); ++i) assert(guarded.before[i] == 0xa5 && guarded.after[i] == 0xa5);
}

static void truncated_at_page_end(void)
{
    const long page = sysconf(_SC_PAGESIZE);
    unsigned char *memory;
    const char *const truncated[] = {"\xc2", "\xe4", "\xe4\xb8", "\xf0", "\xf0\x90", "\xf0\x90\x80"};
    uint8_t frame[C1PKG_TEXT_FRAME_BYTES] = {0};
    size_t i;
    assert(page > 0);
    memory = mmap(NULL, (size_t)page * 2U, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(memory != MAP_FAILED);
    assert(mprotect(memory + page, (size_t)page, PROT_NONE) == 0);
    for (i = 0; i < sizeof(truncated) / sizeof(truncated[0]); ++i) {
        size_t length = strlen(truncated[i]);
        char *text = (char *)memory + page - length - 1U;
        memcpy(text, truncated[i], length + 1U);
        assert(!c1pkg_valid_label(text));
        assert(c1pkg_text_width(text) == 0);
        c1pkg_text(frame, 0, 0, text, 296, 1);
    }
    assert(munmap(memory, (size_t)page * 2U) == 0);
}

int main(void)
{
    labels();
    rendering();
    truncated_at_page_end();
    puts("package UTF-8 text tests passed");
    return 0;
}
