/**********************************************************************

  jit.c -

  $Author$

  Copyright (C) 2014 Masahiro Ide

 **********************************************************************/

#include "ruby/ruby.h"
#include "ruby/vm.h"
#include "ruby/st.h"
#include "method.h"
#include "vm_core.h"
#include "vm_exec.h"
#include "vm_insnhelper.h"
#include "internal.h"
#include "iseq.h"
//#define USE_INSN_STACK_INCREASE 1
#include "insns.inc"
#include "insns_info.inc"

#include "jit.h"
#include "jit_opts.h"

#include "jit_prelude.c"

typedef long reg_t;
#include "kmap.c"

static int trace_mode = -1;

typedef struct lir_builder lir_builder_t;

typedef VALUE *(*native_func_t)(rb_thread_t *th, rb_control_frame_t *reg_cfp, VALUE *reg_pc);

typedef void (*trace_func_t)(lir_builder_t *builder, rb_control_frame_t *reg_cfp, VALUE *reg_pc);

struct Trace {
  long   LoopCount;
  VALUE *StartPC;
  native_func_t  Code;
  struct Trace *Parent;
  const rb_iseq_t *iseq;
  hashmap_t SideExit;
};

struct compiler_info {
  rb_thread_t *th; // current thread;
  struct Trace *Current;
  struct Trace *Parent;
  hashmap_t Traces;
  lir_builder_t *_LirBuilder;
};

typedef struct lir_compile_data_header {
  unsigned opcode:8;
  unsigned flag  :24;
  unsigned id;
  struct lir_compile_data_header *next;
} lir_compile_data_header_t;

typedef struct lir_basic_block {
  lir_compile_data_header_t   base;
  lir_compile_data_header_t **Insts;
  unsigned size;
  unsigned capacity;
  VALUE *start_pc;
} BasicBlock;

struct call_stack_struct {
  int regstack_size;
  int method_argc;
};

struct lir_builder {
  struct Trace *Current;
  BasicBlock   *Block;
  BasicBlock   *EntryBlock;
  lir_compile_data_header_t *buffer_head;
  lir_compile_data_header_t *buffer_root;
  char *buffer_current;
  reg_t *RegStack;
  int    RegStackSize;
  int    RegStackCapacity;

  struct call_stack_struct *CallStack;
  int   CallStackSize;
  int   CallStackCapacity;

  int   ValueId;
  //int   stack_top;
  int   stack_bottom;
  int   CallDepth;
};

enum trace_state {
  TRACE_NONE,
  TRACE_RECORD
};

static struct compiler_info *compiler_info = NULL;
enum trace_error {
  TRACE_OK,
  TRACE_ERROR_NATIVE_METHOD,
  TRACE_ERROR_THROW,
  TRACE_ERROR_SUPPORT_OP,
  TRACE_ERROR_LEAVE,
  TRACE_ERROR_TOO_LONG_TRACE,
  TRACE_ERROR_END
};

static const char *trace_error_message[] = {
  "ok",
  "invoking native method",
  "throw exception",
  "not supported bytecode",
  "this trace return into native method",
  "trace is too long",
  ""
};

static void enable_trace(lir_builder_t *builder) {
  trace_mode = 1;
}

static void disable_trace(rb_control_frame_t *reg_cfp, VALUE *reg_pc, enum trace_error reason) {
  trace_mode = 0;
  if (compiler_info && compiler_info->Current) {
    compiler_info->Current->Code = NULL;
  }
  if (reason != TRACE_OK) {
    const rb_iseq_t *iseq = GET_ISEQ();
    VALUE file = iseq->location.path;
    fprintf(stderr, "failed to trace at file:%s line:%d because %s\n",
            RSTRING_PTR(file),
            rb_iseq_line_no(iseq, reg_pc - iseq->iseq_encoded),
            trace_error_message[reason]);
  }
}

static int is_tracing() {
  return trace_mode == 1;
}

static struct Trace *new_trace(const rb_iseq_t *iseq, VALUE *pc)
{
  struct Trace *trace = malloc(sizeof(*trace));
  trace->LoopCount = 0;
  trace->iseq    = iseq;
  trace->StartPC = pc;
  trace->Code    = NULL;
  trace->Parent  = NULL;
  hashmap_init(&trace->SideExit, 1);
  return trace;
}

