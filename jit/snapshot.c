/**********************************************************************

  snapshot.c -

  $Author$

  Copyright (C) 2014 Masahiro Ide

 **********************************************************************/

static StackMap *GetStackMap(TraceRecorder *Rec, VALUE *pc)
{
    hashmap_t *map = &TraceRecorderGetTrace(Rec)->SideExit;
    return (StackMap *)hashmap_get(map, (hashmap_data_t)pc);
}

static void AddStackMap(TraceRecorder *Rec, VALUE *pc, StackMap *stack)
{
    if (RJitModeIs(Rec->jit, TRACE_MODE_RECORD)) {
        hashmap_t *map = &TraceRecorderGetTrace(Rec)->SideExit;
        hashmap_set(map, (hashmap_data_t)pc, (hashmap_data_t)stack);
    }
}

static void TraceRecorderRecordBottom(TraceRecorder *Rec, int current)
{
    if (current < Rec->RegStackBottom) {
        Rec->RegStackBottom = current;
    }
}

static void TakeStackSnapshot(TraceRecorder *Rec, VALUE *PC)
{
#if DUMP_STACK_MAP > 0
    fprintf(stderr, "\t\ts:pc=%p:top=%d bottom=%d, %d\n", PC,
            0 /*Rec->stack_top*/, Rec->StackBottom, Rec->RegStackSize);
#endif
    int begin = Rec->RegStackBottom;
    int end = Rec->RegStackSize;
    int i, j, n = end - begin;
    StackMap *map;
    if ((map = GetStackMap(Rec, PC))) {
        DeleteStackMap(map);
    }

    map = (StackMap *)malloc(sizeof(StackMap) + sizeof(reg_t) * n);
    map->size = n;
    map->flag = TRACE_EXIT_SIDE_EXIT;
    if (RJitModeIs(Rec->jit, TRACE_MODE_EMIT_BACKWARD_BRANCH)) {
        map->flag = TRACE_EXIT_SUCCESS;
    }

    for (i = begin, j = 0; i < end; i++) {
        reg_t reg = Rec->RegStack[i];
#if DUMP_STACK_MAP > 0
        fprintf(stderr, "\t\t[%d] reg=%ld\n", i, reg);
#endif
        map->regs[j++] = reg;
    }
    AddStackMap(Rec, PC, map);
}

static reg_t AdjustStack(TraceRecorder *Rec)
{
    reg_t Reg;
    IStackAdjust *sa = (IStackAdjust *)Rec->EntryBlock->Insts[0];
    BasicBlock *PrevBB = Rec->Block;
    Rec->Block = Rec->EntryBlock;
    Reg = Emit_StackPop(Rec);
    assert(sa->base.opcode == OPCODE_IStackAdjust);

    /*
     *      before     |   after
     * L0: StackAdjust | StackAdjust
     * L1: StackPop(1) | StackPop(1)
     * L2: StackPop(0) | StackPop(0)
     * L3: Jump Block  | StackPop(2)
     * L4: StackPop(1) | Jump Block
     */
    int jumpIdx = Rec->EntryBlock->size - 1;
    lir_compile_data_header_t **Insts = Rec->EntryBlock->Insts;
    lir_compile_data_header_t *last, *jump;
    jump = Insts[jumpIdx - 1];
    last = Insts[jumpIdx];
    Insts[jumpIdx - 1] = last;
    Insts[jumpIdx] = jump;
    Rec->Block = PrevBB;
    assert(Rec->EntryBlock);
#if 0
  /*
   *      before     |   after
   * L0: StackAdjust | StackAdjust
   * L1: StackPop(1) | StackPop(2)
   * L2: StackPop(0) | StackPop(1)  <- jumpIdx - 2
   * L3: StackPop(2) | StackPop(0)  <- jumpIdx - 1
   * L4: Jump Block  | Jump Block   <- jumpIdx
   */
  if (jumpIdx > 2) {
    // Insts.insert(1, last)
    lir_compile_data_header_t *prev = Insts[1];
    int i;
    for (i = 2; i < jumpIdx; i++) {
      lir_compile_data_header_t *tmp = Insts[i];
      Insts[i] = prev;
      prev     = tmp;
    }
    Insts[1] = last;
  }
#endif
    return Reg;
}

