#!/bin/sh
# Copyright (c) 2016, the Dartino project authors. Please see the AUTHORS file
# for details. All rights reserved. Use of this source code is governed by a
# BSD-style license that can be found in the LICENSE.md file.

# Temporary script to build and flash a Dart application on the
# discovery board.

# To run on Linux first run
# $ tools/stm/one-stm-lib.sh

# To run on Mac first build
# $ ninja -C out/ReleaseX64
# Then copy libone_disco_fletch.a from the out/ReleaseSTM and out/DebugSTM
# directories on a Linux machine, as that part does not build on Mac. These
# files are generated by running the script tools/stm/one-stm-lib.sh as
# mentioned above.
#
# Make sure that the Mac and Linux versions have the exact same version!!!

set -e

if [ -z "$1" ]; then
  echo "Usage: $0 <dart file>"
  exit 1
fi
DART_FILE=$1

OS="`uname`"
case $OS in
  'Linux')
    OS='linux'
    ;;
  'Darwin')
    OS='mac'
    ;;
  *)
    echo "Unsupported OS $OS"
    exit 1
    ;;
esac

TOOLCHAIN_PREFIX=third_party/gcc-arm-embedded/$OS/gcc-arm-embedded/bin/arm-none-eabi-
CC=${TOOLCHAIN_PREFIX}gcc
OBJCOPY=${TOOLCHAIN_PREFIX}objcopy

BUILDDIR=out/DebugSTM


# Get the dart file relative to out/DebugSTM.
OUT_RELATIVE_DART_FILE=$DART_FILE
if [[ "$OUT_RELATIVE_DART_FILE" != /* ]]; then
  OUT_RELATIVE_DART_FILE=../../$DART_FILE
fi

cd out/DebugSTM

echo "Generating snapshot"
../../out/ReleaseX64/fletch export $OUT_RELATIVE_DART_FILE to file snapshot
echo "Converting snapshot to object file"
../../$OBJCOPY -I binary -O elf32-littlearm -B arm snapshot snapshot.o
cd ../..

echo "Linking application"
$CC \
-specs=nano.specs \
-Tplatforms/stm/disco_fletch/generated/SW4STM32/configuration/STM32F746NGHx_FLASH.ld \
-Wl,--whole-archive \
-Wl,--gc-sections \
-mcpu=cortex-m7 \
-mthumb \
-mfloat-abi=hard \
-mfpu=fpv5-sp-d16 \
-Wl,-Map=output.map \
-Wl,--gc-sections \
-Wl,--wrap=__libc_init_array \
-Wl,--wrap=_malloc_r \
-Wl,--wrap=_malloc_r \
-Wl,--wrap=_realloc_r \
-Wl,--wrap=_calloc_r \
-Wl,--wrap=_free_r \
-o $BUILDDIR/disco_fletch.elf \
-Wl,--start-group \
$BUILDDIR/snapshot.o \
$BUILDDIR/libone_disco_fletch.a \
-Wl,--end-group \
-lstdc++ \
-Wl,--no-whole-archive

echo "Generating flashable image"
$OBJCOPY -O binary $BUILDDIR/disco_fletch.elf $BUILDDIR/disco_fletch.bin

echo "Flashing image"
tools/lk/flash-image.sh --disco $BUILDDIR/disco_fletch.bin
