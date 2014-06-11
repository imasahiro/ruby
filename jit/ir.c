/**********************************************************************

  ir.c -

  $Author$

  Copyright (C) 2014 Masahiro Ide

 **********************************************************************/

#define FMT(T) FMT_##T
#define FMT_int "%d"
#define FMT_long "%ld"
#define FMT_lir_t "%04ld"
#define FMT_LirPtr "%04ld"
#define FMT_ID "%04ld"
#define FMT_VALUE "0x%lx"
#define FMT_VALUEPtr "%p"
#define FMT_voidPtr "%p"
#define FMT_CALL_INFO "%p"
#define FMT_IC "%p"
#define FMT_ISEQ "%p"
#define FMT_BasicBlockPtr "bb:%d"
#define FMT_rb_event_flag_t "%lu"

#define DATA(T, V) DATA_##T(V)
#define DATA_int(V) (V)
#define DATA_long(V) (V)
#define DATA_lir_t(V) (lir_getid(V))
#define DATA_LirPtr(V) (lir_getid(*(V)))
#define DATA_VALUE(V) (V)
#define DATA_VALUEPtr(V) (V)
#define DATA_voidPtr(V) (V)
#define DATA_CALL_INFO(V) (V)
#define DATA_IC(V) (V)
#define DATA_ISEQ(V) (V)
#define DATA_BasicBlockPtr(V) ((V)->base.base.id)
#define DATA_rb_event_flag_t(V) (V)

typedef void *lir_folder_t;

static long lir_getid(lir_inst_t *ir)
{
    return (ir != NULL) ? (long)ir->base.id : 0;
}

static int lir_opcode(lir_inst_t *ir)
{
    return ir->base.opcode;
}

static long block_id(BasicBlock *bb)
{
    return (long)bb->base.base.id;
}

