/**********************************************************************

  ir.c -

  $Author$

  Copyright (C) 2014 Masahiro Ide

 **********************************************************************/

typedef VALUE *VALUEPtr;
typedef void  *voidPtr;
typedef BasicBlock *BasicBlockPtr;

#define FMT(T) FMT_##T
#define FMT_int      "%d"
#define FMT_long     "%ld"
#define FMT_reg_t    "%04lu"
#define FMT_ID       "%04d"
#define FMT_VALUE    "0x%lx"
#define FMT_VALUEPtr "%p"
#define FMT_voidPtr  "%p"
#define FMT_CALL_INFO "%p"
#define FMT_IC        "%p"
#define FMT_BasicBlockPtr FMT_reg_t
#define FMT_rb_event_flag_t "%lu"

#define DATA(T, V) DATA_##T(V)
#define DATA_int(V)           (V)
#define DATA_long(V)          (V)
#define DATA_reg_t(V)         (V)
#define DATA_VALUE(V)         (V)
#define DATA_VALUEPtr(V)      (V)
#define DATA_voidPtr(V)       (V)
#define DATA_CALL_INFO(V)     (V)
#define DATA_IC(V)            (V)
#define DATA_BasicBlockPtr(V) (lir_getid(&(V)->base))
#define DATA_rb_event_flag_t(V) (V)

static long lir_getid(lir_compile_data_header_t *ir)
{
  return (long) ir->id;
}

// FIXME this function is super slow!!!
static lir_compile_data_header_t *FindLIRById(lir_builder_t *builder, reg_t Reg)
{
  BasicBlock *block = builder->EntryBlock;
  while(block != NULL) {
    unsigned i = 0;
    for (i = 0; i < block->size; i++) {
      if (block->Insts[i]->id == (unsigned) Reg) {
        return block->Insts[i];
      }
    }
    block = (BasicBlock *) block->base.next;
  }
  return NULL;
}

static void *lir_inst_init(void *ptr, unsigned opcode)
{
  lir_compile_data_header_t *inst = (lir_compile_data_header_t *) ptr;
  inst->id   = 0;
  inst->flag = 0;
  inst->opcode = opcode;
  return inst;
}

static int AddInst(lir_builder_t *builder, lir_compile_data_header_t *inst, size_t inst_size);

