/**********************************************************************

  jit_context.h -

  $Author$

  Copyright (C) 2014 Masahiro Ide

 **********************************************************************/

#ifndef GWJIT_CONTEXT_H
#define GWJIT_CONTEXT_H

typedef enum trace_exit_staus {
    TRACE_EXIT_ERROR = -1,
    TRACE_EXIT_SUCCESS = 0,
    TRACE_EXIT_SIDE_EXIT
} TraceExitStatus;

typedef struct gwjit_context {
    VALUE cArray;
    VALUE cFixnum;
    VALUE cFloat;
    VALUE cHash;
    VALUE cRegexp;
    VALUE cString;
    VALUE cTime;
    VALUE cSymbol;

    VALUE cFalseClass;
    VALUE cTrueClass;
    VALUE cNilClass;

    // ruby API
    // type check API
    VALUE (*_rb_check_array_type)(VALUE);
    VALUE (*_rb_big_plus)(VALUE, VALUE);
    VALUE (*_rb_big_minus)(VALUE, VALUE);
    VALUE (*_rb_big_mul)(VALUE, VALUE);
    VALUE (*_rb_int2big)(SIGNED_VALUE);
    VALUE (*_rb_str_length)(VALUE);
    VALUE (*_rb_str_plus)(VALUE, VALUE);
    VALUE (*_rb_str_append)(VALUE, VALUE);
    VALUE (*_rb_str_resurrect)(VALUE);
    VALUE (*_rb_range_new)(VALUE, VALUE, int);
    VALUE (*_rb_hash_new)();
    VALUE (*_rb_hash_aref)(VALUE, VALUE);
    VALUE (*_rb_hash_aset)(VALUE, VALUE, VALUE);
    VALUE (*_rb_reg_match)(VALUE, VALUE);
    VALUE (*_rb_reg_new_ary)(VALUE, int);
    VALUE (*_rb_ary_new)();
    VALUE (*_rb_ary_new_from_values)(long n, const VALUE *elts);
    VALUE (*_rb_gvar_get)(struct rb_global_entry *);
    VALUE (*_rb_gvar_set)(struct rb_global_entry *, VALUE);
    VALUE (*_rb_class_new_instance)(int argc, const VALUE *argv, VALUE klass);
    VALUE (*_rb_obj_as_string)(VALUE);

    // Internal ruby APIs
    double (*_ruby_float_mod)(double, double);
    VALUE (*_rb_float_new_in_heap)(double);
    VALUE (*_rb_ary_entry)(VALUE, long);
    void (*_rb_ary_store)(VALUE, long, VALUE);
    void (*_rb_gc_writebarrier)(VALUE a, VALUE b);
#if SIZEOF_INT < SIZEOF_VALUE
    NORETURN(void (*_rb_out_of_int)(SIGNED_VALUE num));
#endif
    NORETURN(void (*_rb_exc_raise)(VALUE));
    VALUE (*_make_no_method_exception)(VALUE exc, const char *format, VALUE obj,
                                       int argc, const VALUE *argv);

    // Internal data structure
    short *_ruby_vm_redefined_flag;
    short *_jit_vm_redefined_flag;
    struct rb_vm_struct *_ruby_current_vm;
    rb_serial_t *_ruby_vm_global_method_state;
} gwjit_context_t;

extern const gwjit_context_t *jit_context;

typedef VALUE (*gwjit_native_func0_t)();
typedef VALUE (*gwjit_native_func1_t)(VALUE);
typedef VALUE (*gwjit_native_func2_t)(VALUE, VALUE);
typedef VALUE (*gwjit_native_func3_t)(VALUE, VALUE, VALUE);
typedef VALUE (*gwjit_native_func4_t)(VALUE, VALUE, VALUE, VALUE);
typedef VALUE (*gwjit_native_func5_t)(VALUE, VALUE, VALUE, VALUE, VALUE);
typedef VALUE (*gwjit_native_func6_t)(VALUE, VALUE, VALUE, VALUE, VALUE, VALUE);

#define __int3__ asm volatile("int3")

#define JIT_OP_UNREDEFINED_P(op, klass) \
    (LIKELY((jit_vm_redefined_flag[(op)-JIT_BOP_LAST_] & (klass)) == 0))

#define OP_UNREDEFINED_P(op, klass)                       \
    ((op < BOP_LAST_) ? BASIC_OP_UNREDEFINED_P(op, klass) \
                      : JIT_OP_UNREDEFINED_P(op, klass))

#ifdef GWJIT_HOST
static VALUE make_no_method_exception(VALUE exc, const char *format, VALUE obj,
                                      int argc, const VALUE *argv)
{
    int n = 0;
    VALUE mesg;
    VALUE args[3];

    if (!format) {
        format = "undefined method `%s' for %s";
    }
    mesg = rb_const_get(exc, rb_intern("message"));
    if (rb_method_basic_definition_p(CLASS_OF(mesg), '!')) {
        args[n++]
            = rb_name_err_mesg_new(mesg, rb_str_new2(format), obj, argv[0]);
    } else {
        args[n++] = rb_funcall(mesg, '!', 3, rb_str_new2(format), obj, argv[0]);
    }
    args[n++] = argv[0];
    if (exc == rb_eNoMethodError) {
        args[n++] = rb_ary_new4(argc - 1, argv + 1);
    }
    return rb_class_new_instance(n, args, exc);
}
#endif /* defined(GWJIT_HOST) */

#endif /* end of include guard */
