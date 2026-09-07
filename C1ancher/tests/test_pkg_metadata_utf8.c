/* Link repo.c, util.c and text.c with -ffunction-sections -fdata-sections
 * -Wl,--gc-sections to isolate metadata parsing from transport/crypto I/O. */
#include "pkg/pkg.h"
#include "pkg/text.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static struct c1pkg_index index_data, saved;

static int parse(int format, const char *name, const char *author)
{
    char data[2048], error[256] = "";
    int length = snprintf(data, sizeof(data),
        "C1PKG-INDEX %d\nS\t1\nP\treader\t1.0.0\t%s\treader.tar.gz\t"
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
        "\t100\tbin/reader%s%s\n", format, name, format == 2 ? "\t" : "", format == 2 ? author : "");
    assert(length > 0 && (size_t)length < sizeof(data));
    return c1pkg_repo_parse((const unsigned char *)data, (size_t)length,
                            &index_data, error, sizeof(error));
}

int main(void)
{
    const char *const invalid[] = {"\xe4\xb8", "\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80",
        "A\xe2\x80\xae" "B", "A\xe2\x81\xa9" "B", "A\xe2\x80\x8b" "B", "\302\205",
        "\xef\xbf\xbd", "　边界", "边界　", "bad\tfield", "bad\nfield"};
    char boundary[44];
    size_t i;
    assert(C1PKG_NAME_MAX == C1PKG_LABEL_MAX_BYTES);
    assert(parse(1, "中文阅读器", "") == 0);
    assert(strcmp(index_data.packages[0].name, "中文阅读器") == 0);
    assert(strcmp(index_data.packages[0].author, "Unknown") == 0);
    assert(parse(2, "中文阅读器", "张三 / 王小明") == 0);
    assert(strcmp(index_data.packages[0].name, "中文阅读器") == 0);
    assert(strcmp(index_data.packages[0].author, "张三 / 王小明") == 0);
    saved = index_data;
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        assert(parse(2, invalid[i], "作者") != 0);
        assert(memcmp(&index_data, &saved, sizeof(saved)) == 0);
        assert(parse(2, "软件", invalid[i]) != 0);
        assert(memcmp(&index_data, &saved, sizeof(saved)) == 0);
        assert(parse(1, invalid[i], "") != 0);
        assert(memcmp(&index_data, &saved, sizeof(saved)) == 0);
    }
    for (i = 0; i < 13; ++i) memcpy(boundary + i * 3, "中", 3);
    boundary[39] = 'A'; boundary[40] = 0;
    assert(parse(2, boundary, boundary) == 0);
    assert(strlen(index_data.packages[0].name) == 40);
    assert(strlen(index_data.packages[0].author) == 40);
    boundary[40] = 'B'; boundary[41] = 0;
    assert(parse(2, boundary, "作者") != 0);
    assert(parse(2, "软件", boundary) != 0);
    puts("package UTF-8 metadata tests passed");
    return 0;
}
