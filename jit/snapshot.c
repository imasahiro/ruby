/**********************************************************************

  snapshot.c -

  $Author$

  Copyright (C) 2014 Masahiro Ide

 **********************************************************************/

static StackMap *GetStackMap(TraceRecorder *Rec, VALUE *pc)
{
  return (StackMap *) hashmap_get(&TraceRecorderGetTrace(Rec)->SideExit, pc);
}

static void AddStackMap(TraceRecorder *Rec, VALUE *pc, StackMap *map)
{
  hashmap_set(&TraceRecorderGetTrace(Rec)->SideExit, pc, (struct Trace *) map);
}

static void TakeStackSnapshot(TraceRecorder *Rec, VALUE *PC)
{
#if DUMP_STACK_MAP > 0
  fprintf(stderr, "\t\ts:pc=%p:top=%d bottom=%d, %d\n",
          PC,
          0/*Rec->stack_top*/, Rec->StackBottom,
          Rec->RegStackSize
         );
#endif
  int begin = Rec->StackBottom;
  int end   = Rec->RegStackSize;
  int i, n = end - begin;
  StackMap *map;
  if ((map = GetStackMap(Rec, PC))) {
    DeleteStackMap(map);
  }

  map = (StackMap *) malloc(sizeof(StackMap) + sizeof(reg_t) * n);
  map->size = n;
  map->flag = TRACE_EXIT_SIDE_EXIT;
  if (RJitModeIs(Rec->jit, TRACE_MODE_EMIT_BACKWARD_BRANCH)) {
    map->flag = TRACE_EXIT_SUCCESS;
  }
  for (i = 0; i < n; i++) {
    reg_t reg = Rec->RegStack[i];
#if DUMP_STACK_MAP > 0
    fprintf(stderr, "\t\t[%d] reg=%ld\n", i, reg);
#endif
    map->regs[i] = reg;
  }
  AddStackMap(Rec, PC, map);
}

static void record_stack_bottom(TraceRecorder *Rec, int opcode, VALUE *pc)
{
  //FIXME
  // typeof concatstrings's operand is rb_num_t not Fixnum.
  // but insn_stack_increase() assume typeof the operand is Fixnum
#if 0
  int depth = insn_stack_increase(0, opcode, pc + 1);
  Rec->stack_top += depth;
  if (Rec->stack_top < Rec->StackBottom) {
    Rec->StackBottom = Rec->stack_top;
  }
#if DUMP_STACK_MAP > 1
  fprintf(stderr, "\t\tr:top=%d bottom=%d, %d\n",
          Rec->stack_top, Rec->StackBottom,
          Rec->RegStackSize
         );
#endif
#endif
}

static void PushRegister(TraceRecorder *Rec, reg_t Reg)
{
  if (Rec->RegStackSize == Rec->RegStackCapacity) {
    unsigned newsize = Rec->RegStackCapacity * 2;
    Rec->RegStack =
        (reg_t *) lir_realloc(Rec, Rec->RegStack,
                              sizeof(reg_t) * Rec->RegStackCapacity,
                              sizeof(reg_t) * newsize);
    Rec->RegStackCapacity = newsize;
  }
  assert(Reg >= 0);
  Rec->RegStack[Rec->RegStackSize] = Reg;
  Rec->RegStackSize += 1;
#if DUMP_STACK_MAP > 1
  fprintf(stderr, "push: %d %ld\n", Rec->RegStackSize, Reg);
#endif
}

static reg_t PopRegister(TraceRecorder *Rec)
{
  reg_t Reg;
  if (Rec->RegStackSize == 0) {
    PushRegister(Rec, Emit_StackPop(Rec));
  }
  assert(Rec->RegStackSize > 0);
  Rec->RegStackSize -= 1;
  Reg = Rec->RegStack[Rec->RegStackSize];
#if DUMP_STACK_MAP > 1
  fprintf(stderr, "pop : %d %ld\n", Rec->RegStackSize, Reg);
#endif
  return Reg;
}


static reg_t TopRegister(TraceRecorder *Rec, long n)
{
  assert(Rec->RegStackSize > n && n >= 0);
  reg_t Reg = Rec->RegStack[Rec->RegStackSize - n - 1];
#if DUMP_STACK_MAP > 1
  fprintf(stderr, "top : %ld %ld\n", n, Reg);
#endif
  return Reg;
}

static void SetRegister(TraceRecorder *Rec, long n, reg_t Reg)
{
  assert(Rec->RegStackSize > n && n >= 0);
#if DUMP_STACK_MAP > 1
  fprintf(stderr, "set : %ld %ld\n", Rec->RegStackSize - n - 1, Reg);
#endif
  Rec->RegStack[Rec->RegStackSize - n - 1] = Reg;
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
  fprintf(stderr, "call pop: %d %d\n",
          Rec->CallStackSize, Rec->RegStackSize);
#endif
}

static void PushCallStack(TraceRecorder *Rec, int argc, reg_t args[])
{
  int i;
  if (Rec->CallStackSize == Rec->CallStackCapacity) {
    unsigned newsize = Rec->CallStackCapacity * 2;
    Rec->CallStack = (struct call_stack_struct *)
        lir_realloc(Rec, Rec->CallStack,
                    sizeof(struct call_stack_struct) * Rec->CallStackCapacity,
                    sizeof(struct call_stack_struct) * newsize);
    Rec->CallStackCapacity = newsize;
  }

  Rec->CallStack[Rec->CallStackSize].regstack_size = Rec->RegStackSize;
  Rec->CallStack[Rec->CallStackSize].method_argc   = argc;
  Rec->CallStackSize += 1;

  for (i = 0; i < argc; i++) {
    PushRegister(Rec, args[i]);
  }

#if DUMP_CALL_STACK_MAP > 0
  fprintf(stderr, "call push: %d %d\n",
          Rec->CallStackSize, Rec->RegStackSize);
#endif
}
