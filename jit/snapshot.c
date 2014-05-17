/**********************************************************************

  snapshot.c -

  $Author$

  Copyright (C) 2014 Masahiro Ide

 **********************************************************************/

typedef struct Stackmap {
  size_t size;
  reg_t regs[0];
} StackMap;

static StackMap *GetStackMap(lir_builder_t *builder, VALUE *pc)
{
  return (StackMap *) hashmap_get(&builder->Current->SideExit, pc);
}

static void AddStackMap(lir_builder_t *builder, VALUE *pc, StackMap *map)
{
#if 0
  StackMap *old = GetStackMap(builder, pc);
  assert(old == NULL);
#endif
  hashmap_set(&builder->Current->SideExit, pc, (struct Trace *) map);
}

static void DeleteStackMap(void *map)
{
  free(map);
}

static void TakeStackSnapshot(lir_builder_t *builder, VALUE *PC)
{
#if DUMP_STACK_MAP > 0
  fprintf(stderr, "\t\ts:pc=%p:top=%d bottom=%d, %d\n",
          PC,
          0/*builder->stack_top*/, builder->stack_bottom,
          builder->RegStackSize
         );
#endif
  int begin = builder->stack_bottom;
  int end   = builder->RegStackSize;
  //int end   = builder->stack_top < builder->RegStackSize
  //    ? builder->stack_top : builder->RegStackSize;
  int i, n = end - begin;
  StackMap *map = (StackMap *) malloc(sizeof(StackMap) + sizeof(reg_t) * n);
  map->size = n;
  for (i = 0; i < n; i++) {
    reg_t reg = builder->RegStack[i];
#if DUMP_STACK_MAP > 0
    fprintf(stderr, "\t\t[%d] reg=%ld\n", i, reg);
#endif
    map->regs[i] = reg;
  }
  AddStackMap(builder, PC, map);
}

static void record_stack_bottom(lir_builder_t *builder, int opcode, VALUE *pc)
{
  //FIXME
  // typeof concatstrings's operand is rb_num_t not Fixnum.
  // but insn_stack_increase() assume typeof the operand is Fixnum
#if 0
  int depth = insn_stack_increase(0, opcode, pc + 1);
  builder->stack_top += depth;
  if (builder->stack_top < builder->stack_bottom) {
    builder->stack_bottom = builder->stack_top;
  }
#if DUMP_STACK_MAP > 1
  fprintf(stderr, "\t\tr:top=%d bottom=%d, %d\n",
          builder->stack_top, builder->stack_bottom,
          builder->RegStackSize
         );
#endif
#endif
}

static reg_t PopRegister(lir_builder_t *builder)
{
  reg_t Reg;
  assert(builder->RegStackSize > 0);
  builder->RegStackSize -= 1;
  Reg = builder->RegStack[builder->RegStackSize];
#if DUMP_STACK_MAP > 1
  fprintf(stderr, "pop : %d %ld\n", builder->RegStackSize, Reg);
#endif
  return Reg;
}

static void PushRegister(lir_builder_t *builder, reg_t Reg)
{
  if (builder->RegStackSize == builder->RegStackCapacity) {
    unsigned newsize = builder->RegStackCapacity * 2;
    builder->RegStack =
        (reg_t *) lir_realloc(builder, builder->RegStack,
                              sizeof(reg_t) * builder->RegStackCapacity,
                              sizeof(reg_t) * newsize);
    builder->RegStackCapacity = newsize;
  }
  assert(Reg >= 0);
  builder->RegStack[builder->RegStackSize] = Reg;
  builder->RegStackSize += 1;
#if DUMP_STACK_MAP > 1
  fprintf(stderr, "push: %d %ld\n", builder->RegStackSize, Reg);
#endif
}

static reg_t TopRegister(lir_builder_t *builder, long n)
{
  assert(builder->RegStackSize > n && n >= 0);
  reg_t Reg = builder->RegStack[builder->RegStackSize - n - 1];
#if DUMP_STACK_MAP > 1
  fprintf(stderr, "top : %ld %ld\n", n, Reg);
#endif
  return Reg;
}

//static void SetRegister(lir_builder_t *builder, long n, int Reg)
//{
//  assert(builder->RegStackSize > n && n >= 0);
//#if DUMP_STACK_MAP > 1
//  fprintf(stderr, "set : %ld %d\n", builder->RegStackSize - n - 1, Reg);
//#endif
//  builder->RegStack[builder->RegStackSize - n] = Reg;
//}

// call stack
static void PopCallStack(lir_builder_t *builder)
{
  int i;
  struct call_stack_struct *cs;

  assert(builder->CallStackSize > 0);
  builder->CallStackSize -= 1;
  cs = &builder->CallStack[builder->CallStackSize];
  while (cs->regstack_size != builder->RegStackSize) {
    PopRegister(builder);
  }

  for (i = 0; i < cs->method_argc; i++) {
    PopRegister(builder);
  }

#if DUMP_CALL_STACK_MAP > 0
  fprintf(stderr, "call pop: %d %d\n",
          builder->CallStackSize, builder->RegStackSize);
#endif
}

static void PushCallStack(lir_builder_t *builder, int argc, reg_t args[])
{
  int i;
  if (builder->CallStackSize == builder->CallStackCapacity) {
    unsigned newsize = builder->CallStackCapacity * 2;
    builder->CallStack = (struct call_stack_struct *)
        lir_realloc(builder, builder->CallStack,
                    sizeof(struct call_stack_struct) * builder->CallStackCapacity,
                    sizeof(struct call_stack_struct) * newsize);
    builder->CallStackCapacity = newsize;
  }

  builder->CallStack[builder->CallStackSize].regstack_size = builder->RegStackSize;
  builder->CallStack[builder->CallStackSize].method_argc   = argc;
  builder->CallStackSize += 1;

  for (i = 0; i < argc; i++) {
    PushRegister(builder, args[i]);
  }

#if DUMP_CALL_STACK_MAP > 0
  fprintf(stderr, "call push: %d %d\n",
          builder->CallStackSize, builder->RegStackSize);
#endif
}
