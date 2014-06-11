/**********************************************************************

  optimizer.c -

  $Author$

  Copyright (C) 2014 Masahiro Ide

 **********************************************************************/

typedef VALUE (*lir_folder1_t)(VALUE);
typedef VALUE (*lir_folder2_t)(VALUE, VALUE);

static int is_guard(lir_inst_t *inst)
{
    switch (lir_opcode(inst)) {
    case OPCODE_IGuardTypeFixnum:
    case OPCODE_IGuardTypeFloat:
    case OPCODE_IGuardTypeFlonum:
    case OPCODE_IGuardTypeSpecialConst:
    case OPCODE_IGuardTypeArray:
    case OPCODE_IGuardTypeString:
    case OPCODE_IGuardTypeHash:
    case OPCODE_IGuardTypeRegexp:
    case OPCODE_IGuardTypeTime:
    case OPCODE_IGuardTypeMath:
    case OPCODE_IGuardTypeObject:
    case OPCODE_IGuardTypeNil:
    case OPCODE_IGuardTypeNonNil:
    case OPCODE_IGuardBlockEqual:
    case OPCODE_IGuardObjectEqual:
    case OPCODE_IGuardProperty:
    case OPCODE_IGuardMethodCache:
    case OPCODE_IGuardMethodRedefine:
        return 1;
    default:
        break;
    }
    return 0;
}

static int is_terminator(lir_inst_t *inst)
{
    switch (lir_opcode(inst)) {
    case OPCODE_IExit:
    case OPCODE_IJump:
    case OPCODE_IJumpIf:
    case OPCODE_IThrow:
        return 1;
    default:
        break;
    }
    return 0;
}

static int elimnate_guard(TraceRecorder *Rec, lir_inst_t *inst)
{
    /* Remove guard that always true
     * If we think following code, L2 is always true. So we can remove L2.
     * L1 = LoadConstFixnum 10
     * L2 = GuardTypeFixnum L1 exit_pc
     */
    IGuardTypeFixnum *guard = (IGuardTypeFixnum *)inst;
    lir_inst_t *src = guard->R;

    if (src == NULL) {
        return 0;
    }

#define RETURN_IF(INST1, OP1, INST2, OP2)         \
    if (lir_opcode(INST1) == OPCODE_I##OP1) {     \
        if (lir_opcode(INST2) == OPCODE_I##OP2) { \
            return 1;                             \
        }                                         \
    }

    RETURN_IF(inst, GuardTypeFixnum, src, LoadConstFixnum);
    RETURN_IF(inst, GuardTypeFloat, src, LoadConstFloat);
    RETURN_IF(inst, GuardTypeRegexp, src, LoadConstRegexp);

    if (lir_opcode(inst) == OPCODE_IGuardTypeFlonum) {
        if (lir_opcode(src) == OPCODE_ILoadConstFloat) {
            if (FLONUM_P(((ILoadConstFloat *)src)->Val)) {
                return 1;
            }
        }
    }

    if (lir_opcode(inst) == OPCODE_IGuardTypeArray) {
        if (lir_opcode(src) == OPCODE_IAllocArray) {
            return 1;
        }
    }

    if (lir_opcode(inst) == OPCODE_IGuardTypeString) {
        if (lir_opcode(src) == OPCODE_ILoadConstString) {
            return 1;
        }
        if (lir_opcode(src) == OPCODE_IAllocString) {
            return 1;
        }
    }

    //if (lir_opcode(inst) == OPCODE_IGuardTypeSpecialConst) {
    //    switch (lir_opcode(src)) {
    //    case OPCODE_ILoadConstNil     :
    //    case OPCODE_ILoadConstBoolean :
    //    case OPCODE_ILoadConstFixnum  :
    //    case OPCODE_ILoadConstFloat   :
    //    case OPCODE_IGuardTypeNil     :
    //    case OPCODE_IGuardTypeNonNil  :
    //        return 1;
    //    default:
    //        break;
    //    }
    //}

    return 0;
}

static lir_inst_t *fold_binop_fixnum2(TraceRecorder *Rec, lir_folder_t folder, lir_inst_t *inst)
{
    ILoadConstFixnum *LHS = (ILoadConstFixnum *)*lir_inst_get_args(inst, 0);
    ILoadConstFixnum *RHS = (ILoadConstFixnum *)*lir_inst_get_args(inst, 1);
    int lop = lir_opcode(&LHS->base);
    int rop = lir_opcode(&RHS->base);
    // const + const
    if (lop == OPCODE_ILoadConstFixnum && rop == OPCODE_ILoadConstFixnum) {
        VALUE val = ((lir_folder2_t)folder)(LHS->Val, RHS->Val);
        return Emit_LoadConstFixnum(Rec, val);
    }
    return inst;
}

static lir_inst_t *fold_binop_float2(TraceRecorder *Rec, lir_folder_t folder, lir_inst_t *inst)
{
    IFloatAdd *ir = (IFloatAdd *)inst;
    ILoadConstFloat *LHS = (ILoadConstFloat *)*lir_inst_get_args(inst, 0);
    ILoadConstFloat *RHS = (ILoadConstFloat *)*lir_inst_get_args(inst, 1);
    int lop = lir_opcode(&LHS->base);
    int rop = lir_opcode(&RHS->base);
    // const + const
    if (lop == OPCODE_ILoadConstFloat && rop == OPCODE_ILoadConstFloat) {
        // FIXME need to insert GuardTypeFlonum?
        VALUE val = ((lir_folder2_t)folder)(LHS->Val, RHS->Val);
        return Emit_LoadConstFloat(Rec, val);
    }
    return inst;
}

