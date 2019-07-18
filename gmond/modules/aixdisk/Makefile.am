AM_CFLAGS  = -I$(top_builddir)/include -I$(top_builddir)/lib -I$(top_builddir)/libmetrics

if STATIC_BUILD
noinst_LTLIBRARIES    = libmodaixdisk.la
libmodaixdisk_la_SOURCES = mod_aixdisk.c 
else
pkglib_LTLIBRARIES    = modaixdisk.la
modaixdisk_la_SOURCES = mod_aixdisk.c 
modaixdisk_la_LDFLAGS = -module -avoid-version
modaixdisk_la_LIBADD = $(top_builddir)/libmetrics/libmetrics.la

EXTRA_DIST = ../conf.d/aixdisk.conf
endif

INCLUDES = @APR_INCLUDES@

