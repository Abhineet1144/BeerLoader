/* KeyValues parsing: text appinfo shapes and binary package records. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "vdf.h"

int main(void)
{
    const char text[] =
        "\"appinfo\"\n{\n"
        "\t\"appid\"\t\t\"480\"\n"
        "\t// a comment\n"
        "\t\"common\"\n\t{\n"
        "\t\t\"name\"\t\t\"Space \\\"War\\\"\"\n"
        "\t\t\"type\"\t\t\"Game\"\n"
        "\t\t\"oslist\"\t\"windows,linux\" [$WIN32]\n"
        "\t}\n"
        "\t\"depots\" { \"481\" { \"name\" \"Content\" } }\n"
        "}\n";
    VdfNode *root = vdf_parse_text(text, sizeof(text) - 1);
    assert(root);
    VdfNode *info = vdf_child(root, "appinfo");
    assert(info);
    assert(!strcmp(vdf_string(info, "appid", ""), "480"));
    assert(!strcmp(vdf_string(info, "COMMON/Name", ""), "Space \"War\""));
    assert(!strcmp(vdf_string(info, "common/oslist", ""), "windows,linux"));
    assert(!strcmp(vdf_string(info, "depots/481/name", ""), "Content"));
    assert(!strcmp(vdf_string(info, "common/missing", "fallback"), "fallback"));
    vdf_free(root);

    assert(!vdf_parse_text("\"a\" { \"b\" \"c\"", 14));       /* unclosed block */
    assert(!vdf_parse_text("\"a\" \"unterminated", 17));

    /* {"10": {"packageid": 10, "appids": {"0": 10, "1": 80}, "name": "pkg"}} */
    const unsigned char binary[] = {
        0, '1', '0', 0,
            2, 'p', 'a', 'c', 'k', 'a', 'g', 'e', 'i', 'd', 0, 10, 0, 0, 0,
            0, 'a', 'p', 'p', 'i', 'd', 's', 0,
                2, '0', 0, 10, 0, 0, 0,
                2, '1', 0, 80, 0, 0, 0,
            8,
            1, 'n', 'a', 'm', 'e', 0, 'p', 'k', 'g', 0,
        8,
        8,
    };
    root = vdf_parse_binary(binary, sizeof(binary));
    assert(root && root->child && !strcmp(root->child->key, "10"));
    assert(!strcmp(vdf_string(root->child, "packageid", ""), "10"));
    assert(!strcmp(vdf_string(root->child, "appids/1", ""), "80"));
    assert(!strcmp(vdf_string(root->child, "name", ""), "pkg"));
    vdf_free(root);
    assert(!vdf_parse_binary(binary, 12));                     /* truncated */

    puts("steam vdf tests passed");
    return 0;
}
