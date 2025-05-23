#!/bin/bash
SCM_URL=https://github.com/cisco/openh264/archive
SCM_TAG=v2.6.0
SCM_HASH=558544ad358283a7ab2930d69a9ceddf913f4a51ee9bf1bfb9e377322af81a69

source $(dirname "${BASH_SOURCE[0]}")/android-build-common.sh

function build {
  echo "Building architecture $1..."
  BASE=$(pwd)
  common_run cd $BUILD_SRC
  PATH=$ANDROID_NDK:$PATH
  MAKE="make LDFLAGS=-static-libstdc++ PATH=$PATH ENABLEPIC=Yes OS=android NDKROOT=$ANDROID_NDK NDK_TOOLCHAIN_VERSION=clang TARGET=android-$2 NDKLEVEL=$2 ARCH=$1 -j libraries"

  common_run export QUIET_AR="$CCACHE "
  common_run export QUIET_ASM="$CCACHE "
  common_run export QUIET_CC="$CCACHE "
  common_run export QUIET_CCAR="$CCACHE "
  common_run export QUIET_CXX="$CCACHE "

  common_run $MAKE
  # Install creates a non optimal directory layout, fix that
  common_run $MAKE PREFIX=$BUILD_SRC/libs/$1 install
  common_run cd $BASE
}

# Run the main program.
common_parse_arguments $@
common_update $SCM_URL $SCM_TAG $BUILD_SRC $SCM_HASH

for ARCH in $BUILD_ARCH; do
  case $ARCH in
  "armeabi")
    OARCH="arm"
    ;;
  "armeabi-v7a")
    OARCH="arm"
    ;;
  "arm64-v8a")
    OARCH="arm64"
    ;;
  *)
    OARCH=$ARCH
    ;;
  esac

  echo "$ARCH=$OARCH"

  build $OARCH $NDK_TARGET

  if [ ! -d $BUILD_DST/$ARCH/include ]; then
    common_run mkdir -p $BUILD_DST/$ARCH/include
  fi

  common_run cp -L -r $BUILD_SRC/libs/$OARCH/include/ $BUILD_DST/$ARCH/
  if [ ! -d $BUILD_DST/$ARCH ]; then
    common_run mkdir -p $BUILD_DST/$ARCH
  fi
  common_run cp -L $BUILD_SRC/libs/$OARCH/lib/*.so $BUILD_DST/$ARCH/
done
