#!/bin/sh
# Run this to generate all the initial makefiles, etc.

srcdir=`dirname $0`
test -z "$srcdir" && srcdir=.

PKG_NAME="MemProf"

(test -f $srcdir/configure.in \
  && test -f $srcdir/memprof.glade) || {
    echo -n "**ERROR**: Directory "\`$srcdir\`" does not look like the"
    echo " top-level memprof directory"
    exit 1
}

which gnome-autogen.sh || {
    echo "You need to install gnome-common from the GNOME CVS"
    exit 1
}
USE_GNOME2_MACROS=1 . gnome-autogen.sh
