SUBDIRS = intl po

bin_PROGRAMS = memprof
noinst_PROGRAMS = testmemprof
pkgdata_DATA = memprof.glade leak.xpm gnome-question.png

lib_LTLIBRARIES = libmemintercept.la

memprof_SOURCES =	        \
	bfdutils.c		\
	leakdetect.c		\
	leakdetect.h		\
	main.c			\
	memprof.h		\
	process.c		\
	process.h		\
	profile.c		\
	profile.h

memprof_LDADD = $(MEMPROF_LIBS)


libmemintercept_la_SOURCES =    \
	memintercept.c		\
	memintercept.h

libmemintercept_la_LDFLAGS = -module -avoid-version

INCLUDES = 					   \
	$(MEMPROF_CFLAGS) 			   \
	-DDATADIR=\"$(pkgdatadir)\"		   \
	-DLIBDIR=\"$(libdir)\"			   \
	-DGNOMELOCALEDIR=\""$(datadir)/locale"\"

EXTRA_DIST =			\
	memprof.desktop		\
	gnome-question.png	\
	leak.png		\
	leak.xpm		\
	memprof.glade	  	\
	memprof.spec		\
	memprof-strings.c

Developmentdir = $(datadir)/gnome/apps/Development
Development_DATA = memprof.desktop

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