static BasicBlock *CreateBlock(TraceRecorder *Rec, VALUE *pc)
{
    BasicBlock *bb = NULL;

    if (!RJitModeIs(Rec->jit, TRACE_MODE_RECORD)) {
        return bb;
    }

    bb = (BasicBlock *)lir_alloc(Rec, sizeof(BasicBlock));
    // fprintf(stderr, "newblock=(%p, %p)\n", bb, pc);
    bb->base.base.flag = 0;
    bb->base.next = NULL;
    bb->start_pc = pc;
    bb->size = 0;
    bb->capacity = 1;
    bb->Insts = (lir_inst_t **)lir_alloc(Rec, sizeof(lir_inst_t *) * 1);
    if (Rec->Block != NULL) {
        Rec->Block->base.next = (lir_linkedlist_t *)bb;
    }
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

static void *lir_inst_init(void *ptr, unsigned opcode)
{
    lir_inst_t *inst = (lir_inst_t *)ptr;
    inst->base.id = 0;
    inst->base.flag = 0;
    inst->base.opcode = opcode;
    inst->parent = NULL;
    inst->user = NULL;
    return inst;
}

static lir_list_t *lir_list_init(TraceRecorder *Rec)
{
    lir_list_t *list = (lir_list_t *)lir_alloc(Rec, sizeof(*list));
    list->list = (lir_inst_t **)lir_alloc(Rec, sizeof(lir_inst_t *) * 1);
    list->size = 0;
    list->capacity = 1;
    return list;
}

static void lir_list_add(TraceRecorder *Rec, lir_list_t *list, lir_inst_t *ir)
{
    if (list->size == list->capacity) {
        unsigned newsize = list->capacity * 2;
        list->list = (lir_inst_t **)lir_realloc(
            Rec, list->list,
            sizeof(lir_inst_t *) * list->capacity,
            sizeof(lir_inst_t *) * newsize);
        list->capacity = newsize;
    }
    list->list[list->size] = ir;
    list->size += 1;
}

static void lir_inst_adduser(TraceRecorder *Rec, lir_inst_t *inst, lir_inst_t *ir)
{
    if (inst->user) {
        inst->user = lir_list_init(Rec);
    }
    lir_list_add(Rec, inst->user, ir);
}

static lir_inst_t *AddInst(TraceRecorder *Rec, lir_inst_t *inst, size_t inst_size);

#define LIR_NEWINST(T) ((T *)lir_inst_init(alloca(sizeof(T)), OPCODE_##T))
#define LIR_NEWINST_N(T, SIZE) \
    ((T *)lir_inst_init(alloca(sizeof(T) + sizeof(lir_t) * (SIZE)), OPCODE_##T))

#define ADD_INST(REC, INST) ADD_INST_N(REC, INST, 0)

#define ADD_INST_N(REC, INST, SIZE) \
    AddInst(REC, &(INST)->base, sizeof(*INST) + sizeof(lir_t) * (SIZE))

#include "gwir.c"

static int elimnate_guard(TraceRecorder *Rec, lir_inst_t *inst);
static int is_guard(lir_inst_t *inst);

static int peephole(TraceRecorder *Rec, lir_inst_t *inst)
{
    if (is_guard(inst) && elimnate_guard(Rec, inst)) {
        return -1;
    }
    return 0;
}

static lir_inst_t **lir_inst_get_args(lir_inst_t *inst, int idx)
{
#define GET_ARG(OPNAME)    \
    case OPCODE_I##OPNAME: \
        return GetNext_##OPNAME(inst, idx);

    switch (lir_opcode(inst)) {
        GWIR_EACH(GET_ARG);
    default:
        assert(0 && "unreachable");
    }
#undef GET_ARG
    return NULL;
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
static void dump_lir_inst(lir_inst_t *Inst);
#endif /* DUMP_LIR > 0*/

static lir_inst_t *CreateInst(TraceRecorder *Rec, lir_inst_t *inst, size_t inst_size)
{
    lir_inst_t *buf = lir_alloc(Rec, inst_size);
    long newid = lir_getid(buf);
    memcpy(buf, inst, inst_size);
    buf->base.id = (int) newid;
    assert(lir_getid(buf) != 0);
    return buf;
}

static lir_inst_t *constant_fold_inst(TraceRecorder *builder, lir_inst_t *inst);

static lir_inst_t *AddInst(TraceRecorder *Rec, lir_inst_t *inst, size_t inst_size)
{
    int opt;
    BasicBlock *bb = Rec->Block;
    if (!RJitModeIs(Rec->jit, TRACE_MODE_RECORD)) {
        return NULL;
    }

    opt = peephole(Rec, inst);
    if (opt != 0) {
        return NULL;
    }

    if (bb->size == bb->capacity) {
        unsigned newsize = bb->capacity * 2;
        bb->Insts = (lir_inst_t **)lir_realloc(
            Rec, bb->Insts, sizeof(lir_inst_t *) * bb->capacity,
            sizeof(lir_inst_t *) * newsize);
        bb->capacity = newsize;
    }

    lir_inst_t *buf = constant_fold_inst(Rec, inst);
    if (buf == inst) {
        buf = CreateInst(Rec, inst, inst_size);
        bb->Insts[bb->size] = buf;
        bb->size += 1;
    }
#if DUMP_LIR > 1
    dump_lir_inst(buf);
#endif
    return buf;
}

#if DUMP_LIR > 0
static void dump_lir_inst(lir_inst_t *Inst)
{
#define DUMP_IR(OPNAME)      \
case OPCODE_I##OPNAME:   \
                         Dump_##OPNAME(Inst); \
    break;
    switch (lir_opcode(Inst)) {
        GWIR_EACH(DUMP_IR);
    default:
        assert(0 && "unreachable");
    }
#undef DUMP_IR
}

static void dump_lir_block(BasicBlock *block)
{
    unsigned i = 0;
    fprintf(stderr, "BB%ld (pc=%p)\n", block_id(block), block->start_pc);
    for (i = 0; i < block->size; i++) {
        lir_inst_t *Inst = block->Insts[i];
        dump_lir_inst(Inst);
    }
}

static void dump_side_exit(TraceRecorder *Rec)
{
    int i;
    hashmap_iterator_t itr = { 0, 0 };
    while (hashmap_next(&TraceRecorderGetTrace(Rec)->StackMap, &itr)) {
        VALUE *pc = (VALUE *)itr.entry->key;
        StackMap *stack = GetStackMap(Rec, pc);
        fprintf(stderr, "side exit(%d): pc=%p: %s ", stack->size, pc,
                TraceStatusToStr(stack->flag));
        for (i = 0; i < stack->size; i++) {
            lir_inst_t *inst = stack->regs[i];
            fprintf(stderr, "  [%d] = %04ld;", i, lir_getid(inst));
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