static void DeleteStackMap(void *map);

static void delete_trace(void *trace_)
{
  struct Trace *trace = (struct Trace *) trace_;
  hashmap_dispose(&trace->SideExit, DeleteStackMap);
  free(trace);
}

#define BUFF_POS(STORAGE)  ((STORAGE)->id)
#define BUFF_SIZE(STORAGE) ((STORAGE)->flag)
//------------------------

static lir_builder_t *CreateLIRBuilder();

static void init_compiler()
{
  struct compiler_info *cinfo = malloc(sizeof(*cinfo));
  memset(cinfo, 0, sizeof(*cinfo));
  hashmap_init(&cinfo->Traces, GWIR_TRACE_INIT_SIZE);
  cinfo->_LirBuilder = CreateLIRBuilder();
  compiler_info = cinfo;
}

static void delete_compiler()
{
  hashmap_dispose(&compiler_info->Traces, delete_trace);
  free(compiler_info);
}

static void finalize_codegen();

void jit_init()
{
  disable_trace(NULL, NULL, TRACE_OK);
  init_compiler();
  Init_jit();
}

void jit_disable()
{
  finalize_codegen();
  delete_compiler();
}
//------------------------


static lir_compile_data_header_t *lir_alloc(lir_builder_t *builder, size_t size)
{
  lir_compile_data_header_t *ptr;
  lir_compile_data_header_t *storage = builder->buffer_head;
  if (BUFF_POS(storage) + size > BUFF_SIZE(storage)) {
    unsigned alloc_size = BUFF_SIZE(storage) * 2;
    while(alloc_size < size) {
      alloc_size *= 2;
    }
    storage->next = malloc(sizeof(lir_compile_data_header_t) + alloc_size);
    builder->buffer_head = storage = storage->next;
    storage->next = NULL;
    builder->buffer_current  = (char *) (storage + 1);
    BUFF_POS(storage)  = 0;
    BUFF_SIZE(storage) = alloc_size;
  }
  ptr = (lir_compile_data_header_t *)
      (builder->buffer_current + BUFF_POS(storage));
  memset(ptr, 0, size);
  ptr->id = builder->ValueId++;
  BUFF_POS(storage) += (unsigned) size;
  return ptr;
}

static lir_compile_data_header_t *lir_realloc(lir_builder_t *builder, void *oldptr, size_t oldsize, size_t newsize)
{
  void *newptr = NULL;
  if (oldsize >= newsize) {
    return (lir_compile_data_header_t *) oldptr;
  }
  newptr = lir_alloc(builder, newsize);
  memcpy(newptr, oldptr, oldsize);
  return newptr;
}

static void ResetLIRBuilder(lir_builder_t *builder, struct Trace *Current, int AllocNewBuffer)
{
  lir_compile_data_header_t *storage = builder->buffer_head;
  builder->Current    = Current;
  builder->Block      = NULL;
  builder->EntryBlock = NULL;
  builder->ValueId    = 0;
  builder->buffer_head    = NULL;
  builder->buffer_current = NULL;
  while(storage != NULL) {
    lir_compile_data_header_t *next = storage->next;
    free(storage);
    storage = next;
  }
  if (AllocNewBuffer) {
    builder->buffer_head =
        malloc(sizeof(lir_compile_data_header_t) + LIR_COMPILE_DATA_BUFF_SIZE);
    BUFF_POS(builder->buffer_head)  = 0;
    BUFF_SIZE(builder->buffer_head) = LIR_COMPILE_DATA_BUFF_SIZE;
    builder->buffer_head->next = NULL;
  }
  builder->buffer_root    = builder->buffer_head;
  builder->buffer_current = (char *) (builder->buffer_head + 1);

  builder->RegStackSize     = 0;
  builder->RegStackCapacity = 0;
  builder->RegStack = NULL;
  if (AllocNewBuffer) {
    builder->RegStackCapacity = 1;
    builder->RegStack = (reg_t *) lir_alloc(builder, sizeof(reg_t) * 1);
  }

  builder->CallStackSize     = 0;
  builder->CallStackCapacity = 0;
  builder->CallStack = NULL;
  if (AllocNewBuffer) {
    builder->CallStackCapacity = 1;
    builder->CallStack = (struct call_stack_struct *)
        lir_alloc(builder, sizeof(struct call_stack_struct) * 1);
  }

  //builder->stack_top    = 0;
  builder->stack_bottom = 0;
  builder->CallDepth    = 0;
}

