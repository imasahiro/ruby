/**********************************************************************

  record.c -

  $Author$

  Copyright (C) 2014 Masahiro Ide

 **********************************************************************/

static int IsLoopEdge(struct Trace *trace, VALUE *reg_pc, int op)
{
  switch(op) {
    case BIN(branchif):{
      return trace->StartPC == reg_pc;
    }
  }
  return 0;
}

/* original code is copied from vm_insnhelper.c vm_getivar() */
static int
vm_load_cache(VALUE obj, ID id, IC ic, rb_call_info_t *ci, int is_attr)
{
  VALUE val = Qundef;
  if (RB_TYPE_P(obj, T_OBJECT)) {
    VALUE klass = RBASIC(obj)->klass;
    st_data_t index;
    long len = ROBJECT_NUMIV(obj);
    VALUE *ptr = ROBJECT_IVPTR(obj);
    struct st_table *iv_index_tbl = ROBJECT_IV_INDEX_TBL(obj);

    if (iv_index_tbl) {
      if (st_lookup(iv_index_tbl, id, &index)) {
        if ((long)index < len) {
          val = ptr[index];
        }
        if (!is_attr) {
          ic->ic_value.index = index;
          ic->ic_serial = RCLASS_SERIAL(klass);
        }
        else { /* call_info */
          ci->aux.index = index + 1;
        }
        return 1;
      }
    }
  }
  return 0;
}

static inline int
check_cfunc(const rb_method_entry_t *me, VALUE (*func)())
{
  if (me && me->def->type == VM_METHOD_TYPE_CFUNC &&
      me->def->body.cfunc.func == func) {
    return 1;
  }
  else {
    return 0;
  }
}

#undef  GET_GLOBAL_CONSTANT_STATE
#define GET_GLOBAL_CONSTANT_STATE() (*ruby_vm_global_constant_state_ptr)

#define not_support_op(CFP, PC, OPNAME) do {\
  fprintf(stderr, "exit trace : not support bytecode: " OPNAME "\n");\
  disable_trace(CFP, PC, TRACE_ERROR_SUPPORT_OP);\
  return;\
} while(0)

#define EmitIR(OP, ...) Emit_##OP(builder, ## __VA_ARGS__)

#define _POP()       PopRegister(builder)
#define _PUSH(REG)   PushRegister(builder, REG)
#define _TOPN(N)     TopRegister(builder, (N))

static reg_t EmitConverter(lir_builder_t *builder, VALUE val, reg_t Rval, VALUE *reg_pc, int type) {
  if (FIXNUM_P(val)) {
    EmitIR(GuardTypeFixnum, reg_pc, Rval);
    switch (type) {
      case T_FIXNUM:
        return Rval;
      case T_FLOAT:
        return EmitIR(FloatToFixnum, Rval);
      case T_STRING:
        return EmitIR(StringToFixnum, Rval);
    }
  }
  else if (FLONUM_P(val)) {
    EmitIR(GuardTypeFlonum, reg_pc, Rval);
    switch (type) {
      case T_FIXNUM:
        return EmitIR(FixnumToFloat, Rval);
      case T_FLOAT:
        return Rval;
      case T_STRING:
        return EmitIR(StringToFloat, Rval);
    }
  }
  else if (!SPECIAL_CONST_P(val) && RBASIC_CLASS(val) == rb_cString) {
    EmitIR(GuardTypeString, reg_pc, Rval);
    switch (type) {
      case T_FIXNUM:
        return EmitIR(FixnumToString, Rval);
      case T_FLOAT:
        return EmitIR(FloatToString, Rval);
      case T_STRING:
        return Rval;
    }
  }
  return -1;
}

static reg_t EmitLoadConst(lir_builder_t *builder, VALUE val)
{
  reg_t Rval;
  if (FIXNUM_P(val)) {
    Rval = EmitIR(LoadConstFixnum, val);
  }
  else if (FLONUM_P(val)) {
    Rval = EmitIR(LoadConstFloat, val);
  }
  else if (!SPECIAL_CONST_P(val) && RBASIC_CLASS(val) == rb_cString) {
    Rval = EmitIR(LoadConstString, val);
  }
  else {
    Rval = EmitIR(LoadConstObject, val);
  }
  return Rval;
}

static void EmitMethodCall(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc, CALL_INFO ci, rb_block_t *block, reg_t Rblock)
{
  reg_t Rrecv, Rval = -1;
  int i, argc = ci->argc + 1/*recv*/;
  int cacheable = 0;
  reg_t argv[argc];
  VALUE obj = TOPN(ci->argc);

  vm_search_method(ci, ci->recv = TOPN(ci->argc));

  // check method type
  if(ci->me) {
    // getter method
    if(ci->me->def->type == VM_METHOD_TYPE_IVAR) {
      goto emit_attribute;
    }
    // setter method
    if(ci->me->def->type == VM_METHOD_TYPE_ATTRSET) {
      goto emit_attribute;
    }
    // user defined ruby method
    if (ci->me->def->type == VM_METHOD_TYPE_ISEQ) {
      goto emit_push_frame;
    }
  }

  // check Math method
  if (ci->me && ci->me->def->type == VM_METHOD_TYPE_CFUNC) {
    VALUE cMath = rb_singleton_class(rb_mMath);
    if (cMath == ci->me->klass) {
      goto emit_math_api;
    }
  }

  // I think this method is c-defined method.
  // abort trace compilation
  disable_trace(reg_cfp, reg_pc, TRACE_ERROR_NATIVE_METHOD);
  return;

emit_push_frame:
  builder->CallDepth += 1;
  for (i = 0; i < argc; i++) {
    argv[i] = _TOPN(ci->argc - i);
  }
  EmitIR(GuardMethodCache, reg_pc, argv[0], ci);
  if (Rblock) {
    EmitIR(GuardObjectEqual, reg_pc, Rblock, (VALUE) block);
  }
  PushCallStack(builder, argc, argv);
  _PUSH(EmitIR(FramePush, ci, 0, Rblock, argc, argv));
  return;

emit_attribute:
  if (ci->argc == 1) {
    Rval  = _POP();
    Rrecv = _POP();
  }
  else {
    Rrecv = _POP();
  }

  cacheable = vm_load_cache(obj, ci->me->def->body.attr.id, 0, ci, 1);
  assert(ci->aux.index > 0 && cacheable);
  EmitIR(GuardTypeObject, reg_pc, Rrecv);
  EmitIR(GuardProperty, reg_pc, Rrecv, 1/*is_attr*/, (void *)ci);
  if (ci->argc == 0) {
    _PUSH(EmitIR(GetPropertyName, Rrecv, ci->aux.index - 1));
  }
  else {
    assert(ci->argc == 1);
    _PUSH(EmitIR(SetPropertyName, Rrecv, ci->aux.index - 1, Rval));
  }
  return;

emit_math_api:
  if(ci->orig_argc == 1) {
    Rrecv = _POP();
    obj   = TOPN(0);
    if (EmitConverter(builder, obj, Rrecv, reg_pc, T_FLOAT) == -1) {
      fprintf(stderr, "unsupported converter typeof(recv) => T_FLOAT\n");
      disable_trace(reg_cfp, reg_pc, TRACE_ERROR_NATIVE_METHOD);
      return;
    }

    if(ci->mid == rb_intern("exp")) {
      _PUSH(EmitIR(MathExp, Rrecv));
      return;
    }
    if(ci->mid == rb_intern("log2")) {
      _PUSH(EmitIR(MathLog2, Rrecv));
      return;
    }
    if(ci->mid == rb_intern("log10")) {
      _PUSH(EmitIR(MathLog10, Rrecv));
      return;
    }
    if(ci->mid == rb_intern("sqrt")) {
      _PUSH(EmitIR(MathSqrt, Rrecv));
      return;
    }
    if(ci->mid == rb_intern("sin")) {
      _PUSH(EmitIR(MathSin, Rrecv));
      return;
    }
    if(ci->mid == rb_intern("cos")) {
      _PUSH(EmitIR(MathCos, Rrecv));
      return;
    }
    if(ci->mid == rb_intern("tan")) {
      _PUSH(EmitIR(MathTan, Rrecv));
      return;
    }
  }
  // unsupported math api
  fprintf(stderr, "unsupported math api\n");
  disable_trace(reg_cfp, reg_pc, TRACE_ERROR_NATIVE_METHOD);
  return;
}