#define LIR_NEWINST(T) \
    ((T*) lir_inst_init(alloca(sizeof(T)), OPCODE_##T))
#define LIR_NEWINST_N(T, SIZE) \
    ((T*) lir_inst_init(alloca(sizeof(T) + sizeof(reg_t) * (SIZE)), OPCODE_##T))

#define ADD_INST(BUILDER, INST) ADD_INST_N(BUILDER, INST, 0)

#define ADD_INST_N(BUILDER, INST, SIZE) \
    AddInst(BUILDER, &(INST)->base, sizeof(*INST) + sizeof(reg_t) * (SIZE))

#include "gwir.c"

static int peephole(lir_builder_t *builder, lir_compile_data_header_t *inst)
{
  if (inst->opcode == OPCODE_IGuardTypeFixnum) {
    IGuardTypeFixnum *ir = (IGuardTypeFixnum *) inst;
    lir_compile_data_header_t *src = FindLIRById(builder, ir->R);
    if (src && src->opcode == OPCODE_ILoadConstFixnum) {
      return -1;
    }
  }

  if (inst->opcode == OPCODE_IGuardTypeFlonum ||
      inst->opcode == OPCODE_IGuardTypeFloat) {
    IGuardTypeFloat *ir = (IGuardTypeFloat *) inst;
    lir_compile_data_header_t *src = FindLIRById(builder, ir->R);
    if (src && src->opcode == OPCODE_ILoadConstFloat) {
      return -1;
    }
  }

  //  if (inst->opcode == OPCODE_IGuardTypeSpecialConst) {
  //    IGuardTypeFloat *ir = (IGuardTypeFloat *) inst;
  //    lir_compile_data_header_t *src = FindLIRById(builder, ir->R);
  //    if (src && src->opcode == OPCODE_LoadConstFloat) {
  //      return -1;
  //    }
  //  }
  //
  //  if (inst->opcode == OPCODE_IGuardType) {
  //    IGuardType *ir = (IGuardType *) inst;
  //    lir_compile_data_header_t *src = FindLIRById(builder, ir->Reg);
  //    if (src && src->opcode == OPCODE_ILoadObject) {
  //      ILoadObject *lo = (ILoadObject *) src;
  //      if (ir->klass == RBASIC_CLASS(lo->O)) {
  //        return -1;
  //      }
  //    }
  //  }
  return 0;
}

static int lir_inst_define_value(int opcode)
{
#define DEF_IR(OPNAME) case OPCODE_I##OPNAME: return GWIR_USE_##OPNAME;
  switch(opcode) {
    GWIR_EACH(DEF_IR);
    default:
    assert(0 && "unreachable");
  }
#undef DEF_IR
  return 0;
}

#if DUMP_LIR > 0
static void dump_lir_inst(lir_compile_data_header_t *Inst);
#endif /* DUMP_LIR > 0*/

static int AddInst(lir_builder_t *builder, lir_compile_data_header_t *inst, size_t inst_size)
{
  BasicBlock *bb = builder->Block;
  int opt = peephole(builder, inst);
  if (opt != 0) {
    return opt;
  }

  if (bb->size == bb->capacity) {
    unsigned newsize = bb->capacity * 2;
    bb->Insts = (lir_compile_data_header_t **)
        lir_realloc(builder, bb->Insts,
                    sizeof(lir_compile_data_header_t *) * bb->capacity,
                    sizeof(lir_compile_data_header_t *) * newsize);
    bb->capacity = newsize;
  }
  lir_compile_data_header_t *buf = lir_alloc(builder, inst_size);
  int newid = buf->id;
  memcpy(buf, inst, inst_size);
  buf->id = newid;
  assert(buf->id != 0);
  bb->Insts[bb->size] = buf;
  bb->size += 1;
  //#if DUMP_LIR > 0
  //  dump_lir_inst(buf);
  //#endif
  return newid;
}

#if DUMP_LIR > 0
static void dump_lir_inst(lir_compile_data_header_t *Inst)
{
#define DUMP_IR(OPNAME) case OPCODE_I##OPNAME: Dump_##OPNAME(Inst); break;
  switch(Inst->opcode) {
    GWIR_EACH(DUMP_IR);
    default:
    assert(0 && "unreachable");
  }
#undef  DUMP_IR
}

static void dump_lir_block(BasicBlock *block)
{
  unsigned i = 0;
  fprintf(stderr, "BB%d (pc=%p)\n", block->base.id, block->start_pc);
  for (i = 0; i < block->size; i++) {
    lir_compile_data_header_t *Inst = block->Insts[i];
    dump_lir_inst(Inst);
  }
}

static void dump_side_exit(lir_builder_t *builder)
{
  unsigned i;
  hashmap_iterator_t itr = {0, 0};
  while(hashmap_next(&builder->Current->SideExit, &itr)) {
    VALUE *pc = itr.entry->k;
    fprintf(stderr, "side exit: pc=%p: ", pc);
    StackMap *stack = GetStackMap(builder, pc);
    for (i = 0; i < stack->size; i++) {
      fprintf(stderr, "  [%d] = %04ld;", i, stack->regs[i]);
    }
    fprintf(stderr, "\n");
  }
}
#endif /* DUMP_LIR > 0 */

static void dump_lir(lir_builder_t *builder)
{
#if DUMP_LIR > 0
  BasicBlock *entry = builder->EntryBlock;
  BasicBlock *block = entry;
  fprintf(stderr, "---------------\n");
  while(block != NULL) {
    dump_lir_block(block);
    block = (BasicBlock *) block->base.next;
  }
  fprintf(stderr, "---------------\n");
  dump_side_exit(builder);
  fprintf(stderr, "---------------\n");
#endif
}
