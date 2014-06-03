/**********************************************************************

  jit.h -

  $Author$

  Copyright (C) 2014 Masahiro Ide

 **********************************************************************/

#ifndef RUBY_JIT_H
#define RUBY_JIT_H 1

extern rb_serial_t *ruby_vm_global_method_state_ptr;
extern VALUE *rb_jit_trace(rb_thread_t *, rb_control_frame_t *, VALUE *);
extern void rb_jit_global_init();
extern void rb_jit_global_destruct();
extern void rb_jit_global_check_redefinition_opt_method();
extern void rb_jit_check_redefinition_opt_method(const rb_method_entry_t *me, VALUE klass);

#endif /* end of include guard */
