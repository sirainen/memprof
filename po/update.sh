#!/bin/sh

xgettext --default-domain=evolution --directory=.. \
  --add-comments --keyword=_ --keyword=N_ \
  --files-from=./POTFILES.in \
&& test ! -f MemProf.po \
   || ( rm -f ./MemProf.pot \
    && mv MemProf.po ./MemProf.pot )
