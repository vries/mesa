include $(top_srcdir)/src/gallium/Automake.inc

AM_CFLAGS = \
	$(PTHREAD_CFLAGS)

AM_CXXFLAGS = \
	$(GALLIUM_DRIVER_CXXFLAGS) \
	$(CXX11_CXXFLAGS)

AM_CPPFLAGS = \
	-I$(top_srcdir)/src/gtest/include \
	-I$(top_srcdir)/src/mapi \
	-I$(top_builddir)/src/mesa \
	-I$(top_srcdir)/src/mesa \
	-I$(top_builddir)/src/compiler/glsl \
	$(DEFINES)

if HAVE_STD_CXX11
TESTS = st-renumerate-test
check_PROGRAMS = st-renumerate-test

noinst_LIBRARIES = libmesa-st-tests-common.a
endif

libmesa_st_tests_common_a_SOURCES = \
	st_tests_common.cpp \
	st_tests_common.h

st_renumerate_test_SOURCES =			\
	test_glsl_to_tgsi_lifetime.cpp

st_renumerate_test_LDFLAGS = \
	$(LLVM_LDFLAGS)

st_renumerate_test_LDADD = \
	libmesa-st-tests-common.a \
	$(top_builddir)/src/mesa/libmesagallium.la \
	$(top_builddir)/src/mapi/shared-glapi/libglapi.la \
	$(top_builddir)/src/gallium/auxiliary/libgallium.la \
	$(top_builddir)/src/util/libmesautil.la \
	$(top_builddir)/src/gtest/libgtest.la \
	$(GALLIUM_COMMON_LIB_DEPS) \
	$(LLVM_LIBS)
