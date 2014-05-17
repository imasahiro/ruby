#!/bin/sh
BUILD_DIR=build
rm -rf jit.zip ruby-jit
wget https://github.com/imasahiro/ruby/archive/jit.zip
#git clone https://github.com/imasahiro/ruby.git ruby-jit
cd ruby-jit
#git checkout jit
autoreconf -ivf
mkdir ${BUILD_DIR}
(cd ${BUILD_DIR} &&
        ../configure --prefix=$HOME &&
            make -j8 >& /dev/null )

./jit/make_pch.sh
ruby ./benchmark/driver.rb -v \
            --executables="ruby; built-ruby::${BUILD_DIR}/ruby" \
            --pattern='bm_' --directory=./benchmark
