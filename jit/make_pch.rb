#!ruby

source_dir = "."
target_dir = "build"
debug_mode = !true
cc         = "clang"

opt        = 3
debug      = 0

if debug_mode
  opt   = 0
  debug = 3
end

if ARGV.size > 0
  target_dir = ARGV[0]
end

if ARGV.size > 1
  source_dir = ARGV[1]
end

if ARGV.size > 2
  cc = ARGV[2]
end

target_path = File.realpath(File.dirname(target_dir))
source_path = File.realpath(source_dir)

header_paths = "-I#{target_path}"
header_paths += " -I#{source_path}"
header_paths += " -I#{target_path}/.ext/include/x86_64-darwin13/"
header_paths += " -I#{source_path}/include/ -I#{source_path}"

cmd = "#{cc} -pipe -O#{opt} -g#{debug} -x c-header #{header_paths} \
#{source_path}/jit/ruby_jit.h -o #{target_path}/ruby_jit.h.pch"
#puts cmd
`#{cmd}`

path = File.join(target_path, 'ruby_jit.h.pch')

print "static const char cmd_template[] = "
print "\"#{cc} -pipe -O#{opt} -g#{debug} -x c #{header_paths}"
print " -I#{source_path}/jit/"
print " -dynamiclib -include-pch #{path} -Wall -o %s %s\";";
