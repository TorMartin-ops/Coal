/* One list of applet names, shared by coalbox (dispatch) and sh (completion). */
#ifndef COALBOX_APPLETS_H
#define COALBOX_APPLETS_H

#define APPLET_LIST(X)                                                          \
    X(basename) X(cat) X(clear) X(cp) X(date) X(dirname) X(dmesg) X(echo)       \
    X(env) X(false) X(free) X(grep) X(halt) X(head) X(hexdump) X(kill) X(ls)    \
    X(mkdir) X(mount) X(mv) X(poweroff) X(ps) X(pwd) X(reboot) X(rm) X(rmdir)   \
    X(sleep) X(sort) X(sync) X(tail) X(touch) X(true) X(umount) X(uname)        \
    X(uptime) X(wc) X(which) X(yes)

#endif
