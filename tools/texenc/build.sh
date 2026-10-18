#!/bin/sh
# Builds texenc (tex3ds's pixel encoders and rg_etc1, see texenc.cpp):
#
#   tools/texenc/build.sh          WebAssembly for the browser packer: site/packer/texenc.mjs (an ES
#                                  module with the wasm inlined; runs in a worker and in Node) - needs Docker
#   tools/texenc/build.sh native   tools/texenc/build/libtexenc.so for check.py, with the host's c++
#
# The wasm build uses a pinned emscripten image, so the committed texenc.mjs can be rebuilt as is.
set -e
cd "$(dirname "$0")"
SRC="texenc.cpp rg_etc1.cpp"
CFLAGS="-O2 -ffp-contract=off -fno-exceptions -fno-rtti -DNDEBUG"
if [ "$1" = native ]; then
	mkdir -p build
	${CXX:-c++} $CFLAGS -std=c++11 -shared -fPIC -o build/libtexenc.so $SRC
	echo "built build/libtexenc.so"
	exit 0
fi
IMAGE=${EMSDK_IMAGE:-emscripten/emsdk:4.0.10}
docker run --rm -v "$PWD/../..:/src" -w /src/tools/texenc --user "$(id -u):$(id -g)" -e HOME=/tmp "$IMAGE" \
	em++ $CFLAGS -std=c++11 -o ../../site/packer/texenc.mjs $SRC \
	-sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createTexenc -sSINGLE_FILE=1 -sENVIRONMENT=web,worker,node \
	-sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=16MB -sEXPORTED_FUNCTIONS=_texenc_level,_texenc_size,_texenc_set_quality,_texenc_decode_etc1,_malloc,_free \
	-sEXPORTED_RUNTIME_METHODS=HEAPU8
echo "built site/packer/texenc.mjs"