static void _record_getlocal(lir_builder_t *builder, rb_num_t level, lindex_t idx)
{
  reg_t Rval = EmitIR(StackLoad, (int) level, (int) idx);
  _PUSH(Rval);
}

static void _record_setlocal(lir_builder_t *builder, rb_num_t level, lindex_t idx)
{
  reg_t Rval = _POP();
  EmitIR(StackStore, (int) level, (int) idx, Rval);
}

// record API

static void record_nop(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  /* do nothing */
}

static void record_getlocal(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  rb_num_t level = (rb_num_t)GET_OPERAND(2);
  lindex_t idx = (lindex_t)GET_OPERAND(1);
  _record_getlocal(builder, level, idx);
}

static void record_setlocal(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  rb_num_t level = (rb_num_t)GET_OPERAND(2);
  lindex_t idx = (lindex_t)GET_OPERAND(1);
  _record_setlocal(builder, level, idx);
}

static void record_getspecial(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  not_support_op(reg_cfp, reg_pc, "getspecial");
}

static void record_setspecial(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  not_support_op(reg_cfp, reg_pc, "setspecial");
}

static void record_getinstancevariable(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  IC ic = (IC)GET_OPERAND(2);
  ID id = (ID)GET_OPERAND(1);
  VALUE obj = GET_SELF();
  reg_t Rrecv = EmitIR(LoadSelf);

  int cacheable = vm_load_cache(obj, id, ic, NULL, 0);
  if (cacheable) {
    TakeStackSnapshot(builder, reg_pc);
    EmitIR(GuardTypeObject, reg_pc, Rrecv);
    EmitIR(GuardProperty, reg_pc, Rrecv, 0/*!is_attr*/, (void *) ic);
    _PUSH(EmitIR(GetPropertyName, Rrecv, ic->ic_value.index));
    return;
  }
  not_support_op(reg_cfp, reg_pc, "getinstancevariable");
}

static void record_setinstancevariable(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  IC ic = (IC)GET_OPERAND(2);
  ID id = (ID)GET_OPERAND(1);
  VALUE obj = GET_SELF();
  reg_t Rrecv = EmitIR(LoadSelf);

  int cacheable = vm_load_cache(obj, id, ic, NULL, 0);
  if (cacheable) {
    TakeStackSnapshot(builder, reg_pc);
    EmitIR(GuardTypeObject, reg_pc, Rrecv);
    EmitIR(GuardProperty, reg_pc, Rrecv, 0/*!is_attr*/, (void *) ic);
    EmitIR(SetPropertyName, Rrecv, ic->ic_value.index, _POP());
    return;
  }

  not_support_op(reg_cfp, reg_pc, "setinstancevariable");
}

static void record_getclassvariable(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  not_support_op(reg_cfp, reg_pc, "getclassvariable");
}

static void record_setclassvariable(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  not_support_op(reg_cfp, reg_pc, "setclassvariable");
}

static void record_getconstant(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  not_support_op(reg_cfp, reg_pc, "getconstant");
}

static void record_setconstant(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  not_support_op(reg_cfp, reg_pc, "setconstant");
}

static void record_getglobal(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  GENTRY entry = (GENTRY) GET_OPERAND(1);
  reg_t Id = EmitLoadConst(builder, entry);
  _PUSH(EmitIR(GetGlobal, Id));
}

static void record_setglobal(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  GENTRY entry = (GENTRY) GET_OPERAND(1);
  reg_t Rval = _POP();
  reg_t Id = EmitLoadConst(builder, entry);
  EmitIR(SetGlobal, Id, Rval);
}

static void record_putnil(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  _PUSH(EmitIR(LoadConstNil));
}

static void record_putself(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  _PUSH(EmitIR(LoadSelf));
}

static void record_putobject(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  VALUE val = (VALUE) GET_OPERAND(1);
  _PUSH(EmitLoadConst(builder, val));
}

/* copied from vm_insnhelper.c */
extern NODE *rb_vm_get_cref(const rb_iseq_t *iseq, const VALUE *ep);
static inline VALUE
jit_vm_get_cbase(const rb_iseq_t *iseq, const VALUE *ep)
{
  NODE *cref = rb_vm_get_cref(iseq, ep);
  VALUE klass = Qundef;

  while (cref) {
    if ((klass = cref->nd_clss) != 0) {
      break;
    }
    cref = cref->nd_next;
  }

  return klass;
}

static inline VALUE
jit_vm_get_const_base(const rb_iseq_t *iseq, const VALUE *ep)
{
    NODE *cref = rb_vm_get_cref(iseq, ep);
    VALUE klass = Qundef;

    while (cref) {
	if (!(cref->flags & NODE_FL_CREF_PUSHED_BY_EVAL) &&
	    (klass = cref->nd_clss) != 0) {
	    break;
	}
	cref = cref->nd_next;
    }

    return klass;
}

static void record_putspecialobject(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  enum vm_special_object_type type = (enum vm_special_object_type) GET_OPERAND(1);
  VALUE val = 0;
  switch (type) {
    case VM_SPECIAL_OBJECT_VMCORE:
      val = rb_mRubyVMFrozenCore;
      break;
    case VM_SPECIAL_OBJECT_CBASE:
      val = jit_vm_get_cbase(GET_ISEQ(), GET_EP());
      break;
    case VM_SPECIAL_OBJECT_CONST_BASE:
      val = jit_vm_get_const_base(GET_ISEQ(), GET_EP());
      break;
    default:
      rb_bug("putspecialobject insn: unknown value_type");
  }
  _PUSH(EmitLoadConst(builder, val));
}

static void record_putiseq(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  ISEQ iseq = (ISEQ) GET_OPERAND(1);
  _PUSH(EmitLoadConst(builder, iseq->self));
}

