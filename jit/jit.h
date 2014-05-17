/**********************************************************************

  jit.h -

  $Author$

  Copyright (C) 2014 Masahiro Ide

 **********************************************************************/

#ifndef RUBY_JIT_H
#define RUBY_JIT_H 1

void jit_init();
extern rb_serial_t *ruby_vm_global_method_state_ptr;
VALUE *jit_trace(rb_thread_t *th, rb_control_frame_t *reg_cfp, VALUE *reg_pc);

#endif /* end of include guard */
