SUBDIRS = po

bin_PROGRAMS = memprof
noinst_PROGRAMS = testmemprof
pkgdata_DATA = memprof.glade leak.xpm memprof.png

lib_LTLIBRARIES = libmemintercept.la

memprof_SOURCES =	        \
	bfdutils.c		\
	gui.h			\
	inode.c			\
	leakdetect.c		\
	leakdetect.h		\
	main.c			\
	memprof.h		\
	process.c		\
	process.h		\
	profile.c		\
	profile.h		\
	server.c		\
	server.h		\
	tree.c

memprof_LDADD = $(MEMPROF_LIBS)
memprof_LDFLAGS = -export-dynamic

libmemintercept_la_SOURCES =    \
	memintercept.c		\
	memintercept.h

libmemintercept_la_LDFLAGS = -module -avoid-version
libmemintercept_la_LIBADD = -ldl

INCLUDES = 					   \
	$(MEMPROF_CFLAGS) 			   \
	-DDATADIR=\"$(pkgdatadir)\"		   \
	-DLIBDIR=\"$(libdir)\"			   \
	-DGNOMELOCALEDIR=\""$(datadir)/locale"\"

schemadir = @GCONF_SCHEMA_FILE_DIR@
schema_DATA = memprof.schemas

EXTRA_DIST =			\
	memprof.desktop		\
	leak.png		\
	leak.xpm		\
	memprof.glade	  	\
	memprof.spec.in		\
	memprof.png         	\
	memprof-strings.c	\
	$(schema_DATA)

Developmentdir = $(datadir)/applications
Development_DATA = memprof.desktop

pixmapsdir = $(datadir)/pixmaps
pixmaps_DATA = memprof.png


install-data-local:
	GCONF_CONFIG_SOURCE=$(GCONF_SCHEMA_CONFIG_SOURCE) \
	gconftool-2 --makefile-install-rule $(schema_DATA)

.PHONY: copyrights

copyrights:
	FILES="$(memprof_SOURCES) $(libmemintercept_la_SOURCES)";	  \
	for i in $$FILES ; do 				  		  \
           mv $$i $$i.bak; 				  		  \
           cat copyright-header > $$i;			  		  \
	   if grep "/\*====\*/" $$i.bak ; then          	 	  \
	     perl -ne 'print if m!/\*====\*/!..0' $$i.bak >> $$i; 	  \
	   else						  		  \
	     echo "/*====*/" >> $$	i;		  		  \
             cat $$i.bak >> $$i;				  	  \
	   fi;								  \
	done

dist-hook:
	if grep '^- Version @VERSION@' $(srcdir)/memprof.spec.in ; then : ; else \
	   perl -ni.bak -e \
	      'print; if (/^%changelog/) { chomp ($$date = `date +"%a %b %d %Y"`); print "* $$date Owen Taylor <otaylor\@redhat.com>\n- Version @VERSION@\n\n"; }' \
	      $(srcdir)/memprof.spec.in ; \
	   ( CONFIG_FILES=memprof.spec CONFIG_HEADERS= $(SHELL) ./config.status ) ; \
	   cp -p $(srcdir)/memprof.spec.in $(distdir) ; \
	fi
	cp memprof.spec $(distdir)
