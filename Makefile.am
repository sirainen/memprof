SUBDIRS = po

bin_PROGRAMS = memprof
noinst_PROGRAMS = testmemprof
pkgdata_DATA = memprof.glade leak.xpm memprof.png

pkglib_LTLIBRARIES = libmemintercept.la libspeedintercept.la

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
	stackstash.c		\
	stackstash.h		\
	tree.c

memprof_LDADD = $(MEMPROF_LIBS)
memprof_LDFLAGS = -export-dynamic

intercept_sources = 		\
	intercept.c		\
	intercept.h		\
	memintercept-utils.c	\
	memintercept-utils.h	\
	stack-frame.c		\
	stack-frame.h


libmemintercept_la_SOURCES =    \
	$(intercept_sources)	\
	memintercept.c		\
	memintercept.h	

libmemintercept_la_LDFLAGS = -module -avoid-version
libmemintercept_la_LIBADD = -ldl

libspeedintercept_la_SOURCES =  \
	$(intercept_sources)	\
	mi-perfctr.c		\
	mi-perfctr.h		\
	perfctr.h		\
	speedintercept.c	\
	speedintercept.h

libspeedintercept_la_LDFLAGS = -module -avoid-version
libspeedintercept_la_LIBADD = -ldl

INCLUDES = 					   \
	$(MEMPROF_CFLAGS) 			   \
	-DDATADIR=\"$(pkgdatadir)\"		   \
	-DPKGLIBDIR=\"$(pkglibdir)\"		   \
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
	intltool-extract.in	\
	intltool-merge.in	\
	intltool-update.in	\
	$(schema_DATA)

Developmentdir = $(datadir)/applications
Development_DATA = memprof.desktop

pixmapsdir = $(datadir)/pixmaps
pixmaps_DATA = memprof.png

DISTCLEANFILES =		\
	intltool-extract	\
	intltool-merge		\
	intltool-update

install-data-local:
	GCONF_CONFIG_SOURCE=$(GCONF_SCHEMA_CONFIG_SOURCE) \
	$(GCONFTOOL) --makefile-install-rule $(srcdir)/$(schema_DATA)

install-exec-hook:
	cd $(bindir) && ln -sf memprof speedprof

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

dist-hook: memprof.spec
	if grep '^- Version @VERSION@' $(srcdir)/memprof.spec.in ; then : ; else \
	   perl -ni.bak -e \
	      'print; if (/^%changelog/) { chomp ($$date = `date +"%a %b %d %Y"`); print "* $$date Owen Taylor <otaylor\@redhat.com>\n- Version @VERSION@\n\n"; }' \
	      $(srcdir)/memprof.spec.in ; \
	   ( CONFIG_FILES=memprof.spec CONFIG_HEADERS= $(SHELL) ./config.status ) ; \
	   cp -p $(srcdir)/memprof.spec.in $(distdir) ; \
	fi
	cp memprof.spec $(distdir)
