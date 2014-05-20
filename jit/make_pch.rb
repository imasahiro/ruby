#!ruby

source_dir = "."
target_dir = "build"

if ARGV.size != 0
    target_dir = ARGV[0]
end

`clang -pipe -O3 -g0 -x c-header -I #{target_dir} -I #{target_dir}/.ext/include/x86_64-darwin13 -Iinclude -I. #{source_dir}/jit/ruby_jit.h -o #{target_dir}/ruby_jit.h.pch`
