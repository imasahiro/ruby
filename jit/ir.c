/**********************************************************************

  ir.c -

  $Author$

  Copyright (C) 2014 Masahiro Ide

 **********************************************************************/

#define FMT(T) FMT_##T
#define FMT_int "%d"
#define FMT_long "%ld"
#define FMT_reg_t "%04ld"
#define FMT_RegPtr "%04ld"
#define FMT_ID "%04d"
#define FMT_VALUE "0x%lx"
#define FMT_VALUEPtr "%p"
#define FMT_voidPtr "%p"
#define FMT_CALL_INFO "%p"
#define FMT_IC "%p"
#define FMT_ISEQ "%p"
#define FMT_BasicBlockPtr FMT_reg_t
#define FMT_rb_event_flag_t "%lu"

#define DATA(T, V) DATA_##T(V)
#define DATA_int(V) (V)
#define DATA_long(V) (V)
#define DATA_reg_t(V) (V)
#define DATA_RegPtr(V) (*(V))
#define DATA_VALUE(V) (V)
#define DATA_VALUEPtr(V) (V)
#define DATA_voidPtr(V) (V)
#define DATA_CALL_INFO(V) (V)
#define DATA_IC(V) (V)
#define DATA_ISEQ(V) (V)
#define DATA_BasicBlockPtr(V) (lir_getid(&(V)->base))
#define DATA_rb_event_flag_t(V) (V)

static long lir_getid(lir_compile_data_header_t *ir) { return (long)ir->id; }

static BasicBlock *CreateBlock(TraceRecorder *Rec, VALUE *pc)
{
    BasicBlock *bb = NULL;

    if (!RJitModeIs(Rec->jit, TRACE_MODE_RECORD)) {
        return bb;
    }

    bb = (BasicBlock *)lir_alloc(Rec, sizeof(BasicBlock));
    // fprintf(stderr, "newblock=(%p, %p)\n", bb, pc);
    bb->base.flag = 0;
    bb->base.next = NULL;
    bb->start_pc = pc;
    bb->size = 0;
    bb->capacity = 1;
    bb->Insts = (lir_compile_data_header_t **)lir_alloc(
        Rec, sizeof(lir_compile_data_header_t *) * 1);
    if (Rec->Block != NULL) {
        Rec->Block->base.next = (lir_compile_data_header_t *)bb;
    }

    Rec->Block = bb;
    return bb;
}

static BasicBlock *FindBasicBlockByPC(TraceRecorder *Rec, VALUE *pc)
{
    BasicBlock *bb = Rec->EntryBlock;
    while (bb != NULL) {
        if (pc == bb->start_pc) {
            return bb;
        }
        bb = (BasicBlock *)bb->base.next;
    }
    return NULL;
}

static unsigned CountLIRInstSize(TraceRecorder *Rec)
{
    BasicBlock *block = Rec->EntryBlock;
    unsigned sum = 0;
    while (block != NULL) {
        sum += block->size;
        block = (BasicBlock *)block->base.next;
    }
    return sum;
}

// FIXME this function is super slow!!!
static lir_compile_data_header_t *FindLIRById(TraceRecorder *Rec, reg_t Reg)
{
    BasicBlock *block = Rec->EntryBlock;
    while (block != NULL) {
        unsigned i = 0;
        for (i = 0; i < block->size; i++) {
            if (block->Insts[i]->id == Reg) {
                return block->Insts[i];
            }
        }
        block = (BasicBlock *)block->base.next;
    }
    return NULL;
}

static void *lir_inst_init(void *ptr, unsigned opcode)
{
    lir_compile_data_header_t *inst = (lir_compile_data_header_t *)ptr;
    inst->id = 0;
    inst->flag = 0;
    inst->opcode = opcode;
    return inst;
}

static int AddInst(TraceRecorder *Rec, lir_compile_data_header_t *inst,
                   size_t inst_size);

