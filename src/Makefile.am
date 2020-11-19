bin_PROGRAMS = cachetest launcher
launcher_SOURCES = launcher.cc
if PAPI
LDADD = @PAPI_DIR@/lib/libpapi.a
cachetest_SOURCES = cachetest.cc Distribution.cc Buffer.cc genzipf.cc Perf_PAPI.cc Perf.cc Small_buffer.cc Large_buffer.cc
else
cachetest_SOURCES = cachetest.cc Distribution.cc Buffer.cc genzipf.cc Perf_Cachetest.cc Perf.cc Small_buffer.cc Large_buffer.cc
endif
cachetest_LDADD = -lpthread -L${CURDIR}/../deps/libfibre/src/ -lfibre
cachetest_LDFLAGS = -Wl,-rpath=${CURDIR}/../deps/libfibre/src/

AM_CPPFLAGS = -I./include -I${CURDIR}/../deps/libfibre/src/
AM_CXXFLAGS = -std=c++11
