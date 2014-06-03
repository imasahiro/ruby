#
# configuration file for specialized gwir instruction
#
puts "// This file is auto generated by jit/yarv2gwir.rb"
puts "// DO NOT TOUCH!"
puts ""
puts "// If you want to fix something, you must edit 'jit/yarv2gwir.rb'"

DATA = [
    ["FixnumAddOverflow", "opt_plus",  "+", [:Fixnum, :Fixnum]],
    ["FixnumSubOverflow", "opt_minus", "-", [:Fixnum, :Fixnum]],
    ["FixnumMulOverflow", "opt_mult",  "*", [:Fixnum, :Fixnum]],
    ["FixnumDivOverflow", "opt_div",   "/", [:Fixnum, :Fixnum]],
    ["FixnumModOverflow", "opt_mod",   "%", [:Fixnum, :Fixnum]],

    ["FixnumEq", "opt_eq",  "==", [:Fixnum, :Fixnum]],
    ["FixnumNe", "opt_neq", "!=", [:Fixnum, :Fixnum]],
    ["FixnumGt", "opt_gt",  ">",  [:Fixnum, :Fixnum]],
    ["FixnumGe", "opt_ge",  ">=", [:Fixnum, :Fixnum]],
    ["FixnumLt", "opt_lt",  "<",  [:Fixnum, :Fixnum]],
    ["FixnumLe", "opt_le",  "<=", [:Fixnum, :Fixnum]],

    ["FixnumAnd",        "opt_send_simple", "&", [:Fixnum, :Fixnum]],
    ["FixnumOr",         "opt_send_simple", "|", [:Fixnum, :Fixnum]],
    ["FixnumXor",        "opt_send_simple", "^", [:Fixnum, :Fixnum]],
    ["FixnumLshift",     "opt_send_simple", "<<",[:Fixnum, :Fixnum]],
    ["FixnumRshift",     "opt_send_simple", ">>",[:Fixnum, :Fixnum]],
    ["FixnumComplement", "opt_send_simple", "~", [:Fixnum]],
    ["FixnumSucc",       "opt_succ",        "succ", [:Fixnum]],

    ["FloatAdd", "opt_plus",  "+", [:Float, :Float]],
    ["FloatSub", "opt_minus", "-", [:Float, :Float]],
    ["FloatMul", "opt_mult",  "*", [:Float, :Float]],
    ["FloatDiv", "opt_div",   "/", [:Float, :Float]],
    ["FloatMod", "opt_mod",   "%", [:Float, :Float]],

    ["FloatEq", "opt_eq",  "==", [:Float, :Float]],
    ["FloatNe", "opt_neq", "!=", [:Float, :Float]],
    ["FloatGt", "opt_gt",  ">",  [:Float, :Float]],
    ["FloatGe", "opt_ge",  ">=", [:Float, :Float]],
    ["FloatLt", "opt_lt",  "<",  [:Float, :Float]],
    ["FloatLe", "opt_le",  "<=", [:Float, :Float]],

    ["MathSin",   "opt_send_simple", "sin",   [:Math, :_]],
    ["MathCos",   "opt_send_simple", "cos",   [:Math, :_]],
    ["MathTan",   "opt_send_simple", "tan",   [:Math, :_]],
    ["MathExp",   "opt_send_simple", "expr",  [:Math, :_]],
    ["MathSqrt",  "opt_send_simple", "sqrt",  [:Math, :_]],
    ["MathLog10", "opt_send_simple", "log10", [:Math, :_]],
    ["MathLog2",  "opt_send_simple", "log2",  [:Math, :_]],

    ["StringLength", "opt_length",  "length", [:String]],
    ["StringLength", "opt_size",    "size",   [:String]],
    ["StringEmptyP", "opt_empty_p", "empty?", [:String]],
    ["StringConcat", "opt_plus",    "+",      [:String, :String]],
    ["StringConcat", "opt_ltlt",    "<<",     [:String, :String]],

    ["ArrayLength", "opt_length",  "length", [:Array]],
    ["ArrayLength", "opt_size",    "size",   [:Array]],
    ["ArrayEmptyP", "opt_empty_p", "empty?", [:Array]],
    ["ArrayConcat", "opt_plus",    "+",      [:Array, :_]],
    ["ArrayConcat", "opt_ltlt",    "<<",     [:Array, :_]],
    ["ArrayGet",    "opt_aref|opt_aref_with", "[]",  [:Array, :Fixnum]],
    ["ArraySet",    "opt_aset|opt_aset_with", "[]=", [:Array, :Fixnum, :_]],

    ["HashLength", "opt_length",  "length", [:Hash]],
    ["HashLength", "opt_size",    "size",   [:Hash]],
    ["HashEmptyP", "opt_empty_p", "empty?", [:Hash]],
    ["HashGet",    "opt_aref|opt_aref_with", "[]",  [:Hash, :_]],
    ["HashSet",    "opt_aset|opt_aset_with", "[]=", [:Hash, :_, :_]],

    ["RegExpMatch", "opt_regexpmatch1|opt_regexpmatch2", "=~", [:Regexp, :String]],

    ["TimeSucc",    "opt_succ", "succ", [:Time]],

    ["GetPropertyName", "opt_send_simple", "#getter", [:Object]],
    ["SetPropertyName", "opt_send_simple", "#setter", [:Object, :_]]
];