#define LIR_NEWINST(T) ((T *)lir_inst_init(alloca(sizeof(T)), OPCODE_##T))
#define LIR_NEWINST_N(T, SIZE) \
    ((T *)lir_inst_init(alloca(sizeof(T) + sizeof(reg_t) * (SIZE)), OPCODE_##T))

#define ADD_INST(REC, INST) ADD_INST_N(REC, INST, 0)

#define ADD_INST_N(REC, INST, SIZE) \
    AddInst(REC, &(INST)->base, sizeof(*INST) + sizeof(reg_t) * (SIZE))

#include "gwir.c"

static int elimnate_guard(TraceRecorder *Rec, lir_compile_data_header_t *inst)
{
    /* Remove guard that always true
     * If we think following code, L2 is always true. So we can remove L2.
     * L1 = LoadConstFixnum 10
     * L2 = GuardTypeFixnum L1 exit_pc
     */
    IGuardTypeFixnum *guard = (IGuardTypeFixnum *) inst;
    lir_compile_data_header_t *src = FindLIRById(Rec, guard->R);

    if (src == NULL) {
        return 0;
    }

#define RETURN_IF(INST1, OP1, INST2, OP2) \
    if ((INST1)->opcode == OPCODE_I##OP1 && (INST2)->opcode == OPCODE_I##OP2) {\
        return 1;\
    }


    RETURN_IF(inst, GuardTypeFixnum, src, LoadConstFixnum);
    RETURN_IF(inst, GuardTypeFloat,  src, LoadConstFloat);
    RETURN_IF(inst, GuardTypeRegexp, src, LoadConstRegexp);

    if (inst->opcode == OPCODE_IGuardTypeFlonum) {
        if (src->opcode == OPCODE_ILoadConstFloat) {
            if (FLONUM_P(((ILoadConstFloat*)src)->Val)) {
                return 1;
            }
        }
    }

    if (inst->opcode == OPCODE_IGuardTypeArray) {
        if (src->opcode == OPCODE_IAllocArray) {
            return 1;
        }
    }

    if (inst->opcode == OPCODE_IGuardTypeString) {
        if (src->opcode == OPCODE_ILoadConstString) {
            return 1;
        }
        if (src->opcode == OPCODE_IAllocString) {
            return 1;
        }
    }

    if (inst->opcode == OPCODE_IGuardTypeSpecialConst) {
        switch (src->opcode) {
        case OPCODE_ILoadConstNil     :
        case OPCODE_ILoadConstBoolean :
        case OPCODE_ILoadConstFixnum  :
        case OPCODE_ILoadConstFloat   :
        case OPCODE_IGuardTypeNil     :
        case OPCODE_IGuardTypeNonNil  :
            return 1;
        default:
            break;
        }
    }

    return 0;
}

static int peephole(TraceRecorder *Rec, lir_compile_data_header_t *inst)
{
    switch (inst->opcode) {
    case OPCODE_IGuardTypeFixnum      :
    case OPCODE_IGuardTypeFloat       :
    case OPCODE_IGuardTypeFlonum      :
    case OPCODE_IGuardTypeSpecialConst:
    case OPCODE_IGuardTypeArray       :
    case OPCODE_IGuardTypeString      :
    case OPCODE_IGuardTypeHash        :
    case OPCODE_IGuardTypeRegexp      :
    case OPCODE_IGuardTypeTime        :
    case OPCODE_IGuardTypeMath        :
    case OPCODE_IGuardTypeObject      :
    case OPCODE_IGuardTypeNil         :
    case OPCODE_IGuardTypeNonNil      :
        if (elimnate_guard(Rec, inst)) {
            return -1;
        }
    default:
        break;
    }
    return 0;
}

static int lir_inst_define_value(int opcode)
{
#define DEF_IR(OPNAME)     \
case OPCODE_I##OPNAME: \
                       return GWIR_USE_##OPNAME;
    switch (opcode) {
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

static lir_compile_data_header_t *CreateInst(TraceRecorder *Rec,
        lir_compile_data_header_t *inst,
        size_t inst_size)
{
    lir_compile_data_header_t *buf = lir_alloc(Rec, inst_size);
    int newid = buf->id;
    memcpy(buf, inst, inst_size);
    buf->id = newid;
    assert(buf->id != 0);
    return buf;
}

static int AddInst(TraceRecorder *Rec, lir_compile_data_header_t *inst,
        size_t inst_size)
{
    lir_compile_data_header_t *buf;
    int opt;
    BasicBlock *bb = Rec->Block;
    if (!RJitModeIs(Rec->jit, TRACE_MODE_RECORD)) {
        return -1;
    }

    opt = peephole(Rec, inst);
    if (opt != 0) {
        return opt;
    }

    if (bb->size == bb->capacity) {
        unsigned newsize = bb->capacity * 2;
        bb->Insts = (lir_compile_data_header_t **)lir_realloc(
                Rec, bb->Insts, sizeof(lir_compile_data_header_t *) * bb->capacity,
                sizeof(lir_compile_data_header_t *) * newsize);
        bb->capacity = newsize;
    }

    buf = CreateInst(Rec, inst, inst_size);
    bb->Insts[bb->size] = buf;
    bb->size += 1;
#if DUMP_LIR > 1
    dump_lir_inst(buf);
#endif
    return buf->id;
}

#if DUMP_LIR > 0
static void dump_lir_inst(lir_compile_data_header_t *Inst)
{
#define DUMP_IR(OPNAME)      \
case OPCODE_I##OPNAME:   \
                         Dump_##OPNAME(Inst); \
    break;
    switch (Inst->opcode) {
        GWIR_EACH(DUMP_IR);
    default:
        assert(0 && "unreachable");
    }
#undef DUMP_IR
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

static void dump_side_exit(TraceRecorder *Rec)
{
    int i;
    hashmap_iterator_t itr = { 0, 0 };
    while (hashmap_next(&TraceRecorderGetTrace(Rec)->SideExit, &itr)) {
        VALUE *pc = (VALUE *)itr.entry->key;
        StackMap *stack = GetStackMap(Rec, pc);
        fprintf(stderr, "side exit(%d): pc=%p: %s ", stack->size, pc,
                TraceStatusToStr(stack->flag));
        for (i = 0; i < stack->size; i++) {
            fprintf(stderr, "  [%d] = %04ld;", i, stack->regs[i]);
        }
        fprintf(stderr, "\n");
    }
}
#endif /* DUMP_LIR > 0 */

static void dump_lir(TraceRecorder *Rec)
{
#if DUMP_LIR > 0
    BasicBlock *entry = Rec->EntryBlock;
    BasicBlock *block = entry;
    fprintf(stderr, "---------------\n");
    while (block != NULL) {
        dump_lir_block(block);
        block = (BasicBlock *)block->base.next;
    }
    fprintf(stderr, "---------------\n");
    dump_side_exit(Rec);
    fprintf(stderr, "---------------\n");
#endif
}

static int get_opcode(rb_control_frame_t *cfp, VALUE *pc);

static void dump_inst(rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
#if DUMP_INST > 0
    long pc = GET_PC_COUNT();
    int op = get_opcode(reg_cfp, reg_pc);
    fprintf(stderr, "%04ld pc=%p %02d %s\n", pc, reg_pc, op, rb_insns_name(op));
#endif
}