static lir_builder_t *CreateLIRBuilder()
{
  lir_builder_t *builder = malloc(sizeof(*builder));
  memset(builder, 0, sizeof(*builder));
  ResetLIRBuilder(builder, NULL, 0);
  return builder;
}

static native_func_t TranslateToNativeCode(lir_builder_t *builder);
static void dump_lir(lir_builder_t *builder);

static native_func_t FlushLIRBuilder(lir_builder_t *builder)
{
  dump_lir(builder);
  native_func_t compiled_code = TranslateToNativeCode(builder);
  ResetLIRBuilder(builder, NULL, 0);
  return compiled_code;
}

static BasicBlock *CreateBlock(lir_builder_t *builder, VALUE *pc)
{
  BasicBlock *bb = (BasicBlock *) lir_alloc(builder, sizeof(BasicBlock));
  //fprintf(stderr, "newblock=(%p, %p)\n", bb, pc);
  bb->base.flag = 0;
  bb->base.next = NULL;
  bb->start_pc  = pc;
  bb->size      = 0;
  bb->capacity  = 1;
  bb->Insts     = (lir_compile_data_header_t **)
      lir_alloc(builder, sizeof(lir_compile_data_header_t *) * 1);
  if (builder->Block != NULL) {
    builder->Block->base.next = (lir_compile_data_header_t *) bb;
  }

  builder->Block = bb;
  return bb;
}

static BasicBlock *FindBasicBlockByPC(lir_builder_t *builder, VALUE *pc)
{
  BasicBlock *bb = builder->EntryBlock;
  while(bb != NULL) {
    if (pc == bb->start_pc) {
      return bb;
    }
    bb = (BasicBlock *) bb->base.next;
  }
  return NULL;
}

#include "snapshot.c"
#include "ir.c"

static int get_opcode(rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  long pc = GET_PC_COUNT();
  int  op = (int)(GET_ISEQ()->iseq[pc]);
  return op;
}

static void dump_inst(rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
#if DUMP_INST > 0
  long pc = GET_PC_COUNT();
  int  op = get_opcode(reg_cfp, reg_pc);
  fprintf(stderr, "%04ld pc=%p %02d %s\n", pc, reg_pc, op, rb_insns_name(op));
#endif
}

#define HOTLOOP 4
static VALUE *trace_edge(rb_thread_t *th, rb_control_frame_t *reg_cfp, VALUE *pc, VALUE *dst_pc)
{
  struct Trace *trace = hashmap_get(&compiler_info->Traces, pc);
  if (trace == NULL) {
    trace = new_trace(GET_ISEQ(), pc);
    hashmap_set(&compiler_info->Traces, pc, trace);
  }

  if (trace->Code) {
    POPN(1);
    VALUE *newpc = trace->Code(th, reg_cfp, dst_pc);
    return newpc;
  }

  if (++trace->LoopCount == HOTLOOP) {
    lir_builder_t *builder = compiler_info->_LirBuilder;
    ResetLIRBuilder(builder, trace, 1);
    builder->Block = CreateBlock(builder, dst_pc);
    builder->EntryBlock = builder->Block;
    trace->Parent = compiler_info->Current;
    compiler_info->Current = trace;
    enable_trace(builder);
    dump_inst(reg_cfp, pc);
  }
  return pc;
}

#include "record.c"

VALUE *jit_trace(rb_thread_t *th, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
#ifndef NOJIT
  int opcode = get_opcode(reg_cfp, reg_pc);
  if (!is_tracing()) {
    switch(opcode) {
      case BIN(branchif):{
        OFFSET dst = (OFFSET)GET_OPERAND(1);
        VALUE  val = TOPN(0);
        if (RTEST(val) && dst < 0) {
          VALUE *dst_pc = reg_pc + insn_len(BIN(branchif)) + dst;
          return trace_edge(th, reg_cfp, reg_pc, dst_pc);
        }
        break;
      }
    }
  }
  else {
    lir_builder_t *builder = compiler_info->_LirBuilder;
    dump_inst(reg_cfp, reg_pc);
    record_insn(builder, opcode, reg_cfp, reg_pc);
  }
#endif
  return reg_pc;
}

#include "cgen.c"
