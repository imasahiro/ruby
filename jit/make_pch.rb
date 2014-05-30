#!ruby

source_dir = "."
target_dir = "build"
opt        = 0
debug      = 3

if ARGV.size != 0
    target_dir = ARGV[0]
end

`clang -pipe -O#{opt} -g#{debug} -x c-header -I #{target_dir} -I #{target_dir}/.ext/include/x86_64-darwin13 -Iinclude -I. #{source_dir}/jit/ruby_jit.h -o #{target_dir}/ruby_jit.h.pch`
