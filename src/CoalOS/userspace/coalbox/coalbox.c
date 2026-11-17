/* coalbox: one binary, many commands. Dispatches on the basename of argv[0],
 * or on the first argument when invoked as "coalbox". */

#include "cb.h"
#include "applets.h"

struct applet {
    const char *name;
    int (*fn)(int, char **);
};

#define ROW(n) { #n, n##_main },
static const struct applet applets[] = { APPLET_LIST(ROW) };

int main(int argc, char **argv) {
    const char *name = cb_base(argv[0]);
    if (!strcmp(name, "coalbox") && argc > 1) {
        argc--;
        argv++;
        name = cb_base(argv[0]);
    }
    for (unsigned i = 0; i < sizeof applets / sizeof applets[0]; i++) {
        if (!strcmp(name, applets[i].name)) {
            prog = applets[i].name;
            return applets[i].fn(argc, argv);
        }
    }
    if (!strcmp(name, "coalbox")) {
        fputs("usage: coalbox <applet> [args]\napplets:", stdout);
        for (unsigned i = 0; i < sizeof applets / sizeof applets[0]; i++) printf(" %s", applets[i].name);
        putchar('\n');
        return 0;
    }
    fprintf(stderr, "coalbox: %s: applet not found\n", name);
    return 127;
}
