/**********************************************************************

  record.c -

  $Author$

  Copyright (C) 2014 Masahiro Ide

 **********************************************************************/

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

#undef  GET_GLOBAL_CONSTANT_STATE
#define GET_GLOBAL_CONSTANT_STATE() (*ruby_vm_global_constant_state_ptr)

#define not_support_op(Rec, CFP, PC, OPNAME) do {\
  fprintf(stderr, "exit trace : not support bytecode: " OPNAME "\n");\
  TraceRecorderAbort(Rec, CFP, PC, TRACE_ERROR_UNSUPPORT_OP);\
  return;\
} while(0)

#define EmitIR(OP, ...) Emit_##OP(Rec, ## __VA_ARGS__)

#define _POP()       PopRegister(Rec)
#define _PUSH(REG)   PushRegister(Rec, REG)
#define _TOPN(N)     TopRegister(Rec, (int)(N))
#define _SET(N, REG) SetRegister(Rec, (int)(N), REG)

static reg_t EmitConverter(TraceRecorder *Rec, VALUE val, reg_t Rval, VALUE *reg_pc, int type) {
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

static reg_t EmitLoadConst(TraceRecorder *Rec, VALUE val)
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

static reg_t EmitSpecialInst_FixnumSucc(TraceRecorder *Rec, CALL_INFO ci, reg_t *regs)
{
  reg_t Robj = EmitIR(LoadConstFixnum, INT2FIX(1));
  return EmitIR(FixnumAddOverflow, regs[0], Robj);
}

static reg_t EmitSpecialInst_TimeSucc(TraceRecorder *Rec, CALL_INFO ci, reg_t *regs)
{
  return Emit_InvokeNative(Rec, rb_time_succ, 1, regs);
}

static reg_t EmitSpecialInst_GetPropertyName(TraceRecorder *Rec, CALL_INFO ci, reg_t *regs)
{
  VALUE obj = ci->recv;
  VALUE *reg_pc = Rec->CurrentEvent->pc;
  int cacheable = vm_load_cache(obj, ci->me->def->body.attr.id, 0, ci, 1);
  assert(ci->aux.index > 0 && cacheable);
  EmitIR(GuardProperty, reg_pc, regs[0], 1/*is_attr*/, (void *)ci);
  return Emit_GetPropertyName(Rec, regs[0], ci->aux.index - 1);
}

static reg_t EmitSpecialInst_SetPropertyName(TraceRecorder *Rec, CALL_INFO ci, reg_t *regs)
{
  VALUE obj = ci->recv;
  VALUE *reg_pc = Rec->CurrentEvent->pc;
  int cacheable = vm_load_cache(obj, ci->me->def->body.attr.id, 0, ci, 1);
  assert(ci->aux.index > 0 && cacheable);
  EmitIR(GuardProperty, reg_pc, regs[0], 1/*is_attr*/, (void *)ci);
  return Emit_SetPropertyName(Rec, regs[0], ci->aux.index - 1, regs[1]);
}

#define Unredefined(...)      (1)
#define GuardUnredefined(...) (1)
static reg_t EmitSpecialInst(TraceRecorder *Rec, VALUE *pc, CALL_INFO ci, enum ruby_vminsn_type opcode, VALUE* params, reg_t* regs)
{
#include "yarv2gwir.c"
  return -1;
}

static void EmitSpecialInst0(TraceRecorder *Rec, VALUE *pc, CALL_INFO ci, int opcode, VALUE* params, reg_t* regs)
{
  reg_t Rval = -1;
  if ((Rval = EmitSpecialInst(Rec, pc, ci, opcode, params, regs)) == -1) {
    vm_search_method(ci, params[0]);
    ci = CloneInlineCache(&Rec->CacheMng, ci);
    EmitIR(GuardMethodCache, pc, regs[0], ci);
    Rval = EmitIR(InvokeMethod, ci, 2, regs);
  }
  _PUSH(Rval);
}

static void EmitSpecialInst1(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc, int opcode)
{
  TakeStackSnapshot(Rec, reg_pc);
  CALL_INFO ci = (CALL_INFO)GET_OPERAND(1);
  int i, argc = ci->argc + 1;
  reg_t regs[argc];
  VALUE params[argc];
  for (i = 0; i < argc; i++) {
    params[i] = TOPN(ci->argc - i);
    regs[ci->argc - i] = _POP();
  }
  EmitSpecialInst0(Rec, reg_pc, ci, opcode, params, regs);
}

static void EmitPushFrame(TraceRecorder *Rec, VALUE *reg_pc, CALL_INFO ci, reg_t Rblock, rb_block_t *block)
{
  int i, argc = ci->argc + 1/*recv*/;
  reg_t argv[argc];
  for (i = 0; i < argc; i++) {
    argv[i] = _TOPN(ci->argc - i);
  }
  Rec->CallDepth += 1;
  EmitIR(GuardMethodCache, reg_pc, argv[0], ci);
  if (Rblock) {
    EmitIR(GuardBlockEqual, reg_pc, Rblock, (VALUE) block);
  }
  PushCallStack(Rec, argc, argv);
  _PUSH(EmitIR(FramePush, ci, 0, Rblock, argc, argv));
}

static void EmitNewInstance(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc, CALL_INFO ci)
{
  int i, argc = ci->argc;
  reg_t argv[argc];
  reg_t klass = _TOPN(ci->argc);

  TakeStackSnapshot(Rec, reg_pc);
  if ((ci->flag & VM_CALL_ARGS_BLOCKARG) || ci->blockiseq != 0) {
    fprintf(stderr, "Class.new with block is not supported\n");
    TraceRecorderAbort(Rec, reg_cfp, reg_pc, TRACE_ERROR_UNSUPPORT_OP);
    return;
  }

  for (i = 0; i < argc; i++) {
    argv[argc - i - 1] = _POP();
  }
  assert(klass == _POP());

  _PUSH(EmitIR(AllocObject, klass, argc, argv));
}

static void EmitMethodCall(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc, CALL_INFO ci, rb_block_t *block, reg_t Rblock, int opcode)
{
  int i, argc = ci->argc + 1/*recv*/;
  reg_t regs[argc];
  VALUE params[argc];

  for (i = 0; i < argc; i++) {
    regs[i]   = _TOPN(ci->argc - i);
    params[i] = TOPN(ci->argc - i);
  }

  vm_search_method(ci, ci->recv = TOPN(ci->argc));
  ci = CloneInlineCache(&Rec->CacheMng, ci);

  reg_t Rval = -1;
  if ((Rval = EmitSpecialInst(Rec, reg_pc, ci, opcode, params, regs)) != -1) {
    return;
  }

  // check method type
  if(ci->me) {
    // user defined ruby method
    if (ci->me->def->type == VM_METHOD_TYPE_ISEQ) {
      return EmitPushFrame(Rec, reg_pc, ci, Rblock, block);
    }
  }

  // check ClassA.new(argc, argv)
  if (check_cfunc(ci->me,  rb_class_new_instance)) {
    if (ci->me->klass == rb_cClass) {
      return EmitNewInstance(Rec, reg_cfp, reg_pc, ci);
    }
  }

  // check block_given?
  extern VALUE rb_f_block_given_p(void);
  if (check_cfunc(ci->me,  rb_f_block_given_p)) {
    reg_t Rrecv = EmitIR(LoadSelf);
    EmitIR(GuardMethodCache, reg_pc, Rrecv, ci);
    EmitIR(InvokeNative, rb_f_block_given_p, 0, NULL);
    return;
  }

  // unreachable
  TraceRecorderAbort(Rec, reg_cfp, reg_pc, TRACE_ERROR_NATIVE_METHOD);
  return;
}

static void _record_getlocal(TraceRecorder *Rec, rb_num_t level, lindex_t idx)
{
  reg_t Rval = EmitIR(EnvLoad, (int) level, (int) idx);
  _PUSH(Rval);
}

static void _record_setlocal(TraceRecorder *Rec, rb_num_t level, lindex_t idx)
{
  reg_t Rval = _POP();
  EmitIR(EnvStore, (int) level, (int) idx, Rval);
}

// record API

static void record_nop(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  /* do nothing */
}

static void record_getlocal(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  rb_num_t level = (rb_num_t)GET_OPERAND(2);
  lindex_t idx = (lindex_t)GET_OPERAND(1);
  _record_getlocal(Rec, level, idx);
}

static void record_setlocal(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  rb_num_t level = (rb_num_t)GET_OPERAND(2);
  lindex_t idx = (lindex_t)GET_OPERAND(1);
  _record_setlocal(Rec, level, idx);
}

static void record_getspecial(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  not_support_op(Rec, reg_cfp, reg_pc, "getspecial");
}

static void record_setspecial(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  not_support_op(Rec, reg_cfp, reg_pc, "setspecial");
}

static void record_getinstancevariable(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  IC ic = (IC)GET_OPERAND(2);
  ID id = (ID)GET_OPERAND(1);
  VALUE obj = GET_SELF();
  reg_t Rrecv = EmitIR(LoadSelf);

  if (vm_load_cache(obj, id, ic, NULL, 0)) {
    TakeStackSnapshot(Rec, reg_pc);
    EmitIR(GuardTypeObject, reg_pc, Rrecv);
    EmitIR(GuardProperty, reg_pc, Rrecv, 0/*!is_attr*/, (void *) ic);
    _PUSH(EmitIR(GetPropertyName, Rrecv, ic->ic_value.index));
    return;
  }
  not_support_op(Rec, reg_cfp, reg_pc, "getinstancevariable");
}

static void record_setinstancevariable(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  IC ic = (IC)GET_OPERAND(2);
  ID id = (ID)GET_OPERAND(1);
  VALUE obj = GET_SELF();
  reg_t Rrecv = EmitIR(LoadSelf);

  int cacheable = vm_load_cache(obj, id, ic, NULL, 0);
  if (cacheable) {
    TakeStackSnapshot(Rec, reg_pc);
    EmitIR(GuardTypeObject, reg_pc, Rrecv);
    EmitIR(GuardProperty, reg_pc, Rrecv, 0/*!is_attr*/, (void *) ic);
    EmitIR(SetPropertyName, Rrecv, ic->ic_value.index, _POP());
    return;
  }

  not_support_op(Rec, reg_cfp, reg_pc, "setinstancevariable");
}

static void record_getclassvariable(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  not_support_op(Rec, reg_cfp, reg_pc, "getclassvariable");
}

static void record_setclassvariable(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  not_support_op(Rec, reg_cfp, reg_pc, "setclassvariable");
}

static void record_getconstant(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  not_support_op(Rec, reg_cfp, reg_pc, "getconstant");
}

static void record_setconstant(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  not_support_op(Rec, reg_cfp, reg_pc, "setconstant");
}

static void record_getglobal(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  GENTRY entry = (GENTRY) GET_OPERAND(1);
  reg_t Id = EmitLoadConst(Rec, entry);
  _PUSH(EmitIR(GetGlobal, Id));
}

static void record_setglobal(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  GENTRY entry = (GENTRY) GET_OPERAND(1);
  reg_t Rval = _POP();
  reg_t Id = EmitLoadConst(Rec, entry);
  EmitIR(SetGlobal, Id, Rval);
}

static void record_putnil(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  _PUSH(EmitIR(LoadConstNil));
}

static void record_putself(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  _PUSH(EmitIR(LoadSelf));
}

static void record_putobject(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  VALUE val = (VALUE) GET_OPERAND(1);
  _PUSH(EmitLoadConst(Rec, val));
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

static void record_putspecialobject(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
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
  _PUSH(EmitLoadConst(Rec, val));
}

static void record_putiseq(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  ISEQ iseq = (ISEQ) GET_OPERAND(1);
  _PUSH(EmitLoadConst(Rec, iseq->self));
}

static void record_putstring(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  VALUE val  = (VALUE) GET_OPERAND(1);
  reg_t argv[] = {EmitIR(LoadConstString, val)};
  reg_t Rval   = EmitIR(InvokeNative, rb_str_resurrect, 1, argv);
  _PUSH(Rval);
}

static void record_concatstrings(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
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

static void record_tostring(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  reg_t argv[] = {_POP()};
  reg_t Rval = EmitIR(InvokeNative, rb_obj_as_string, 1, argv);
  _PUSH(Rval);
}

static void record_toregexp(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  not_support_op(Rec, reg_cfp, reg_pc, "toregexp");
}

static void record_newarray(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  rb_num_t i, num = (rb_num_t)GET_OPERAND(1);
  reg_t argv[num];
  for (i = 0; i < num; i++) {
    argv[i] = _POP();
  }
  _PUSH(EmitIR(AllocArray, (int) num, argv));
}

static void record_duparray(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  VALUE val = (VALUE) GET_OPERAND(1);
  reg_t Rval = EmitLoadConst(Rec, val);
  reg_t argv[] = {Rval};
  _PUSH(EmitIR(InvokeNative, rb_ary_resurrect, 1, argv));
}

static void record_expandarray(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  not_support_op(Rec, reg_cfp, reg_pc, "expandarray");
}

static void record_concatarray(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  not_support_op(Rec, reg_cfp, reg_pc, "concatarray");
}

static void record_splatarray(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  not_support_op(Rec, reg_cfp, reg_pc, "splatarray");
}

static void record_newhash(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  rb_num_t i, num = (rb_num_t) GET_OPERAND(1);
  reg_t argv[num];
  for (i = num; i > 0; i -= 2) {
    argv[i - 1] = _POP(); // key
    argv[i - 2] = _POP(); // val
  }
  _PUSH(EmitIR(AllocHash, (int) num, argv));
}

static void record_newrange(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  rb_num_t flag = (rb_num_t) GET_OPERAND(1);
  reg_t Rhigh = _POP();
  reg_t Rlow  = _POP();
  _PUSH(EmitIR(AllocRange, Rlow, Rhigh, (int) flag));
}

static void record_pop(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  _POP();
}

static void record_dup(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  reg_t Rval = _POP();
  _PUSH(Rval);
  _PUSH(Rval);
}

static void record_dupn(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  rb_num_t i, n = (rb_num_t)GET_OPERAND(1);
  reg_t argv[n];
  // FIXME optimize
  for (i = 0; i < n; i++) {
    argv[i] = _TOPN(n - i - 1);
  }
  for (i = 0; i < n; i++) {
    _PUSH(argv[i]);
  }
}

static void record_swap(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  reg_t Rval = _POP();
  reg_t Robj = _POP();
  _PUSH(Robj);
  _PUSH(Rval);
}

static void record_reput(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  _PUSH(_POP());
}

static void record_topn(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  not_support_op(Rec, reg_cfp, reg_pc, "topn");
  //rb_num_t n = (rb_num_t)GET_OPERAND(1);
  //asm volatile("int3"); // need test
  //reg_t Rval = _TOPN(n);
  //_PUSH(Rval);
}

static void record_setn(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  rb_num_t n = (rb_num_t)GET_OPERAND(1);
  reg_t Rval = _POP();
  _SET(n, Rval);
  _PUSH(Rval);
}

static void record_adjuststack(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  rb_num_t i, n = (rb_num_t)GET_OPERAND(1);
  for (i = 0; i < n; i++) {
    _POP();
  }
}

static void record_defined(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  not_support_op(Rec, reg_cfp, reg_pc, "defined");
}

static void record_checkmatch(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  reg_t Rpattern = _POP();
  reg_t Rtarget  = _POP();
  rb_event_flag_t flag = (rb_event_flag_t) GET_OPERAND(1);
  enum vm_check_match_type checkmatch_type =
      (enum vm_check_match_type)(flag & VM_CHECKMATCH_TYPE_MASK);
  if (flag & VM_CHECKMATCH_ARRAY) {
    _PUSH(EmitIR(PatternMatchRange, Rpattern, Rtarget, checkmatch_type));
  }
  else {
    _PUSH(EmitIR(PatternMatch, Rpattern, Rtarget, checkmatch_type));
  }
}

static void record_trace(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  rb_event_flag_t flag = (rb_event_flag_t) GET_OPERAND(1);
  EmitIR(Trace, flag);
}

static void record_defineclass(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  not_support_op(Rec, reg_cfp, reg_pc, "defineclass");
}

static void record_send(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  CALL_INFO ci = (CALL_INFO) GET_OPERAND(1);
  reg_t Rblock = 0;
  rb_block_t *block = NULL;
  ci->argc = ci->orig_argc;
  ci->blockptr = 0;
  //vm_caller_setup_args(th, reg_cfp, ci);
  if (UNLIKELY(ci->flag & VM_CALL_ARGS_BLOCKARG)) {
    not_support_op(Rec, reg_cfp, reg_pc, "send");
    return;
  }
  else if (ci->blockiseq != 0) {
    Rblock = EmitIR(LoadSelfAsBlock, ci->blockiseq);
    block  = RUBY_VM_GET_BLOCK_PTR_IN_CFP(reg_cfp);
  }

  if (UNLIKELY(ci->flag & VM_CALL_ARGS_SPLAT)) {
    not_support_op(Rec, reg_cfp, reg_pc, "send");
    return;
  }

  TakeStackSnapshot(Rec, reg_pc);
  EmitMethodCall(Rec, reg_cfp, reg_pc, ci, block, Rblock, BIN(send));
}

static void record_opt_str_freeze(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  if (BASIC_OP_UNREDEFINED_P(BOP_FREEZE, STRING_REDEFINED_OP_FLAG)) {
    TakeStackSnapshot(Rec, reg_pc);
    EmitIR(GuardMethodRedefine, reg_pc, rb_cString, BOP_FREEZE);
    _PUSH(_POP());
  }
  else {
    not_support_op(Rec, reg_cfp, reg_pc, "opt_str_freeze");
  }
}

static void record_opt_send_simple(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  CALL_INFO ci = (CALL_INFO) GET_OPERAND(1);
  TakeStackSnapshot(Rec, reg_pc);
  EmitMethodCall(Rec, reg_cfp, reg_pc, ci, 0, 0, BIN(opt_send_simple));
}

static void record_invokesuper(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  not_support_op(Rec, reg_cfp, reg_pc, "invokesuper");
}

static void record_invokeblock(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  CALL_INFO ci = (CALL_INFO) GET_OPERAND(1);
  int i, argc = 1/*recv*/ + ci->orig_argc;
  reg_t Rblock, argv[argc];
  TakeStackSnapshot(Rec, reg_pc);

  const rb_block_t *block = rb_vm_control_frame_block_ptr(reg_cfp);
  argv[0] = EmitIR(LoadSelf);
  Rblock  = EmitIR(LoadBlock, block->iseq);

  ci->argc = ci->orig_argc;
  ci->blockptr = 0;
  ci->recv = GET_SELF();

  //fprintf(stderr, "cfp=%p, block=%p\n", reg_cfp, block);
  //asm volatile("int3");
  VALUE type = GET_ISEQ()->local_iseq->type;

  if ((type != ISEQ_TYPE_METHOD && type != ISEQ_TYPE_CLASS) || block == 0) {
    // "no block given (yield)"
    TraceRecorderAbort(Rec, reg_cfp, reg_pc, TRACE_ERROR_THROW);
    return;
  }

  if (UNLIKELY(ci->flag & VM_CALL_ARGS_SPLAT)) {
    TraceRecorderAbort(Rec, reg_cfp, reg_pc, TRACE_ERROR_UNSUPPORT_OP);
    return;
  }

  if (BUILTIN_TYPE(block->iseq) == T_NODE) {
    // yield native block
    TraceRecorderAbort(Rec, reg_cfp, reg_pc, TRACE_ERROR_NATIVE_METHOD);
    return;
  }

  Rec->CallDepth += 1;
  for (i = 0; i < ci->orig_argc; i++) {
    argv[i + 1] = _TOPN(ci->orig_argc - i - 1);
  }

  // XXX
  // In opt_send_simple, argv[0] is already pushed but invokeblock is not.
  // This code is needed for adjusting register stack.
  _PUSH(argv[0]);

  EmitIR(GuardBlockEqual, reg_pc, Rblock, (VALUE) block);
  PushCallStack(Rec, argc, argv);
  _PUSH(EmitIR(FramePush, ci, 1, Rblock, argc, argv));
}

static void record_leave(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  if(VM_FRAME_TYPE_FINISH_P(reg_cfp)) {
    TraceRecorderAbort(Rec, reg_cfp, reg_pc, TRACE_ERROR_LEAVE);
    return;
  }
  if (Rec->CallDepth == 0) {
    TraceRecorderAbort(Rec, reg_cfp, reg_pc, TRACE_ERROR_LEAVE);
    return;
  }
  Rec->CallDepth -= 1;
  reg_t Val = _POP();
  PopCallStack(Rec);

  EmitIR(FramePop);
  _PUSH(Val);
}

static void record_throw(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  not_support_op(Rec, reg_cfp, reg_pc, "throw");
}

static void record_jump(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  OFFSET dst = (OFFSET) GET_OPERAND(1);
  VALUE* TargetPC = reg_pc + insn_len(BIN(jump)) + dst;
  EmitIR(Jump, TargetPC);
  if (FindBasicBlockByPC(Rec, TargetPC) == NULL) {
    CreateBlock(Rec, TargetPC);
  }
}

static void record_branchif(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  OFFSET dst = (OFFSET)GET_OPERAND(1);
  reg_t Rval = _POP();
  VALUE val  = TOPN(0);
  VALUE *NextPC = reg_pc + insn_len(BIN(branchif));
  VALUE *JumpPC = NextPC + dst;

  if (RTEST(val)) {
    TakeStackSnapshot(Rec, NextPC);
    EmitIR(GuardTypeNil, NextPC, Rval);
    EmitIR(Jump,  JumpPC);
    if(FindBasicBlockByPC(Rec, JumpPC) == NULL) {
      CreateBlock(Rec, JumpPC);
    }

  }
  else {
    TakeStackSnapshot(Rec, JumpPC);
    EmitIR(GuardTypeNil, JumpPC, Rval);
    EmitIR(Jump,  NextPC);
    if(FindBasicBlockByPC(Rec, NextPC) == NULL) {
      CreateBlock(Rec, NextPC);
    }
  }
}

static void record_branchunless(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  OFFSET dst  = (OFFSET) GET_OPERAND(1);
  reg_t Rval  = _POP();
  VALUE val   = TOPN(0);
  VALUE *NextPC = reg_pc + insn_len(BIN(branchunless));
  VALUE *JumpPC = NextPC + dst;
  VALUE *TargetPC = NULL;

  if (!RTEST(val)) {
    TakeStackSnapshot(Rec, NextPC);
    EmitIR(GuardTypeNonNil, NextPC, Rval);
    TargetPC = JumpPC;
  }
  else {
    TakeStackSnapshot(Rec, JumpPC);
    EmitIR(GuardTypeNil, JumpPC, Rval);
    TargetPC = NextPC;
  }

  EmitIR(Jump,  TargetPC);
  if(FindBasicBlockByPC(Rec, TargetPC) == NULL) {
    CreateBlock(Rec, TargetPC);
  }

}

static void record_getinlinecache(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  IC ic = (IC)GET_OPERAND(2);
  if (ic->ic_serial != GET_GLOBAL_CONSTANT_STATE()) {
    // hmm, constant value is re-defined.
    not_support_op(Rec, reg_cfp, reg_pc, "getinlinecache");
    return;
  }
  _PUSH(EmitLoadConst(Rec, ic->ic_value.value));
}

static void record_setinlinecache(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  not_support_op(Rec, reg_cfp, reg_pc, "setinlinecache");
}

static void record_once(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  not_support_op(Rec, reg_cfp, reg_pc, "once");
}

static void record_opt_case_dispatch(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  not_support_op(Rec, reg_cfp, reg_pc, "opt_case_dispatch");
}

static void record_opt_plus(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  EmitSpecialInst1(Rec, reg_cfp, reg_pc, BIN(opt_plus));
}

static void record_opt_minus(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  EmitSpecialInst1(Rec, reg_cfp, reg_pc, BIN(opt_minus));
}

static void record_opt_mult(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  EmitSpecialInst1(Rec, reg_cfp, reg_pc, BIN(opt_mult));
}

static void record_opt_div(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  EmitSpecialInst1(Rec, reg_cfp, reg_pc, BIN(opt_div));
}

static void record_opt_mod(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  EmitSpecialInst1(Rec, reg_cfp, reg_pc, BIN(opt_mod));
}

static void record_opt_eq(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  EmitSpecialInst1(Rec, reg_cfp, reg_pc, BIN(opt_eq));
}

static void record_opt_neq(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  EmitSpecialInst1(Rec, reg_cfp, reg_pc, BIN(opt_neq));
}

static void record_opt_lt(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  EmitSpecialInst1(Rec, reg_cfp, reg_pc, BIN(opt_lt));
}

static void record_opt_le(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  EmitSpecialInst1(Rec, reg_cfp, reg_pc, BIN(opt_le));
}

static void record_opt_gt(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  EmitSpecialInst1(Rec, reg_cfp, reg_pc, BIN(opt_gt));
}

static void record_opt_ge(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  EmitSpecialInst1(Rec, reg_cfp, reg_pc, BIN(opt_ge));
}

static void record_opt_ltlt(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  EmitSpecialInst1(Rec, reg_cfp, reg_pc, BIN(opt_ltlt));
}

static void record_opt_aref(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  EmitSpecialInst1(Rec, reg_cfp, reg_pc, BIN(opt_aref));
}

static void record_opt_aset(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  EmitSpecialInst1(Rec, reg_cfp, reg_pc, BIN(opt_aset));
}

static void record_opt_aset_with(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  VALUE recv, key, val;
  reg_t Robj, Rrecv, Rval;
  CALL_INFO ci;
  TakeStackSnapshot(Rec, reg_pc);
  ci    = (CALL_INFO)GET_OPERAND(1);
  key   = (VALUE)    GET_OPERAND(2);
  recv  = TOPN(1);
  val   = TOPN(0);
  Rval  = _POP();
  Rrecv = _POP();
  Robj  = EmitIR(LoadConstString, key);

  VALUE params[] = {recv, key, val};
  reg_t regs[] = {Rrecv, Robj, Rval};
  EmitSpecialInst0(Rec, reg_pc, ci, BIN(opt_aset_with), params, regs);
}

static void record_opt_aref_with(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  VALUE recv, key;
  reg_t Robj, Rrecv;
  CALL_INFO ci;
  TakeStackSnapshot(Rec, reg_pc);
  ci    = (CALL_INFO)GET_OPERAND(1);
  key   = (VALUE)    GET_OPERAND(2);
  recv  = TOPN(0);
  Rrecv = _POP();
  Robj  = EmitIR(LoadConstString, key);

  VALUE params[] = {recv, key};
  reg_t regs[] = {Rrecv, Robj};
  EmitSpecialInst0(Rec, reg_pc, ci, BIN(opt_aref_with), params, regs);
}

static void record_opt_length(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  EmitSpecialInst1(Rec, reg_cfp, reg_pc, BIN(opt_length));
}

static void record_opt_size(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  EmitSpecialInst1(Rec, reg_cfp, reg_pc, BIN(opt_size));
}

static void record_opt_empty_p(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  EmitSpecialInst1(Rec, reg_cfp, reg_pc, BIN(opt_empty_p));
}

static void record_opt_succ(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  EmitSpecialInst1(Rec, reg_cfp, reg_pc, BIN(opt_succ));
}

static void record_opt_not(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  EmitSpecialInst1(Rec, reg_cfp, reg_pc, BIN(opt_not));
}

static void record_opt_regexpmatch1(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  VALUE obj = TOPN(0);
  VALUE r   = GET_OPERAND(1);
  VALUE params[] = {obj, r};
  reg_t RRe  = EmitLoadConst(Rec, r);
  reg_t Robj = _POP();
  reg_t regs[] = {Robj, RRe};
  rb_call_info_t ci;
  ci.mid  = idEqTilde;
  ci.flag = 0;
  ci.orig_argc = ci.argc = 1;

  EmitSpecialInst0(Rec, reg_pc, &ci, BIN(opt_regexpmatch1), params, regs);
}

static void record_opt_regexpmatch2(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  VALUE obj2 = TOPN(1);
  VALUE obj1 = TOPN(0);
  reg_t Robj1 = _POP();
  reg_t Robj2 = _POP();
  VALUE params[] = {obj2, obj1};
  reg_t regs[] = {Robj2, Robj1};
  rb_call_info_t ci;
  ci.mid  = idEqTilde;
  ci.flag = 0;
  ci.orig_argc = ci.argc = 1;
  EmitSpecialInst0(Rec, reg_pc, &ci, BIN(opt_regexpmatch2), params, regs);

}

static void record_opt_call_c_function(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  not_support_op(Rec, reg_cfp, reg_pc, "opt_call_c_function");
}

static void record_bitblt(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  not_support_op(Rec, reg_cfp, reg_pc, "bitblt");
}

static void record_answer(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  _PUSH(EmitIR(LoadConstFixnum, INT2FIX(42)));
}

static void record_getlocal_OP__WC__0(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  lindex_t idx = (lindex_t)GET_OPERAND(1);
  _record_getlocal(Rec, 0, idx);
}

static void record_getlocal_OP__WC__1(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  lindex_t idx = (lindex_t)GET_OPERAND(1);
  _record_getlocal(Rec, 1, idx);
}

static void record_setlocal_OP__WC__0(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  lindex_t idx = (lindex_t)GET_OPERAND(1);
  _record_setlocal(Rec, 0, idx);
}

static void record_setlocal_OP__WC__1(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  lindex_t idx = (lindex_t)GET_OPERAND(1);
  _record_setlocal(Rec, 1, idx);
}

static void record_putobject_OP_INT2FIX_O_0_C_(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  reg_t Rval = EmitIR(LoadConstFixnum, INT2FIX(0));
  _PUSH(Rval);
}

static void record_putobject_OP_INT2FIX_O_1_C_(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  reg_t Rval = EmitIR(LoadConstFixnum, INT2FIX(1));
  _PUSH(Rval);
}

static void record_insn(RJit *jit, Event *e)
{
  TraceRecorder *Rec = jit->Rec;
  int opcode = e->opcode;
  VALUE *reg_pc = e->pc;
  rb_control_frame_t *reg_cfp = e->cfp;
#define CASE_RECORD(op) case BIN(op): record_##op(Rec, reg_cfp, reg_pc); break
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
  if (RJitModeIs(jit, TRACE_MODE_RECORD)) {
    TraceUpdateLastInst(TraceRecorderGetTrace(Rec), reg_pc);
  }
}
