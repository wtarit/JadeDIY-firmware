#!/bin/bash
#
# Build the Jade firmware into a shared library for in-process debugging
#
# ./make_libjade.sh [-v]
#
# Defaults to a debug build with gcc. Set CC, CXX, CFLAGS and LDFLAGS to change the defaults,
# e.g. to build with sanitizers:
#
# CFLAGS="-O1 -ggdb -fsanitize=address -fsanitize=undefined -fsanitize=alignment -fsanitize-address-use-after-scope -fno-sanitize-recover=all" LDFLAGS="-fsanitize=address -fsanitize=undefined -fsanitize=alignment -fsanitize-address-use-after-scope -fno-sanitize-recover=all" ./make_libjade.sh
# rm -f *.pin; LD_LIBRARY_PATH=$PWD ASAN_OPTIONS=symbolize=1,detect_leaks=0 LD_PRELOAD=/usr/lib/gcc/x86_64-linux-gnu/10/libasan.so PYTHONMALLOC=malloc PYTHONDEVMODE=1 python ./test_jade.py --run_sw --swtimeout=1
#
set -e

if [ "${1}" = "-v" ]; then
    set -x
fi

# Note -DVERIFY enables (expensive) libsecp256k1 verification
SW_JADE_CFLAGS="${CFLAGS:--O0 -ggdb3} -DVERIFY"
SW_JADE_LDFLAGS="${LDFLAGS}"
SW_JADE_CC="${CC:-gcc}"
SW_JADE_CXX="${CXX:-g++}"


# FIXME: BC-UR (and therefor QRCode) support using the existing code requires:
#        - Jade changes to be made via ifdef, not unconditionally
#        - Removed source files to be put back so they can be built
# For now, copy the upstream source used by gdk and use that instead.
if [ ! -d libjade/bc-ur-0.3.0 ]; then
    echo "downloading bc-ur-0.3.0 ..."
    wget https://github.com/BlockchainCommons/bc-ur/archive/refs/tags/0.3.0.tar.gz
    tar xf 0.3.0.tar.gz -C libjade
    rm 0.3.0.tar.gz
fi

# BC-UR
${SW_JADE_CXX} -c ${SW_JADE_CFLAGS} \
    -shared -pthread -fPIC -DPIC -std=c++17 \
    -fvisibility=hidden \
    -I . \
    -I libjade/include/ \
    -I components/libwally-core/upstream/include/ \
    -I managed_components/espressif__cbor/tinycbor/src/ \
    -I libjade/bc-ur-0.3.0/src/ \
    libjade/bc_ur.cpp -o libjade/bc_ur.o

# Jade Firmware
${SW_JADE_CC} -c ${SW_JADE_CFLAGS} \
    -shared -pthread -fPIC -DPIC \
    -fvisibility=hidden \
    -Wall -W -Wno-format -Wno-unused-function -Wno-unused-parameter \
    -Wno-sign-compare -Wno-missing-field-initializers -Wno-narrowing \
    -I . \
    -I main \
    -I components/assets/ \
    -I components/esp32-quirc/lib/ \
    -I components/esp32-quirc/ \
    -I build/esp-idf/assets/ \
    -I libjade/include/ \
    -I libjade/bc-ur-0.3.0/src/ \
    -I components/libwally-core/upstream/include/ \
    -I components/libwally-core/upstream \
    -I components/libwally-core/upstream/src \
    -I components/libwally-core/upstream/src/ccan \
    -I components/libwally-core/ \
    -I components/libwally-core/upstream/src/secp256k1/include/ \
    -Imanaged_components/espressif__cbor/tinycbor/src/ \
    libjade/libjade.c -o libjade/libjade.o

# libjade.so
${SW_JADE_CXX} -shared -pthread -Wl,--no-undefined \
    -fPIC -DPIC -fvisibility=hidden \
    libjade/libjade.o libjade/bc_ur.o ${SW_JADE_LDFLAGS} -lm -lz -o libjade.so

rm -f libjade/libjade.o libjade/bc_ur.o