static void record_putstring(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  VALUE val  = (VALUE) GET_OPERAND(1);
  reg_t argv[] = {EmitIR(LoadConstString, val)};
  reg_t Rval   = EmitIR(InvokeNative, rb_str_resurrect, 1, argv);
  _PUSH(Rval);
}

static void record_concatstrings(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  rb_num_t num = (rb_num_t)GET_OPERAND(1);
  rb_num_t i = num - 1;

  reg_t argv[] = {_TOPN(i), _TOPN(i)};
  reg_t Rval;
  argv[0] = EmitIR(InvokeNative, rb_str_resurrect, 1, argv);

  while (i-- > 0) {
    argv[1] = _TOPN(i);
    Rval = EmitIR(InvokeNative, rb_str_append, 2, argv);
  }
  for (i = 0; i < num; i++) {
    _POP();
  }
  _PUSH(Rval);
}

static void record_tostring(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  reg_t argv[] = {_POP()};
  reg_t Rval = EmitIR(InvokeNative, rb_obj_as_string, 1, argv);
  _PUSH(Rval);
}

static void record_toregexp(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  not_support_op(reg_cfp, reg_pc, "toregexp");
}

static void record_newarray(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  rb_num_t i, num = (rb_num_t)GET_OPERAND(1);
  reg_t argv[num];
  for (i = 0; i < num; i++) {
    argv[i] = _POP();
  }
  _PUSH(EmitIR(AllocArray, (int) num, argv));
}

static void record_duparray(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  VALUE val = (VALUE) GET_OPERAND(1);
  reg_t Rval = EmitLoadConst(builder, val);
  reg_t argv[] = {Rval};
  _PUSH(EmitIR(InvokeNative, rb_ary_resurrect, 1, argv));
}

static void record_expandarray(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  not_support_op(reg_cfp, reg_pc, "expandarray");
}

static void record_concatarray(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  not_support_op(reg_cfp, reg_pc, "concatarray");
}

static void record_splatarray(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  not_support_op(reg_cfp, reg_pc, "splatarray");
}

static void record_newhash(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  rb_num_t i, num = (rb_num_t) GET_OPERAND(1);
  reg_t argv[num];
  for (i = num; i > 0; i -= 2) {
    argv[i - 1] = _POP(); // key
    argv[i - 2] = _POP(); // val
  }
  _PUSH(EmitIR(AllocHash, (int) num, argv));
}

static void record_newrange(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  rb_num_t flag = (rb_num_t) GET_OPERAND(1);
  reg_t Rhigh = _POP();
  reg_t Rlow  = _POP();
  _PUSH(EmitIR(AllocRange, Rlow, Rhigh, (int) flag));
}

static void record_pop(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  _POP();
}

static void record_dup(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  reg_t Rval = _POP();
  _PUSH(Rval);
  _PUSH(Rval);
}

static void record_dupn(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  not_support_op(reg_cfp, reg_pc, "dupn");
}

static void record_swap(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  reg_t Rval = _POP();
  reg_t Robj = _POP();
  _PUSH(Robj);
  _PUSH(Rval);
}

static void record_reput(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  _PUSH(_POP());
}

static void record_topn(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  not_support_op(reg_cfp, reg_pc, "topn");
  //rb_num_t n = (rb_num_t)GET_OPERAND(1);
  //asm volatile("int3"); // need test
  //reg_t Rval = _TOPN(n);
  //_PUSH(Rval);
}

static void record_setn(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  not_support_op(reg_cfp, reg_pc, "setn");
  //rb_num_t n = (rb_num_t)GET_OPERAND(1);
  //asm volatile("int3"); // need test
  //reg_t Rval = _POP();
  //_SET(n - 1, Rval);
  //_PUSH(Rval);
}

static void record_adjuststack(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  rb_num_t i, n = (rb_num_t)GET_OPERAND(1);
  for (i = 0; i < n; i++) {
    _POP();
  }
}

static void record_defined(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  not_support_op(reg_cfp, reg_pc, "defined");
}

static void record_checkmatch(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  not_support_op(reg_cfp, reg_pc, "checkmatch");
}

static void record_trace(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  rb_event_flag_t flag = (rb_event_flag_t) GET_OPERAND(1);
  EmitIR(Trace, flag);
}

static void record_defineclass(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  not_support_op(reg_cfp, reg_pc, "defineclass");
}

static void record_send(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  CALL_INFO ci = (CALL_INFO) GET_OPERAND(1);
  reg_t Rblock = 0;
  rb_block_t *block = NULL;
  ci->argc = ci->orig_argc;
  ci->blockptr = 0;
  //vm_caller_setup_args(th, reg_cfp, ci);
  if (UNLIKELY(ci->flag & VM_CALL_ARGS_BLOCKARG)) {
    not_support_op(reg_cfp, reg_pc, "send");
    return;
  }
  else if (ci->blockiseq != 0) {
    Rblock = EmitIR(LoadSelfAsBlock);
    block  = RUBY_VM_GET_BLOCK_PTR_IN_CFP(reg_cfp);
  }

  if (UNLIKELY(ci->flag & VM_CALL_ARGS_SPLAT)) {
    not_support_op(reg_cfp, reg_pc, "send");
    return;
  }

  TakeStackSnapshot(builder, reg_pc);
  EmitMethodCall(builder, reg_cfp, reg_pc, ci, block, Rblock);
}

static void record_opt_str_freeze(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  if (BASIC_OP_UNREDEFINED_P(BOP_FREEZE, STRING_REDEFINED_OP_FLAG)) {
    TakeStackSnapshot(builder, reg_pc);
    EmitIR(GuardMethodRedefine, reg_pc, rb_cString, BOP_FREEZE);
    _PUSH(_POP());
  }
  else {
    not_support_op(reg_cfp, reg_pc, "opt_str_freeze");
  }
}

static void record_opt_send_simple(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  CALL_INFO ci = (CALL_INFO) GET_OPERAND(1);
  TakeStackSnapshot(builder, reg_pc);
  EmitMethodCall(builder, reg_cfp, reg_pc, ci, 0, 0);
}

static void record_invokesuper(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  not_support_op(reg_cfp, reg_pc, "invokesuper");
}

