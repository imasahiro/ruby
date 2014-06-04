#!ruby

source_dir = "."
target_dir = "build"
debug_mode = !true

opt        = 3
debug      = 0

if debug_mode
  opt   = 0
  debug = 3
end

if ARGV.size != 0
  target_dir = ARGV[0]
end

`clang -pipe -O#{opt} -g#{debug} -x c-header -I #{target_dir} -I #{target_dir}/.ext/include/x86_64-darwin13 -Iinclude -I. #{source_dir}/jit/ruby_jit.h -o #{target_dir}/ruby_jit.h.pch`
