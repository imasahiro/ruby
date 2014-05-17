#!/bin/sh

BUILD=build/
clang -pipe -O3 -g0 -x c-header -I $BUILD/ -I $BUILD/.ext/include/x86_64-darwin13 -Iinclude -I. jit/ruby_jit.h -o $BUILD/ruby_jit.h.pch