static void record_invokeblock(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  CALL_INFO ci = (CALL_INFO) GET_OPERAND(1);
  int i, argc = 1/*recv*/ + ci->orig_argc;
  reg_t Rblock, argv[argc];
  TakeStackSnapshot(builder, reg_pc);

  argv[0] = EmitIR(LoadSelf);
  Rblock  = EmitIR(LoadBlock);

  ci->argc = ci->orig_argc;
  ci->blockptr = 0;
  ci->recv = GET_SELF();

  const rb_block_t *block = rb_vm_control_frame_block_ptr(reg_cfp);
  VALUE type = GET_ISEQ()->local_iseq->type;

  if ((type != ISEQ_TYPE_METHOD && type != ISEQ_TYPE_CLASS) || block == 0) {
    // "no block given (yield)"
    disable_trace(reg_cfp, reg_pc, TRACE_ERROR_THROW);
    return;
  }

  if (UNLIKELY(ci->flag & VM_CALL_ARGS_SPLAT)) {
    disable_trace(reg_cfp, reg_pc, TRACE_ERROR_SUPPORT_OP);
    return;
  }

  if (BUILTIN_TYPE(block->iseq) == T_NODE) {
    // yield native block
    disable_trace(reg_cfp, reg_pc, TRACE_ERROR_NATIVE_METHOD);
    return;
  }

  builder->CallDepth += 1;
  for (i = 0; i < ci->orig_argc; i++) {
    argv[i + 1] = _TOPN(ci->orig_argc - i - 1);
  }

  // XXX
  // In opt_send_simple, argv[0] is already pushed but invokeblock is not.
  // This code is needed for adjusting register stack.
  _PUSH(argv[0]);

  EmitIR(GuardObjectEqual, reg_pc, Rblock, (VALUE) block);
  PushCallStack(builder, argc, argv);
  _PUSH(EmitIR(FramePush, ci, 1, Rblock, argc, argv));
}

static void record_leave(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  if(VM_FRAME_TYPE_FINISH_P(reg_cfp)) {
    disable_trace(reg_cfp, reg_pc, TRACE_ERROR_LEAVE);
    return;
  }
  if (builder->CallDepth == 0) {
    disable_trace(reg_cfp, reg_pc, TRACE_ERROR_LEAVE);
    return;
  }
  builder->CallDepth -= 1;
  reg_t Val = _POP();
  PopCallStack(builder);

  EmitIR(FramePop);
  _PUSH(Val);
}

static void record_throw(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  not_support_op(reg_cfp, reg_pc, "throw");
}

static void record_jump(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  OFFSET dst = (OFFSET) GET_OPERAND(1);
  VALUE* TargetPC = reg_pc + insn_len(BIN(jump)) + dst;
  EmitIR(Jump, TargetPC);
  if (FindBasicBlockByPC(builder, TargetPC) == NULL) {
    CreateBlock(builder, TargetPC);
  }
}

static void record_branchif(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  struct Trace *trace = compiler_info->Current;
  OFFSET dst = (OFFSET)GET_OPERAND(1);
  reg_t Rval = _POP();
  VALUE val  = TOPN(0);
  VALUE *NextPC = reg_pc + insn_len(BIN(branchif));
  VALUE *JumpPC = NextPC + dst;

  if (RTEST(val)) {
    TakeStackSnapshot(builder, NextPC);
    EmitIR(GuardTypeNil, NextPC, Rval);
    EmitIR(Jump,  JumpPC);
    if(FindBasicBlockByPC(builder, JumpPC) == NULL) {
      CreateBlock(builder, JumpPC);
    }

  }
  else {
    TakeStackSnapshot(builder, JumpPC);
    EmitIR(GuardTypeNil, JumpPC, Rval);
    EmitIR(Jump,  NextPC);
    if(FindBasicBlockByPC(builder, NextPC) == NULL) {
      CreateBlock(builder, NextPC);
    }
  }

  if (IsLoopEdge(trace, reg_pc, BIN(branchif))) {
    compiler_info->Current = trace->Parent;
    compiler_info->Parent  = (trace->Parent) ? trace->Parent->Parent : NULL;
    disable_trace(NULL, NULL, TRACE_OK);
    trace->Code = FlushLIRBuilder(builder);
    if (0 && trace->Code == NULL) {
      // FIXME we need to implement `bloacklist`
      hashmap_remove(&compiler_info->Traces, reg_pc);
    }
    return;
  }
}

static void record_branchunless(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  OFFSET dst  = (OFFSET) GET_OPERAND(1);
  reg_t Rval  = _POP();
  VALUE val   = TOPN(0);
  VALUE *NextPC = reg_pc + insn_len(BIN(branchunless));
  VALUE *JumpPC = NextPC + dst;
  VALUE *TargetPC = NULL;

  if (!RTEST(val)) {
    TakeStackSnapshot(builder, NextPC);
    EmitIR(GuardTypeNonNil, NextPC, Rval);
    TargetPC = JumpPC;
  }
  else {
    TakeStackSnapshot(builder, JumpPC);
    EmitIR(GuardTypeNil, JumpPC, Rval);
    TargetPC = NextPC;
  }

  EmitIR(Jump,  TargetPC);
  if(FindBasicBlockByPC(builder, TargetPC) == NULL) {
    CreateBlock(builder, TargetPC);
  }

}

static void record_getinlinecache(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  IC ic = (IC)GET_OPERAND(2);
  if (ic->ic_serial != GET_GLOBAL_CONSTANT_STATE()) {
    // hmm, constant value is re-defined.
    not_support_op(reg_cfp, reg_pc, "getinlinecache");
    return;
  }
  _PUSH(EmitLoadConst(builder, ic->ic_value.value));
}

static void record_setinlinecache(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  not_support_op(reg_cfp, reg_pc, "setinlinecache");
}

static void record_once(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  not_support_op(reg_cfp, reg_pc, "once");
}

static void record_opt_case_dispatch(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  not_support_op(reg_cfp, reg_pc, "opt_case_dispatch");
}

