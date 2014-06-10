#!ruby

class IRArgument
  attr_accessor :type, :name, :variadic

  def initialize(name, type)
    @type = type
    @name = name
    @variadic = type.end_with?("[]")
    if @variadic
      @type = @type.split("[]")[0]
    end
  end
end

class OP
  attr_accessor :name, :def, :use, :arg, :variadic, :trans
  def initialize(name, has_def, has_use, trans, arg)
    @name = name
    @def = has_def == ":def"
    @use = has_use == ":use"
    @trans = trans == ":trans"
    @arg = []
    @variadic = false
    parse_arg(arg)
  end

  def parse_arg(arg)
    arg.split(",").map(&:strip).map { |e|
      a = IRArgument.new(*e.split(":"))
      @arg.push(a)
      if a.variadic == true
        @variadic = true
      end
    }
  end
end

irs = []

open(ARGV[0]) { |file|
  i = 0
  puts "// This file is auto generated by jit/gwir.rb"
  puts "// DO NOT TOUCH!"
  puts ""
  puts "// If you want to fix something, you must edit 'jit/gwir.def'"

  while l = file.gets
    if /^([a-zA-Z0-9_]+)/ =~ l
      /^([a-zA-Z0-9_]+) *(:def)? *(:use)? *(:trans)? *\((.*)\)$/ =~ l
      has_def = $2
      has_use = $3
      trans   = $4
      arg  = $5

      ir = OP.new($1, $2, $3, $4, $5)
      irs.push(ir)
      ##
      puts "#define OPCODE_I#{ir.name}   #{i}\n"
      puts "typedef struct I#{ir.name} {\n"
      puts "  lir_inst_t base;"
      ir.arg.each{|e|
        type = e.type
        name = e.name
        if e.variadic
          name = name + "[0]"
        end
        puts "  #{type} #{name};\n"
      }
      list = []
      if arg.length > 0
        fields = arg.split(",").map(&:strip)
        for f in fields
          v = f.split(":")
          list.push(v[1])
          list.push(v[0])
        end
      end
      puts "} I" + ir.name + ";\n\n"

      ## Emit_???
      print "static reg_t Emit_#{ir.name}(TraceRecorder *Rec"
      ir.arg.each{|e|
        type = e.type
        name = e.name
        if e.variadic
          name = name + "[]"
        end
        print ", #{type} #{name}"
      }

      puts ")\n{\n"
      if ir.variadic
        puts "  I#{ir.name} *ir = LIR_NEWINST_N(I#{ir.name}, argc);\n"
      else
        puts "  I#{ir.name} *ir = LIR_NEWINST(I#{ir.name});\n"
      end

      ir.arg.each{|e|
        if e.variadic || e.type == "RegPtr"
          puts "  int i;"
          puts "  for(i = 0; i < argc; i++) {"
          puts "    ir->#{e.name}[i] = #{e.name}[i];\n"
          puts "  }"
        else
          puts "  ir->#{e.name} = #{e.name};\n"
        end
      }

      if ir.variadic
        puts "  return ADD_INST_N(Rec, ir, argc);\n"
      else
        puts "  return ADD_INST(Rec, ir);\n"
      end

      puts "}\n"

      ## EmitSpecialInst_???
      if !ir.variadic and ir.trans
        print "static reg_t EmitSpecialInst_#{ir.name}(TraceRecorder *Rec"
        puts ", CALL_INFO ci, reg_t *regs)"
        puts "{\n"
        print "  return Emit_#{ir.name}(Rec"
        puts ir.arg.length.times.map {|i| ", regs[#{i}]" }.join("") + ");"
        puts "}\n"
      end

      puts "#if DUMP_LIR > 0"
      print "static void Dump_#{ir.name}(lir_inst_t *Inst)\n"
      puts "{\n"
      puts "  I#{ir.name} *ir = (I#{ir.name} *)Inst;\n"
      puts "  fprintf(stderr, \"  \" FMT_ID \" #{ir.name}\", lir_getid(&ir->base));"

      ir.arg.each{|e|
        t = e.type
        n = e.name
        puts "  fprintf(stderr, \" #{n}:\");\n"
        if e.variadic || e.type == "RegPtr"
          puts "  int i_#{n};"
          puts "  for(i_#{n} = 0; i_#{n} < ir->argc; i_#{n}++) {"
          if e.type == "RegPtr"
            puts "    #{t} val = &ir->#{n}[i_#{n}];"
          else
            puts "    #{t} val = ir->#{n}[i_#{n}];"
          end
          puts "    fprintf(stderr, \" \" FMT(#{t}), DATA(#{t}, val));\n"
          puts "  }"
        else
          puts "  fprintf(stderr, FMT(#{t}), DATA(#{t}, ir->#{n}));\n"
        end
      }

      puts "  fprintf(stderr, \"\\n\");"
      puts "}\n"
      puts "#endif /*DUMP_LIR > 0*/"

      puts "#define GWIR_USE_#{ir.name} #{ir.def ? 1 : 0}"
      puts "#define GWIR_DEF_#{ir.name} #{ir.use ? 1 : 0}"

      i += 1
    end
  end
}

puts "#define GWIR_EACH(OP) \\"
irs.each{|ir|
  puts "  OP(#{ir.name})\\"
}
puts ""

inlineop = {}
open(ARGV[1]) { |file|
  while l = file.gets
    if /^static inline VALUE rb_jit_exec_I([a-zA-Z0-9_]+) *\((.*)\)$/ =~ l
      inlineop[$1.to_s] = $1.to_s
    end
  end
}

puts "static lir_folder_t const_fold_funcs[] = {"
irs.each{|ir|
  if inlineop[ir.name]
    puts "(lir_folder_t) rb_jit_exec_I#{ir.name},"
  else
    puts "NULL,"
  end
}
puts "};"