static lir_t EmitLoadConst(TraceRecorder *Rec, VALUE val);

static lir_inst_t *fold_binop_tostr(TraceRecorder *Rec, lir_folder_t folder, lir_inst_t *inst)
{
    IObjectToString *ir = (IObjectToString *)inst;
    ILoadConstObject *Val = (ILoadConstObject *)*lir_inst_get_args(inst, 0);
    VALUE val = Qundef;
    switch(lir_opcode(&Val->base)) {
    case OPCODE_ILoadConstNil:
    case OPCODE_ILoadConstObject:
    case OPCODE_ILoadConstBoolean:
    case OPCODE_ILoadConstFixnum:
    case OPCODE_ILoadConstFloat:
    case OPCODE_ILoadConstString:
    case OPCODE_ILoadConstRegexp:
        val = ((lir_folder1_t)folder)(Val->Val);
        return EmitLoadConst(Rec, val);
    default:
        break;
    }
    return inst;
}

static lir_inst_t *constant_fold_inst(TraceRecorder *Rec, lir_inst_t *inst)
{
    if (is_guard(inst) || is_terminator(inst)) {
        return inst;
    }
    lir_folder_t folder = const_fold_funcs[lir_opcode(inst)];
    if (folder == NULL) {
        return inst;
    }

    switch (lir_opcode(inst)) {
    //case OPCODE_IObjectToString:
    //    return fold_binop_tostr(Rec, folder, inst);
    //case OPCODE_FixnumComplement :
    //case OPCODE_FixnumToFloat :
    //case OPCODE_FixnumToString :
    //case OPCODE_FloatToFixnum :
    //case OPCODE_FloatToString :
    //case OPCODE_StringToFixnum :
    //case OPCODE_StringToFloat :
    //case OPCODE_MathSin :
    //case OPCODE_MathCos :
    //case OPCODE_MathTan :
    //case OPCODE_MathExp :
    //case OPCODE_MathSqrt :
    //case OPCODE_MathLog10 :
    //case OPCODE_MathLog2 :
    //case OPCODE_StringLength :
    //case OPCODE_StringEmptyP :
    //case OPCODE_ArrayLength :
    //case OPCODE_ArrayEmptyP :
    //case OPCODE_ArrayGet :
    //case OPCODE_HashLength :
    //case OPCODE_HashEmptyP :
    //case OPCODE_HashGet :

    case OPCODE_IFixnumAdd :
    case OPCODE_IFixnumSub :
    case OPCODE_IFixnumMul :
    case OPCODE_IFixnumDiv :
    case OPCODE_IFixnumMod :
    case OPCODE_IFixnumAddOverflow :
    case OPCODE_IFixnumSubOverflow :
    case OPCODE_IFixnumMulOverflow :
    case OPCODE_IFixnumDivOverflow :
    case OPCODE_IFixnumModOverflow :
    case OPCODE_IFixnumEq :
    case OPCODE_IFixnumNe :
    case OPCODE_IFixnumGt :
    case OPCODE_IFixnumGe :
    case OPCODE_IFixnumLt :
    case OPCODE_IFixnumLe :
    case OPCODE_IFixnumAnd :
    case OPCODE_IFixnumOr :
    case OPCODE_IFixnumXor :
    case OPCODE_IFixnumLshift :
    case OPCODE_IFixnumRshift :
        return fold_binop_fixnum2(Rec, folder, inst);
    case OPCODE_IFloatAdd :
    case OPCODE_IFloatSub :
    case OPCODE_IFloatMul :
    case OPCODE_IFloatDiv :
    case OPCODE_IFloatMod :
    case OPCODE_IFloatEq :
    case OPCODE_IFloatNe :
    case OPCODE_IFloatGt :
    case OPCODE_IFloatGe :
    case OPCODE_IFloatLt :
    case OPCODE_IFloatLe :
        return fold_binop_float2(Rec, folder, inst);
    case OPCODE_IStringConcat :
    case OPCODE_IArrayConcat :
    case OPCODE_IRegExpMatch :
        break;
    default :
        break;
    }
    //}
return inst;
}

//static int constant_fold(worklist_t *list, TraceRecorder *builder, lir_inst_t *ir)
//{
//    lir_inst_t *newinst = constant_fold_inst(builder, inst);
//    if (inst != newinst) {
//        for (i = 0; i < use->size; i++) {
//            worklist_push(worklist, );
//        }
//        inst_replace_with(inst, newinst);
//    }
//}

static void trace_optimize(TraceRecorder *Rec, Trace *trace)
{
    //BasicBlock *entry_block = Rec->EntryBlock;
    //compute_usedef(builder, entry_block);
    //apply_worklist(builder, entry_block, constant_fold);
    //loop_invariant_code_motion(builder, entry_block);
    //apply_worklist(builder, entry_block, eliminate_dead_code);
}

//typedef struct lir_inst_t {
//    lir_compile_data_header_t base;
//    BasicBlock *parent;
//    lir_list_t *use;
//    lir_list_t *user;
//};
//
//typedef int (*worklist_func_t)(worklist_t *, TraceRecorder *, lir_inst_t *);
//
//static int apply_worklist(TraceRecorder *builder, BasicBlock *entry_block, worklist_func_t func)
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
//static int eliminate_dead_code(worklist_t *list, TraceRecorder *builder, lir_inst_t *ir)
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
