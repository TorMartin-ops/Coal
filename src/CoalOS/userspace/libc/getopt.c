#include <libc/unistd.h>
#include <libc/string.h>
#include <libc/stdio.h>

char *optarg;
int optind = 1, opterr = 1, optopt;

int getopt(int argc, char *const argv[], const char *opts) {
    static int pos;
    if (!pos) {
        if (optind >= argc || argv[optind][0] != '-' || !argv[optind][1]) return -1;
        if (!strcmp(argv[optind], "--")) { optind++; return -1; }
        pos = 1;
    }
    char c = argv[optind][pos++];
    const char *o = strchr(opts, c);
    int last = !argv[optind][pos];
    optopt = c;
    if (!o || c == ':') {
        if (opterr) fprintf(stderr, "%s: invalid option -- '%c'\n", argv[0], c);
        if (last) { pos = 0; optind++; }
        return '?';
    }
    if (o[1] == ':') {
        if (!last) optarg = (char *)argv[optind] + pos;
        else if (optind + 1 < argc) optarg = argv[++optind];
        else {
            pos = 0;
            optind++;
            if (opterr) fprintf(stderr, "%s: option requires an argument -- '%c'\n", argv[0], c);
            return '?';
        }
        pos = 0;
        optind++;
    } else if (last) {
        pos = 0;
        optind++;
    }
    return c;
}
