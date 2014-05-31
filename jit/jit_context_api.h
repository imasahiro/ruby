/**********************************************************************

  jit_context_api.h -

  $Author$

  Copyright (C) 2014 Masahiro Ide

 **********************************************************************/

#include "jit_context.h"

#ifndef JIT_CONTEXT_API_H
#define JIT_CONTEXT_API_H

#define rb_cArray    (jit_context->cArray )
#define rb_cFixnum   (jit_context->cFixnum)
#define rb_cFloat    (jit_context->cFloat )
#define rb_cHash     (jit_context->cHash  )
#define rb_cRegexp   (jit_context->cRegexp)
#define rb_cString   (jit_context->cString)
#define rb_cTime     (jit_context->cTime)
#define rb_cSymbol   (jit_context->cSymbol)

#define rb_cTrueClass   (jit_context->cTrueClass  )
#define rb_cFalseClass  (jit_context->cFalseClass )
#define rb_cNilClass    (jit_context->cNilClass   )

#define rb_check_array_type (jit_context->_rb_check_array_type)
#define rb_big_plus    (jit_context->_rb_big_plus)
#define rb_big_minus   (jit_context->_rb_big_minus)
#define rb_big_mul     (jit_context->_rb_big_mul)
#define rb_int2big     (jit_context->_rb_int2big)
#define rb_str_length  (jit_context->_rb_str_length)
#define rb_range_new   (jit_context->_rb_range_new)
#define rb_hash_new    (jit_context->_rb_hash_new)
#define rb_hash_aref   (jit_context->_rb_hash_aref)
#define rb_hash_aset   (jit_context->_rb_hash_aset)
#define rb_reg_match   (jit_context->_rb_reg_match)
#define rb_ary_new     (jit_context->_rb_ary_new)
#define rb_ary_new_from_values (jit_context->_rb_ary_new_from_values)
#define rb_gvar_get    (jit_context->_rb_gvar_get)
#define rb_gvar_set    (jit_context->_rb_gvar_set)
#define rb_class_new_instance (jit_context->_rb_class_new_instance)

#define make_no_method_exception jit_context->_make_no_method_exception
#define rb_ary_entry           jit_context->_rb_ary_entry
#define rb_ary_store           jit_context->_rb_ary_store
#define ruby_float_mod         jit_context->_ruby_float_mod
#define rb_float_new_in_heap   jit_context->_rb_float_new_in_heap
#define ruby_vm_redefined_flag jit_context->_ruby_vm_redefined_flag

#define rb_gc_writebarrier     jit_context->_rb_gc_writebarrier
#define rb_exc_raise           jit_context->_rb_exc_raise
#define rb_out_of_int          jit_context->_rb_out_of_int
#define ruby_current_vm        jit_context->_ruby_current_vm

#undef CLASS_OF
#define CLASS_OF(O) jit_rb_class_of(O)

static inline VALUE
jit_rb_class_of(VALUE obj)
{
  if (IMMEDIATE_P(obj)) {
    if (FIXNUM_P(obj)) return rb_cFixnum;
    if (FLONUM_P(obj)) return rb_cFloat;
    if (obj == Qtrue)  return rb_cTrueClass;
    if (STATIC_SYM_P(obj)) return rb_cSymbol;
  }
  else if (!RTEST(obj)) {
    if (obj == Qnil)   return rb_cNilClass;
    if (obj == Qfalse) return rb_cFalseClass;
  }
  return RBASIC(obj)->klass;
}
#endif /* end of include guard */
