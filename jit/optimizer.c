//
//typedef struct BasicBlock BasicBlock;
//
//typedef struct lir_compile_data_header {
//    unsigned opcode : 10;
//    unsigned flag : 22;
//    int id;
//} lir_compile_data_header_t;
//
//
//typedef struct lir_inst_t {
//    lir_compile_data_header_t base;
//    BasicBlock *parent;
//    lir_list_t *use;
//    lir_list_t *user;
//};
//
//typedef int (*worklist_func_t)(worklist_t *, irbuilder_t *, lir_inst_t *);
//
//static int apply_worklist(irbuilder_t *builder, BasicBlock *entry_block, worklist_func_t func)
//{
//    worklist_t worklist;
//    worklist_init(&worklist, entry_block);
//    int eliminated = 0;
//    while (!worklist_empty(worklist)) {
//        inst = worklist_pop(worklist);
//        if (inst->use && inst->use->size > 0) {
//            eliminated += func(worklist, builder, inst);
//        }
//    }
//    return eliminated;
//}
//
//static int inst_is_dead(lir_inst_t *inst)
//{
//    if (inst->use && inst->use->size > 0) {
//        return 0;
//    }
//    if (is_terminator(inst)) {
//        return 0;
//    }
//    if (is_guard(inst)) {
//        return 0;
//    }
//    if (!has_sideeffect(inst)) {
//        return 1;
//    }
//    return 0;
//}
//
//
//static int eliminate_dead_code(worklist_t *list, irbuilder_t *builder, lir_inst_t *ir)
//{
//    int i;
//    int is_dead = inst_is_dead(inst);
//    if (!is_dead) {
//        return 0;
//    }
//    for (i = 0; i < inst->use->size; i++) {
//        worklist_push(worklist, inst->use->entry[i]);
//    }
//    remove_from_parent(ir);
//    return 1;
//}
//
//static lir_inst_t *constant_fold_inst(irbuilder_t *builder, lir_inst_t *inst)
//{
//    if (is_guard(inst) || is_terminator(inst)) {
//        return inst;
//    }
//    inst_folder_t folder = get_inst_folder(inst->opcode);
//    if (inst_operand_size(inst->opcode) == 1) {
//        //switch (inst->opcode) {
//        //case OPCODE_FixnumComplement :
//        //case OPCODE_FixnumToFloat :
//        //case OPCODE_FixnumToString :
//        //case OPCODE_FloatToFixnum :
//        //case OPCODE_FloatToString :
//        //case OPCODE_StringToFixnum :
//        //case OPCODE_StringToFloat :
//        //case OPCODE_MathSin :
//        //case OPCODE_MathCos :
//        //case OPCODE_MathTan :
//        //case OPCODE_MathExp :
//        //case OPCODE_MathSqrt :
//        //case OPCODE_MathLog10 :
//        //case OPCODE_MathLog2 :
//        //case OPCODE_StringLength :
//        //case OPCODE_StringEmptyP :
//        //case OPCODE_ArrayLength :
//        //case OPCODE_ArrayEmptyP :
//        //case OPCODE_ArrayGet :
//        //case OPCODE_HashLength :
//        //case OPCODE_HashEmptyP :
//        //case OPCODE_HashGet :
//        //}
//    }
//    else if (inst_operand_size(inst->opcode) == 2) {
//        switch (inst->opcode) {
//        case OPCODE_FixnumAdd :
//        case OPCODE_FixnumSub :
//        case OPCODE_FixnumMul :
//        case OPCODE_FixnumDiv :
//        case OPCODE_FixnumMod :
//        case OPCODE_FixnumAddOverflow :
//        case OPCODE_FixnumSubOverflow :
//        case OPCODE_FixnumMulOverflow :
//        case OPCODE_FixnumDivOverflow :
//        case OPCODE_FixnumModOverflow :
//        case OPCODE_FixnumEq :
//        case OPCODE_FixnumNe :
//        case OPCODE_FixnumGt :
//        case OPCODE_FixnumGe :
//        case OPCODE_FixnumLt :
//        case OPCODE_FixnumLe :
//        case OPCODE_FixnumAnd :
//        case OPCODE_FixnumOr :
//        case OPCODE_FixnumXor :
//        case OPCODE_FixnumLshift :
//        case OPCODE_FixnumRshift :
//            return fold_binop_fixnum2(builder, folder, inst);
//        case OPCODE_FloatAdd :
//        case OPCODE_FloatSub :
//        case OPCODE_FloatMul :
//        case OPCODE_FloatDiv :
//        case OPCODE_FloatMod :
//        case OPCODE_FloatEq :
//        case OPCODE_FloatNe :
//        case OPCODE_FloatGt :
//        case OPCODE_FloatGe :
//        case OPCODE_FloatLt :
//        case OPCODE_FloatLe :
//            return fold_binop_float2(builder, folder, inst);
//        case OPCODE_StringConcat :
//        case OPCODE_ArrayConcat :
//        case OPCODE_RegExpMatch :
//            break;
//        default :
//            break;
//        }
//    }
//    return inst;
//}
//
//static int constant_fold(worklist_t *list, irbuilder_t *builder, lir_inst_t *ir)
//{
//    lir_inst_t *newinst = constant_fold_inst(builder, inst);
//    if (inst != newinst) {
//        for (i = 0; i < use->size; i++) {
//            worklist_push(worklist, );
//        }
//        inst_replace_with(inst, newinst);
//    }
//}
//
//static int gwir_optimize(irbuilder_t *builder, BasicBlock *entry_block)
//{
//    link_basic_block(builder, entry_block);
//    compute_usedef(builder, entry_block);
//    apply_worklist(builder, entry_block, constant_fold);
//    apply_worklist(builder, entry_block, eliminate_dead_code);
//    return 0;
//}
//

static VALUE *get_target_pc_if_terminator(lir_inst_t *inst)
{
    switch (lir_opcode(inst)) {
    case OPCODE_IJump:
        return ((IJump *)inst)->TargetBB;
        break;
    case OPCODE_IJumpIf:
        return ((IJumpIf *)inst)->TargetBB;
        break;
    default:
        break;
    }
    return NULL;
}

static void link_basic_block(TraceRecorder *Rec, BasicBlock *entry_block)
{
    BasicBlock *target, *block = entry_block;
    lir_inst_t *inst;
    VALUE *pc;
    while (block != NULL) {
        assert(block->size > 0);
        inst = block->Insts[block->size - 1];
        pc = get_target_pc_if_terminator(inst);
        assert(pc != NULL);
        target = FindBasicBlockByPC(Rec, pc);
        ((IJump *)inst)->TargetBB = (VALUEPtr)target;
        inst->base.flag |= FLAG_BLOCK;
        block = (BasicBlock *)block->base.next;
    }
}

static void trace_optimize(TraceRecorder *Rec, Trace *trace)
{
    link_basic_block(Rec, Rec->EntryBlock);
}