class Rule
    attr_accessor :type, :val, :need_guard

    def initialize(type, val, guard = :default)
        @type = type
        @val  = val
        @need_guard = guard
    end

    def emit_guard
    end
end

class Opcode < Rule
    def initialize(val)
        super("", val)
    end
    def to_s
        @val.split("|").map {|v|
            "opcode == BIN(#{v})"
        }.join(" || ")
    end
end

class Mtype < Rule
    def initialize(val, guard)
        super("", val, guard)
    end
    def symbolize(sym)
        return "VM_METHOD_TYPE_IVAR"    if sym == "#getter";
        return "VM_METHOD_TYPE_ATTRSET" if sym == "#setter";
        return sym
    end

    def to_s
        "ci->me && ci->me->def->type == #{symbolize(@val)}"
    end

    def emit_guard
        "Emit_GuardMethodCache(Rec, pc, regs[0], ci)"
    end
end

class Mid < Rule
    def initialize(val)
        super("", val)
    end
    def symbolize(sym)
        return "idPLUS"      if sym == '+'
        return "idMINUS"     if sym == '-'
        return "idMULT"      if sym == '*'
        return "idDIV"       if sym == '/'
        return "idMOD"       if sym == '%'
        return "idLT"        if sym == '<'
        return "idLTLT"      if sym == '<<'
        return "idLE"        if sym == '<='
        return "idGT"        if sym == '>'
        return "idGE"        if sym == '>='
        return "idEq"        if sym == '=='
        return "idNeq"       if sym == '!='
        return "idNot"       if sym == '!'
        return "idNot"       if sym == "not";
        return "idAREF"      if sym == "[]"
        return "idASET"      if sym == "[]="
        return "idLength"    if sym == "length"
        return "idSize"      if sym == "size"
        return "idEmptyP"    if sym == "empty?"
        return "idSucc"      if sym == "succ"
        return "idEqTilde"   if sym == "~="
        return "rb_intern(\"#{sym}\")"
    end

    def to_s
        "ci->mid == #{symbolize(@val)}"
    end
end

class Argc < Rule
    def initialize(val)
        super("", val)
    end
    def to_s
        "ci->argc == #{@val}"
    end
end

class Unredefined < Rule
    def initialize(bop, type, guard)
        super(bop, type, guard)
    end
    def to_s
        "Unredefined(#{@type}, \"#{@val}\")"
    end
    def emit_guard
        "GuardUnredefined(#{@type}, \"#{@val}\")"
    end
end

class Type < Rule
    def initialize(val, type, guard)
        super(type, val, guard)
    end

    def to_s
        p = "params[#{@val}]"
        if @type == :Fixnum
            return "FIXNUM_P(#{p})"
        elsif @type == :Float
            return "FLONUM_P(#{p})"
        elsif @type == :_
            return "1/*typeof #{p} is Any*/"
        else
            return "!SPECIAL_CONST_P(#{p}) && RBASIC_CLASS(#{p}) == rb_c#{@type}"
        end
    end
    def emit_guard
        p = "regs[#{@val}]"
        if @type == :Fixnum
            return "Emit_GuardTypeFixnum(Rec, pc, #{p})"
        elsif @type == :Float
            return "Emit_GuardTypeFloat(Rec, pc, #{p})"
        elsif @type == :_
            return ""
        else
            s  = "Emit_GuardTypeSpecialConst(Rec, pc, #{p}) && "
            s += "Emit_GuardType#{@type}(Rec, pc, #{p})"
            return s
        end
    end

end

def indent(i)
    i.times { print "  " }
end

def op(name, arg)
    i = 0
    puts "if ("
    puts arg.map {|a|
        "  (#{a.to_s})"
    }.join(" &&\n")
    puts ") {"
    arg.select {|r|
        r.need_guard == :guard
    }.each {|r|
        s = r.emit_guard
        if !s.empty?
            indent(i + 1);
            puts s + ";"
        end
    }
    indent(i + 1);
    puts "return EmitSpecialInst_#{name}(Rec, ci, regs);"
    indent(i)
    puts "}"
end

def opcode(name)
    Opcode.new(name)
end

def mtype(mname, guard = :default)
    Mtype.new(mname, guard)
end

def mid(mname)
    Mid.new(mname)
end

def argc(argc)
    Argc.new(argc)
end

def unredefined(bop, type, guard = :default)
    Unredefined.new(bop, type, guard)
end

def type(idx, type, guard = :default)
    Type.new(idx, type, guard)
end

DATA.each { |op|
    opname = op[0]
    yarvop = op[1]
    mname  = op[2]
    arg    = op[3]
    param = []
    param << opcode(yarvop)
    if mname == "#getter" || mname == "#setter"
        param << mtype(mname, :guard)
    else
        param << mid(mname)
    end

    param << argc(arg.length)
    if arg.length > 0 && mname != "#getter" && mname != "#setter"
        param << unredefined(arg[0].to_sym, mname, :guard)
    end

    arg.map.with_index { |v, i|
        if arg[i].to_s != :_
            param << type(i, v, :guard)
        else
            param << type(i, v)
        end
    }
    op(opname, param)
}