#define _record_binary(builder, reg_cfp, reg_pc, bop, opname) do {\
  VALUE recv, obj;\
  reg_t Robj, Rrecv, Rval;\
  CALL_INFO ci;\
  TakeStackSnapshot(builder, reg_pc);\
  ci   = (CALL_INFO)GET_OPERAND(1);\
  recv = TOPN(1);\
  obj  = TOPN(0);\
  Robj  = _POP();\
  Rrecv = _POP();\
  reg_t params[] = {Rrecv, Robj};\
  if (FIXNUM_2_P(recv, obj) &&\
      BASIC_OP_UNREDEFINED_P(bop, FIXNUM_REDEFINED_OP_FLAG)) {\
    EmitIR(GuardTypeFixnum, reg_pc, Rrecv);\
    EmitIR(GuardTypeFixnum, reg_pc, Robj);\
    EmitIR(GuardMethodRedefine, reg_pc, rb_cFixnum, bop);\
    Rval = EmitIR(Fixnum##opname##Overflow, Rrecv, Robj);\
  }\
  else if (FLONUM_2_P(recv, obj) &&\
           BASIC_OP_UNREDEFINED_P(bop, FLOAT_REDEFINED_OP_FLAG)) {\
    EmitIR(GuardTypeFlonum, reg_pc, Rrecv);\
    EmitIR(GuardTypeFlonum, reg_pc, Robj );\
    EmitIR(GuardMethodRedefine, reg_pc, rb_cFloat, bop);\
    Rval = EmitIR(Float##opname, Rrecv, Robj);\
  }\
  else if (!SPECIAL_CONST_P(recv) && !SPECIAL_CONST_P(obj)) {\
    EmitIR(GuardTypeSpecialConst, reg_pc, Rrecv);\
    EmitIR(GuardTypeSpecialConst, reg_pc, Robj );\
    VALUE recv_klass = RBASIC_CLASS(recv);\
    VALUE obj_klass  = RBASIC_CLASS(obj);\
    if (recv_klass == rb_cFloat && obj_klass ==  rb_cFloat &&\
        BASIC_OP_UNREDEFINED_P(bop, FLOAT_REDEFINED_OP_FLAG)) {\
      EmitIR(GuardTypeFloat, reg_pc, Rrecv);\
      EmitIR(GuardTypeFloat, reg_pc, Robj );\
      EmitIR(GuardMethodRedefine, reg_pc, rb_cFloat, bop);\
      Rval = EmitIR(Float##opname, Rrecv, Robj);\
    }\
    else if (bop == BOP_PLUS &&\
             recv_klass == rb_cString && obj_klass == rb_cString &&\
             BASIC_OP_UNREDEFINED_P(bop, STRING_REDEFINED_OP_FLAG)) {\
      EmitIR(GuardTypeString, reg_pc, Rrecv);\
      EmitIR(GuardTypeString, reg_pc, Robj );\
      EmitIR(GuardMethodRedefine, reg_pc, rb_cString, bop);\
      Rval = EmitIR(InvokeNative, rb_str_plus, 2, params);\
    }\
    else if (bop == BOP_PLUS &&\
             recv_klass == rb_cArray &&\
             BASIC_OP_UNREDEFINED_P(bop, ARRAY_REDEFINED_OP_FLAG)) {\
      EmitIR(GuardTypeArray, reg_pc, Rrecv);\
      EmitIR(GuardMethodRedefine, reg_pc, rb_cArray, bop);\
      Rval = EmitIR(InvokeNative, rb_ary_plus, 2, params);\
    }\
    else {\
      goto normal_dispatch;\
    }\
  }\
  else {\
    normal_dispatch:\
    vm_search_method(ci, recv);\
    EmitIR(GuardMethodCache, reg_pc, params[0], ci);\
    Rval = EmitIR(InvokeMethod, ci, 2, params);\
  }\
  _PUSH(Rval);\
} while(0)

#define _record_cond(builder, reg_cfp, reg_pc, bop, opname) do {\
  VALUE recv, obj;\
  reg_t Robj, Rrecv, Rval;\
  TakeStackSnapshot(builder, reg_pc);\
  recv = TOPN(1);\
  obj  = TOPN(0);\
  Robj  = _POP();\
  Rrecv = _POP();\
  if (FIXNUM_2_P(recv, obj) &&\
      BASIC_OP_UNREDEFINED_P(bop, FIXNUM_REDEFINED_OP_FLAG)) {\
    EmitIR(GuardTypeFixnum, reg_pc, Rrecv);\
    EmitIR(GuardTypeFixnum, reg_pc, Robj );\
    EmitIR(GuardMethodRedefine, reg_pc, rb_cFixnum, bop);\
    Rval = EmitIR(Fixnum##opname, Rrecv, Robj);\
  }\
  else if (FLONUM_2_P(recv, obj) &&\
           BASIC_OP_UNREDEFINED_P(bop, FLOAT_REDEFINED_OP_FLAG)) {\
    EmitIR(GuardTypeFlonum, reg_pc, Rrecv);\
    EmitIR(GuardTypeFlonum, reg_pc, Robj );\
    EmitIR(GuardMethodRedefine, reg_pc, rb_cFloat, bop);\
    Rval = EmitIR(Float##opname, Rrecv, Robj);\
  }\
  else {\
    CALL_INFO ci = (CALL_INFO) GET_OPERAND(1);\
    reg_t params[] = {Rrecv, Robj};\
    vm_search_method(ci, recv);\
    EmitIR(GuardMethodCache, reg_pc, params[0], ci);\
    Rval = EmitIR(InvokeMethod, ci, 2, params);\
  }\
  _PUSH(Rval);\
} while(0)

static void record_opt_plus(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  _record_binary(builder, reg_cfp, reg_pc, BOP_PLUS, Add);
}

static void record_opt_minus(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  _record_binary(builder, reg_cfp, reg_pc, BOP_MINUS, Sub);
}

static void record_opt_mult(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  _record_binary(builder, reg_cfp, reg_pc, BOP_MULT, Mul);
}

static void record_opt_div(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  _record_binary(builder, reg_cfp, reg_pc, BOP_DIV, Div);
}

static void record_opt_mod(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  _record_binary(builder, reg_cfp, reg_pc, BOP_MOD, Mod);
}

static void record_opt_eq(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  _record_cond(builder, reg_cfp, reg_pc, BOP_EQ, Eq);
}

static void record_opt_neq(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  _record_cond(builder, reg_cfp, reg_pc, BOP_NEQ, Ne);
}

static void record_opt_lt(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  _record_cond(builder, reg_cfp, reg_pc, BOP_LT, Lt);
}

static void record_opt_le(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  _record_cond(builder, reg_cfp, reg_pc, BOP_LE, Le);
}

static void record_opt_gt(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  _record_cond(builder, reg_cfp, reg_pc, BOP_GT, Gt);
}

static void record_opt_ge(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  _record_cond(builder, reg_cfp, reg_pc, BOP_GE, Ge);
}

static void record_opt_ltlt(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  VALUE recv, obj;
  reg_t Robj, Rrecv, Rval;
  CALL_INFO ci;
  TakeStackSnapshot(builder, reg_pc);
  ci   = (CALL_INFO)GET_OPERAND(1);
  recv = TOPN(1);
  obj  = TOPN(0);
  Robj  = _POP();
  Rrecv = _POP();
  reg_t params[] = {Rrecv, Robj};

  if (!SPECIAL_CONST_P(recv) && !SPECIAL_CONST_P(obj)) {
    EmitIR(GuardTypeSpecialConst, reg_pc, Rrecv);
    EmitIR(GuardTypeSpecialConst, reg_pc, Robj );
    VALUE recv_klass = RBASIC_CLASS(recv);
    VALUE obj_klass  = RBASIC_CLASS(obj);
    if (recv_klass == rb_cString && obj_klass == rb_cString &&
        BASIC_OP_UNREDEFINED_P(BOP_LTLT, STRING_REDEFINED_OP_FLAG)) {
      EmitIR(GuardTypeString, reg_pc, Rrecv);
      EmitIR(GuardTypeString, reg_pc, Robj );
      EmitIR(GuardMethodRedefine, reg_pc, rb_cString, BOP_LTLT);
      Rval = EmitIR(InvokeNative, rb_str_plus, 2, params);
    }
    else if(recv_klass == rb_cArray &&
            BASIC_OP_UNREDEFINED_P(BOP_LTLT, ARRAY_REDEFINED_OP_FLAG)) {
      EmitIR(GuardTypeArray, reg_pc, Rrecv);
      EmitIR(GuardMethodRedefine, reg_pc, rb_cArray, BOP_LTLT);
      Rval = EmitIR(InvokeNative, rb_ary_plus, 2, params);
    }
    else {
      goto normal_dispatch;
    }
  }
  else {
normal_dispatch:
    vm_search_method(ci, recv);
    EmitIR(GuardMethodCache, reg_pc, params[0], ci);
    Rval = EmitIR(InvokeMethod, ci, 2, params);
  }
  _PUSH(Rval);
}

static void record_opt_aref(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  VALUE recv, obj;
  reg_t Robj, Rrecv, Rval;
  CALL_INFO ci;
  TakeStackSnapshot(builder, reg_pc);
  ci   = (CALL_INFO)GET_OPERAND(1);
  recv = TOPN(1);
  obj  = TOPN(0);
  Robj  = _POP();
  Rrecv = _POP();
  reg_t params[] = {Rrecv, Robj};

  if (!SPECIAL_CONST_P(recv)) {
    EmitIR(GuardTypeSpecialConst, reg_pc, Rrecv);
    EmitIR(GuardTypeSpecialConst, reg_pc, Robj );
    VALUE recv_klass = RBASIC_CLASS(recv);
    if(recv_klass == rb_cArray && FIXNUM_P(obj) &&
       BASIC_OP_UNREDEFINED_P(BOP_AREF, ARRAY_REDEFINED_OP_FLAG)) {
      EmitIR(GuardTypeArray, reg_pc,  Rrecv);
      EmitIR(GuardTypeFixnum, reg_pc, Robj);
      EmitIR(GuardMethodRedefine, reg_pc, rb_cArray, BOP_AREF);
      Rval = EmitIR(ArrayGet, Rrecv, Robj);
      Rval = EmitIR(InvokeNative, rb_ary_entry, 2, params);
    }
    else if(recv_klass == rb_cHash &&
            BASIC_OP_UNREDEFINED_P(BOP_AREF, ARRAY_REDEFINED_OP_FLAG)) {
      EmitIR(GuardTypeHash, reg_pc, Rrecv);
      EmitIR(GuardMethodRedefine, reg_pc, rb_cHash, BOP_AREF);
      Rval = EmitIR(HashGet, Rrecv, Robj);
    }
    else {
      goto normal_dispatch;
    }
  }
  else {
normal_dispatch:
    vm_search_method(ci, recv);
    EmitIR(GuardMethodCache, reg_pc, params[0], ci);
    Rval = EmitIR(InvokeMethod, ci, 2, params);
  }
  _PUSH(Rval);

}

static void record_opt_aset(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  VALUE recv, obj, set;
  reg_t Robj, Rrecv, Rset, Rval;
  CALL_INFO ci;
  TakeStackSnapshot(builder, reg_pc);
  ci    = (CALL_INFO)GET_OPERAND(1);
  recv  = TOPN(2);
  obj   = TOPN(1);
  set   = TOPN(0);
  Rset  = _POP();
  Robj  = _POP();
  Rrecv = _POP();
  reg_t params[] = {Rrecv, Robj, Rset};

  if (!SPECIAL_CONST_P(recv)) {
    EmitIR(GuardTypeSpecialConst, reg_pc, Rrecv);
    VALUE recv_klass = RBASIC_CLASS(recv);
    if(recv_klass == rb_cArray && FIXNUM_P(obj) &&
       BASIC_OP_UNREDEFINED_P(BOP_ASET, ARRAY_REDEFINED_OP_FLAG)) {
      EmitIR(GuardTypeArray, reg_pc,  Rrecv);
      EmitIR(GuardTypeFixnum, reg_pc, Robj);
      EmitIR(GuardMethodRedefine, reg_pc, rb_cArray, BOP_ASET);
      Rval = EmitIR(ArraySet, Rrecv, Robj, Rset);
    }
    else if(recv_klass == rb_cHash &&
            BASIC_OP_UNREDEFINED_P(BOP_ASET, ARRAY_REDEFINED_OP_FLAG)) {
      EmitIR(GuardTypeHash, reg_pc, Rrecv);
      EmitIR(GuardMethodRedefine, reg_pc, rb_cHash, BOP_ASET);
      Rval = EmitIR(HashSet, Rrecv, Robj, Rset);
    }
    else {
      goto normal_dispatch;
    }
  }
  else {
normal_dispatch:
    vm_search_method(ci, recv);
    EmitIR(GuardMethodCache, reg_pc, params[0], ci);
    Rval = EmitIR(InvokeMethod, ci, 3, params);
  }
  _PUSH(Rval);
}

static void record_opt_aset_with(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  VALUE recv, key, val;
  reg_t Robj, Rrecv, Rval;
  CALL_INFO ci;
  TakeStackSnapshot(builder, reg_pc);
  ci    = (CALL_INFO)GET_OPERAND(1);
  key   = (VALUE)    GET_OPERAND(2);
  recv  = TOPN(1);
  val   = TOPN(0);
  Rval  = _POP();
  Rrecv = _POP();
  Robj  = EmitIR(LoadConstString, key);

  reg_t params[] = {Rrecv, Robj, Rval};

  if (!SPECIAL_CONST_P(recv)) {
    EmitIR(GuardTypeSpecialConst, reg_pc, Rrecv);
    VALUE recv_klass = RBASIC_CLASS(recv);
    if(recv_klass == rb_cHash &&
       BASIC_OP_UNREDEFINED_P(BOP_ASET, ARRAY_REDEFINED_OP_FLAG)) {
      EmitIR(GuardTypeHash, reg_pc, Rrecv);
      EmitIR(GuardMethodRedefine, reg_pc, rb_cHash, BOP_ASET);
      Rval = EmitIR(HashSet, Rrecv, Robj, Rval);
    }
    else {
      goto normal_dispatch;
    }
  }
  else {
normal_dispatch:
    vm_search_method(ci, recv);
    EmitIR(GuardMethodCache, reg_pc, params[0], ci);
    Rval = EmitIR(InvokeMethod, ci, 3, params);
  }
  _PUSH(Rval);
}

static void record_opt_aref_with(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  VALUE recv, key;
  reg_t Robj, Rrecv, Rval;
  CALL_INFO ci;
  TakeStackSnapshot(builder, reg_pc);
  ci    = (CALL_INFO)GET_OPERAND(1);
  key   = (VALUE)    GET_OPERAND(2);
  recv  = TOPN(0);
  Rrecv = _POP();

  Robj  = EmitIR(LoadConstString, key);
  reg_t params[] = {Rrecv, Robj};

  if (!SPECIAL_CONST_P(recv)) {
    EmitIR(GuardTypeSpecialConst, reg_pc, Rrecv);
    VALUE recv_klass = RBASIC_CLASS(recv);
    if(recv_klass == rb_cHash &&
       BASIC_OP_UNREDEFINED_P(BOP_AREF, ARRAY_REDEFINED_OP_FLAG)) {
      EmitIR(GuardTypeHash, reg_pc, Rrecv);
      EmitIR(GuardMethodRedefine, reg_pc, rb_cHash, BOP_AREF);
      Rval = EmitIR(HashGet, Rrecv, Robj);
    }
    else {
      goto normal_dispatch;
    }
  }
  else {
normal_dispatch:
    vm_search_method(ci, recv);
    EmitIR(GuardMethodCache, reg_pc, params[0], ci);
    Rval = EmitIR(InvokeMethod, ci, 2, params);
  }
  _PUSH(Rval);
}

static void _record_length(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc, int bop)
{
  VALUE recv;
  reg_t Rrecv, Rval;
  reg_t params[1];
  CALL_INFO ci;
  TakeStackSnapshot(builder, reg_pc);
  ci   = (CALL_INFO)GET_OPERAND(1);
  recv = TOPN(0);
  Rrecv = _POP();
  params[0] = Rrecv;
  if (!SPECIAL_CONST_P(recv)) {
    EmitIR(GuardTypeSpecialConst, reg_pc, Rrecv);
    if (RBASIC_CLASS(recv) == rb_cString &&
        BASIC_OP_UNREDEFINED_P(bop, STRING_REDEFINED_OP_FLAG)) {
      EmitIR(GuardTypeString, reg_pc, Rrecv);
      EmitIR(GuardMethodRedefine, reg_pc, rb_cString, bop);
      Rval = EmitIR(StringLength, Rrecv);
    }
    else if (RBASIC_CLASS(recv) == rb_cArray &&
             BASIC_OP_UNREDEFINED_P(bop, ARRAY_REDEFINED_OP_FLAG)) {
      EmitIR(GuardTypeArray, reg_pc, Rrecv);
      EmitIR(GuardMethodRedefine, reg_pc, rb_cArray, bop);
      Rval = EmitIR(ArrayLength, Rrecv);
    }
    else if (RBASIC_CLASS(recv) == rb_cHash &&
             BASIC_OP_UNREDEFINED_P(bop, ARRAY_REDEFINED_OP_FLAG)) {
      EmitIR(GuardTypeHash, reg_pc, Rrecv);
      EmitIR(GuardMethodRedefine, reg_pc, rb_cHash, bop);
      Rval = EmitIR(HashLength, Rrecv);
    }
    else {
      goto normal_dispatch;
    }
  }
  else {
normal_dispatch:
    vm_search_method(ci, recv);
    EmitIR(GuardMethodCache, reg_pc, params[0], ci);
    Rval = EmitIR(InvokeMethod, ci, 1, params);
  }
  _PUSH(Rval);
}

static void record_opt_length(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  _record_length(builder, reg_cfp, reg_pc, BOP_LENGTH);
}

static void record_opt_size(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  _record_length(builder, reg_cfp, reg_pc, BOP_SIZE);
}

static void record_opt_empty_p(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  VALUE recv;
  reg_t Rrecv, Rval;
  CALL_INFO ci;
  TakeStackSnapshot(builder, reg_pc);
  ci   = (CALL_INFO)GET_OPERAND(1);
  recv = TOPN(0);
  Rrecv = _POP();
  reg_t params[] = {Rrecv};
  if (!SPECIAL_CONST_P(recv)) {
    EmitIR(GuardTypeSpecialConst, reg_pc, Rrecv);
    if (RBASIC_CLASS(recv) == rb_cString &&
        BASIC_OP_UNREDEFINED_P(BOP_EMPTY_P, STRING_REDEFINED_OP_FLAG)) {
      EmitIR(GuardTypeString, reg_pc, Rrecv);
      EmitIR(GuardMethodRedefine, reg_pc, rb_cString, BOP_EMPTY_P);
      Rval = EmitIR(StringEmptyP, Rrecv);
    }
    else if (RBASIC_CLASS(recv) == rb_cArray &&
             BASIC_OP_UNREDEFINED_P(BOP_EMPTY_P, ARRAY_REDEFINED_OP_FLAG)) {
      EmitIR(GuardTypeArray, reg_pc, Rrecv);
      EmitIR(GuardMethodRedefine, reg_pc, rb_cArray, BOP_EMPTY_P);
      Rval = EmitIR(ArrayEmptyP, Rrecv);
    }
    else if (RBASIC_CLASS(recv) == rb_cHash &&
             BASIC_OP_UNREDEFINED_P(BOP_EMPTY_P, ARRAY_REDEFINED_OP_FLAG)) {
      EmitIR(GuardTypeHash, reg_pc, Rrecv);
      EmitIR(GuardMethodRedefine, reg_pc, rb_cHash, BOP_EMPTY_P);
      Rval = EmitIR(HashEmptyP, Rrecv);
    }
    else {
      goto normal_dispatch;
    }
  }
  else {
normal_dispatch:
    vm_search_method(ci, recv);
    EmitIR(GuardMethodCache, reg_pc, params[0], ci);
    Rval = EmitIR(InvokeMethod, ci, 1, params);
  }
  _PUSH(Rval);

}

static void record_opt_succ(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  VALUE recv = TOPN(0);
  reg_t Rrecv, Robj;
  if (SPECIAL_CONST_P(recv) && FIXNUM_P(recv) &&
      BASIC_OP_UNREDEFINED_P(BOP_SUCC, FIXNUM_REDEFINED_OP_FLAG)) {
    TakeStackSnapshot(builder, reg_pc);
    EmitIR(GuardTypeFixnum, reg_pc, Rrecv);
    EmitIR(GuardMethodRedefine, reg_pc, rb_cFixnum, BOP_SUCC);
    Rrecv = _POP();
    Robj  = EmitIR(LoadConstFixnum, INT2FIX(1));
    _PUSH(EmitIR(FixnumAddOverflow, Rrecv, Robj));
    return;
  }
  else {
    if (RBASIC_CLASS(recv) == rb_cString &&
        BASIC_OP_UNREDEFINED_P(BOP_SUCC, STRING_REDEFINED_OP_FLAG)) {
      EmitIR(GuardMethodRedefine, reg_pc, rb_cString, BOP_SUCC);
      EmitIR(GuardTypeString, reg_pc, Rrecv);
      reg_t argv[] = {_POP()};
      _PUSH(EmitIR(InvokeNative, rb_str_succ, 1, argv));
      return;
    }
    else if (RBASIC_CLASS(recv) == rb_cTime &&
             BASIC_OP_UNREDEFINED_P(BOP_SUCC, TIME_REDEFINED_OP_FLAG)) {
      EmitIR(GuardMethodRedefine, reg_pc, rb_cTime, BOP_SUCC);
      EmitIR(GuardTypeTime, reg_pc, Rrecv);
      reg_t argv[] = {_POP()};
      _PUSH(EmitIR(InvokeNative, rb_time_succ, 1, argv));
      return;
    }
  }
  not_support_op(reg_cfp, reg_pc, "opt_not");
}

static void record_opt_not(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  not_support_op(reg_cfp, reg_pc, "opt_not");
  //CALL_INFO ci = (CALL_INFO)GET_OPERAND(1);
  //reg_t params[] = {_POP()};

  //TakeStackSnapshot(builder, reg_pc);

  //extern VALUE rb_obj_not(VALUE obj);
  //vm_search_method(ci, recv);

  //EmitIR(GuardMethodCache, reg_pc, params[0], ci);
  //if (check_cfunc(ci->me, rb_obj_not)) {
  //  val = RTEST(recv) ? Qfalse : Qtrue;
  //}
  //else {
  //  PUSH(recv);
  //  CALL_SIMPLE_METHOD(recv);
  //}
}

static void record_opt_regexpmatch1(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  if (BASIC_OP_UNREDEFINED_P(BOP_MATCH, REGEXP_REDEFINED_OP_FLAG)) {
    VALUE r = GET_OPERAND(1);
    reg_t RRe;
    TakeStackSnapshot(builder, reg_pc);
    EmitIR(GuardMethodRedefine, reg_pc, rb_cRegexp, BOP_MATCH);
    RRe = EmitLoadConst(builder, r);
    _PUSH(EmitIR(RegExpMatch, RRe, _POP()));
  }
  else {
    not_support_op(reg_cfp, reg_pc, "opt_regexpmatch1");
  }
}

static void record_opt_regexpmatch2(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  VALUE obj2 = TOPN(1);
  if (CLASS_OF(obj2) == rb_cString &&
      BASIC_OP_UNREDEFINED_P(BOP_MATCH, STRING_REDEFINED_OP_FLAG)) {
    reg_t Robj1, Robj2;
    TakeStackSnapshot(builder, reg_pc);
    Robj2 = _POP();
    Robj1 = _POP();
    EmitIR(GuardMethodRedefine, reg_pc, rb_cString, BOP_MATCH);
    EmitIR(GuardTypeString, reg_pc, Robj2);
    _PUSH(EmitIR(RegExpMatch, Robj1, Robj2));
  }
  else {
    not_support_op(reg_cfp, reg_pc, "opt_regexpmatch2");
  }
}

static void record_opt_call_c_function(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  not_support_op(reg_cfp, reg_pc, "opt_call_c_function");
}

static void record_bitblt(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  not_support_op(reg_cfp, reg_pc, "bitblt");
}

static void record_answer(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  _PUSH(EmitIR(LoadConstFixnum, INT2FIX(42)));
}

static void record_getlocal_OP__WC__0(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  lindex_t idx = (lindex_t)GET_OPERAND(1);
  _record_getlocal(builder, 0, idx);
}

static void record_getlocal_OP__WC__1(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  lindex_t idx = (lindex_t)GET_OPERAND(1);
  _record_getlocal(builder, 1, idx);
}

static void record_setlocal_OP__WC__0(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  lindex_t idx = (lindex_t)GET_OPERAND(1);
  _record_setlocal(builder, 0, idx);
}

static void record_setlocal_OP__WC__1(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  lindex_t idx = (lindex_t)GET_OPERAND(1);
  _record_setlocal(builder, 1, idx);
}

static void record_putobject_OP_INT2FIX_O_0_C_(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  reg_t Rval = EmitIR(LoadConstFixnum, INT2FIX(0));
  _PUSH(Rval);
}

static void record_putobject_OP_INT2FIX_O_1_C_(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  reg_t Rval = EmitIR(LoadConstFixnum, INT2FIX(1));
  _PUSH(Rval);
}

static void record_insn(lir_builder_t *builder, int opcode, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
#define CASE_RECORD(op) case BIN(op): record_##op(builder, reg_cfp, reg_pc); break
  switch (opcode) {
    CASE_RECORD(nop);
    CASE_RECORD(getlocal);
    CASE_RECORD(setlocal);
    CASE_RECORD(getspecial);
    CASE_RECORD(setspecial);
    CASE_RECORD(getinstancevariable);
    CASE_RECORD(setinstancevariable);
    CASE_RECORD(getclassvariable);
    CASE_RECORD(setclassvariable);
    CASE_RECORD(getconstant);
    CASE_RECORD(setconstant);
    CASE_RECORD(getglobal);
    CASE_RECORD(setglobal);
    CASE_RECORD(putnil);
    CASE_RECORD(putself);
    CASE_RECORD(putobject);
    CASE_RECORD(putspecialobject);
    CASE_RECORD(putiseq);
    CASE_RECORD(putstring);
    CASE_RECORD(concatstrings);
    CASE_RECORD(tostring);
    CASE_RECORD(toregexp);
    CASE_RECORD(newarray);
    CASE_RECORD(duparray);
    CASE_RECORD(expandarray);
    CASE_RECORD(concatarray);
    CASE_RECORD(splatarray);
    CASE_RECORD(newhash);
    CASE_RECORD(newrange);
    CASE_RECORD(pop);
    CASE_RECORD(dup);
    CASE_RECORD(dupn);
    CASE_RECORD(swap);
    CASE_RECORD(reput);
    CASE_RECORD(topn);
    CASE_RECORD(setn);
    CASE_RECORD(adjuststack);
    CASE_RECORD(defined);
    CASE_RECORD(checkmatch);
    CASE_RECORD(trace);
    CASE_RECORD(defineclass);
    CASE_RECORD(send);
    CASE_RECORD(opt_str_freeze);
    CASE_RECORD(opt_send_simple);
    CASE_RECORD(invokesuper);
    CASE_RECORD(invokeblock);
    CASE_RECORD(leave);
    CASE_RECORD(throw);
    CASE_RECORD(jump);
    CASE_RECORD(branchif);
    CASE_RECORD(branchunless);
    CASE_RECORD(getinlinecache);
    CASE_RECORD(setinlinecache);
    CASE_RECORD(once);
    CASE_RECORD(opt_case_dispatch);
    CASE_RECORD(opt_plus);
    CASE_RECORD(opt_minus);
    CASE_RECORD(opt_mult);
    CASE_RECORD(opt_div);
    CASE_RECORD(opt_mod);
    CASE_RECORD(opt_eq);
    CASE_RECORD(opt_neq);
    CASE_RECORD(opt_lt);
    CASE_RECORD(opt_le);
    CASE_RECORD(opt_gt);
    CASE_RECORD(opt_ge);
    CASE_RECORD(opt_ltlt);
    CASE_RECORD(opt_aref);
    CASE_RECORD(opt_aset);
    CASE_RECORD(opt_aset_with);
    CASE_RECORD(opt_aref_with);
    CASE_RECORD(opt_length);
    CASE_RECORD(opt_size);
    CASE_RECORD(opt_empty_p);
    CASE_RECORD(opt_succ);
    CASE_RECORD(opt_not);
    CASE_RECORD(opt_regexpmatch1);
    CASE_RECORD(opt_regexpmatch2);
    CASE_RECORD(opt_call_c_function);
    CASE_RECORD(bitblt);
    CASE_RECORD(answer);
    CASE_RECORD(getlocal_OP__WC__0);
    CASE_RECORD(getlocal_OP__WC__1);
    CASE_RECORD(setlocal_OP__WC__0);
    CASE_RECORD(setlocal_OP__WC__1);
    CASE_RECORD(putobject_OP_INT2FIX_O_0_C_);
    CASE_RECORD(putobject_OP_INT2FIX_O_1_C_);
    default:
    assert(0 && "unreachable");
  }
  if (is_tracing()) {
    if (builder->ValueId > GWIR_MAX_TRACE_LENGTH) {
      disable_trace(reg_cfp, reg_pc, TRACE_ERROR_TOO_LONG_TRACE);
    }
    record_stack_bottom(builder, opcode, reg_pc);
  }
}