static void PushRegister(TraceRecorder *Rec, reg_t Reg)
{
    if (Rec->RegStackSize + GWIR_RESERVED_REGSTACK_SIZE
        == Rec->RegStackCapacity) {
        unsigned newsize = Rec->RegStackCapacity * 2;
        reg_t *RegStack = (reg_t *)lir_realloc(
            Rec, Rec->RegStack - GWIR_RESERVED_REGSTACK_SIZE,
            sizeof(reg_t) * Rec->RegStackCapacity, sizeof(reg_t) * newsize);
        Rec->RegStack = RegStack + GWIR_RESERVED_REGSTACK_SIZE;
        Rec->RegStackCapacity = newsize;
    }
    assert(Reg >= 0);
    Rec->RegStack[Rec->RegStackSize] = Reg;
    Rec->RegStackSize += 1;
    TraceRecorderRecordBottom(Rec, Rec->RegStackSize);
#if DUMP_STACK_MAP > 1
    fprintf(stderr, "push: %d %ld\n", Rec->RegStackSize, Reg);
#endif
}

static reg_t PopRegister(TraceRecorder *Rec)
{
    reg_t Reg;
    if (Rec->RegStackSize == -1 * GWIR_RESERVED_REGSTACK_SIZE) {
        Event *e = Rec->CurrentEvent;
        TraceRecorderAbort(Rec, e->cfp, e->pc, TRACE_ERROR_REGSTACK_UNDERFLOW);
    }
    Rec->RegStackSize -= 1;
    TraceRecorderRecordBottom(Rec, Rec->RegStackSize);
    Reg = Rec->RegStack[Rec->RegStackSize];
    if (Reg <= 0) {
        Reg = AdjustStack(Rec);
    }
#if DUMP_STACK_MAP > 1
    fprintf(stderr, "pop : %d %ld\n", Rec->RegStackSize, Reg);
    assert(Reg != 0);
#endif
    return Reg;
}

static reg_t TopRegister(TraceRecorder *Rec, int n)
{
    int i, idx = Rec->RegStackSize - n - 1;
    assert(idx < Rec->RegStackSize && idx > -1 * GWIR_RESERVED_REGSTACK_SIZE);
    reg_t Reg = Rec->RegStack[idx];
    TraceRecorderRecordBottom(Rec, idx);
    if (Reg == 0 && idx < 0) {
        for (i = idx; i < 0; i++) {
            if (Rec->RegStack[i] == 0) {
                Rec->RegStack[i] = AdjustStack(Rec);
            }
        }
        Reg = Rec->RegStack[idx];
    }
#if DUMP_STACK_MAP > 1
    fprintf(stderr, "top : %ld %ld\n", idx, Reg);
#endif
    assert(Reg != 0);
    return Reg;
}

static void SetRegister(TraceRecorder *Rec, int n, reg_t Reg)
{
    int idx = Rec->RegStackSize - n - 1;
    assert(idx < Rec->RegStackSize && idx > -1 * GWIR_RESERVED_REGSTACK_SIZE);
    TraceRecorderRecordBottom(Rec, idx);
#if DUMP_STACK_MAP > 1
    fprintf(stderr, "set : %ld %ld\n", idx, Reg);
#endif
    assert(Reg != 0);
    Rec->RegStack[idx] = Reg;
}

// call stack
static void PopCallStack(TraceRecorder *Rec)
{
    int i;
    struct call_stack_struct *cs;

    assert(Rec->CallStackSize > 0);
    Rec->CallStackSize -= 1;
    cs = &Rec->CallStack[Rec->CallStackSize];
    while (cs->regstack_size != Rec->RegStackSize) {
        PopRegister(Rec);
    }

    for (i = 0; i < cs->method_argc; i++) {
        PopRegister(Rec);
    }

#if DUMP_CALL_STACK_MAP > 0
    fprintf(stderr, "call pop: %d %d\n", Rec->CallStackSize, Rec->RegStackSize);
#endif
}

static void PushCallStack(TraceRecorder *Rec, int argc, reg_t args[])
{
    int i;
    if (Rec->CallStackSize == Rec->CallStackCapacity) {
        unsigned newsize = Rec->CallStackCapacity * 2;
        Rec->CallStack = (struct call_stack_struct *)lir_realloc(
            Rec, Rec->CallStack,
            sizeof(struct call_stack_struct) * Rec->CallStackCapacity,
            sizeof(struct call_stack_struct) * newsize);
        Rec->CallStackCapacity = newsize;
    }

    Rec->CallStack[Rec->CallStackSize].regstack_size = Rec->RegStackSize;
    Rec->CallStack[Rec->CallStackSize].method_argc = argc;
    Rec->CallStackSize += 1;

    for (i = 0; i < argc; i++) {
        PushRegister(Rec, args[i]);
    }

#if DUMP_CALL_STACK_MAP > 0
    fprintf(stderr, "call push: %d %d\n", Rec->CallStackSize,
            Rec->RegStackSize);
#endif
}
