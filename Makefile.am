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

libmemintercept_la_LDFLAGS = -avoid-version -module
libmemintercept_la_LIBS =

INCLUDES = 					   \
	$(MEMPROF_CFLAGS) 			   \
	-DDATADIR=\"$(pkgdatadir)\"		   \
	-DGNOMELOCALEDIR=\""$(datadir)/locale"\"

EXTRA_DIST =			\
	gnome-question.png	\
	leak.png		\
	leak.xpm		\
	memprof.glade	  	\
	memprof-strings.c
