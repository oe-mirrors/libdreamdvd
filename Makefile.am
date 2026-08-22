AM_CFLAGS = @DVDNAV_CFLAGS@

lib_LTLIBRARIES = libdreamdvd.la

libdreamdvd_la_SOURCES = \
	dreamalsa.c \
	dreamalsa.h \
	logo.h \
	main.c \
	main.h \
	mpegaudio_enc.c \
	mpegaudio_enc.h \
	mpegaudioenc.h

libdreamdvd_la_LIBADD = \
	@DVDNAV_LIBS@ \
	@LIBDL_LIBS@ \
	@LIBM_LIBS@

pkgincludedir = ${includedir}/dreamdvd
pkginclude_HEADERS = ddvdlib.h

pkgconfigdir = ${libdir}/pkgconfig
pkgconfig_DATA = libdreamdvd.pc
