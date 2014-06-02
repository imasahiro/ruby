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
#include "insns.inc"
#include "insns_info.inc"

#include "jit_prelude.c"

#include "jit.h"
#include "jit_opts.h"
#include "kmap.c"
#define GWJIT_HOST 1
#include "jit_context.h"

typedef long reg_t;

typedef VALUE *VALUEPtr;
typedef void  *voidPtr;
typedef reg_t *RegPtr;

struct Event;
typedef struct TraceRecorder TraceRecorder;

static void dump_lir(TraceRecorder *builder);

typedef struct lir_compile_data_header {
  unsigned opcode:10;
  unsigned flag  :22;
  int id;
  struct lir_compile_data_header *next;
} lir_compile_data_header_t;

typedef struct lir_basic_block {
  lir_compile_data_header_t   base;
  lir_compile_data_header_t **Insts;
  unsigned size;
  unsigned capacity;
  VALUE *start_pc;
} BasicBlock;
typedef BasicBlock *BasicBlockPtr;

static int
check_cfunc(const rb_method_entry_t *me, VALUE (*func)())
{
  if (me && me->def->type == VM_METHOD_TYPE_CFUNC &&
      me->def->body.cfunc.func == func) {
    return 1;
  }
  else {
    return 0;
  }
}

#define HOT_TRACE_THRESHOLD 4

typedef enum trace_mode {
  TRACE_MODE_DEFAULT = 0,
  TRACE_MODE_RECORD  = 1,
  TRACE_MODE_EMIT_BACKWARD_BRANCH = (1 << 1)
} TraceMode;

/* TraceExitStatus API */
static const char *TraceStatusToStr(TraceExitStatus status)
{
  if (status == TRACE_EXIT_SUCCESS) {
    return "TRACE_EXIT_SUCCESS";
  }
  return "TRACE_EXIT_SIDE_EXIT";
}

typedef enum trace_error_status {
  TRACE_OK,
  TRACE_ERROR_NATIVE_METHOD,
  TRACE_ERROR_THROW,
  TRACE_ERROR_UNSUPPORT_OP,
  TRACE_ERROR_LEAVE,
  TRACE_ERROR_REGSTACK_UNDERFLOW,
  TRACE_ERROR_ALREADY_RECORDED,
  TRACE_ERROR_BUFFER_FULL,
  TRACE_ERROR_END
} TraceErrorStatus;

static const char *trace_error_message[] = {
  "ok",
  "invoking native method",
  "throw exception",
  "not supported bytecode",
  "this trace return into native method",
  "register stack underflow",
  "this instruction is already recorded on trace"
  "trace buffer is full"
  ""
};

typedef struct Event Event;
typedef struct Trace Trace;
typedef struct RJit  RJit;

static TraceRecorder *TraceRecorderNew(RJit *jit);
static void TraceRecorderDelete(TraceRecorder *Rec);

typedef TraceExitStatus (*native_func_t)(rb_thread_t *th,
                                         rb_control_frame_t *reg_cfp,
                                         VALUE *reg_pc,
                                         VALUE **exit_pc);

static native_func_t TranslateToNativeCode(TraceRecorder *Rec);

struct RJit {
  TraceRecorder *Rec;
  TraceMode mode_;
  Trace    *CurrentTrace;
  hashmap_t Traces;
};

static void RJitSetMode(RJit *Jit, TraceMode mode)
{
  Jit->mode_ = mode;
  fprintf(stderr, "jit mode = %d\n", mode);
}

static int RJitModeIs(RJit *Jit, TraceMode mode)
{
  return (Jit->mode_ & mode) == mode;
}

RJit *RJitInit()
{
  RJit *Jit = (RJit *) malloc(sizeof(*Jit));
  Jit->Rec = TraceRecorderNew(Jit);
  RJitSetMode(Jit, TRACE_MODE_DEFAULT);
  Jit->CurrentTrace = NULL;
  hashmap_init(&Jit->Traces, 4);
  return Jit;
}

static void TraceFree(Trace *trace);

static void RJitSetTrace(RJit *Jit, TraceMode mode, Trace *trace)
{
  RJitSetMode(Jit, mode);
  Jit->CurrentTrace = trace;
}

void RJitDelete(RJit *Jit)
{
  TraceRecorderDelete(Jit->Rec);
  hashmap_dispose(&Jit->Traces, (void (*)(void*))TraceFree);
  free(Jit);
}

static RJit *global_rjit = NULL;
void RJitGlobalInit()
{
  global_rjit = RJitInit();
  Init_jit(); // load jit_prelude
}

void RJitGlobalDestruct()
{
  RJitDelete(global_rjit);
  global_rjit = NULL;
}

// stack map
struct call_stack_struct {
  unsigned regstack_size;
  unsigned method_argc;
};

typedef struct Stackmap {
  int size;
  int flag;
  reg_t regs[0];
} StackMap;

static StackMap *GetStackMap(TraceRecorder *Rec, VALUE *pc);
static void DeleteStackMap(void *map)
{
  free(map);
}

// trace
struct Trace {
  native_func_t Code;
  void *precondition;
  VALUE *StartPC;
  VALUE *LastPC;
  Trace *Parent;
  const rb_iseq_t *iseq;
  unsigned ctr;
  hashmap_t SideExit;
};

static Trace *TraceNew(const rb_iseq_t *iseq, VALUE *pc, Trace *parent)
{
  Trace *trace = (Trace *) malloc(sizeof(*trace));
  trace->Code = NULL;
  trace->StartPC = pc;
  trace->LastPC  = NULL;
  trace->Parent = parent;
  trace->ctr    = 0;
  trace->iseq   = iseq;
  hashmap_init(&trace->SideExit, 1);
  return trace;
}

static void TraceReset(Trace *trace)
{
  if (hashmap_size(&trace->SideExit) > 0) {
    hashmap_dispose(&trace->SideExit, DeleteStackMap);
    hashmap_init(&trace->SideExit, 1);
  }
}

static void TraceFree(Trace *trace)
{
  hashmap_dispose(&trace->SideExit, DeleteStackMap);
  free(trace);
}

static void TraceUpdateLastInst(Trace *trace, VALUE *pc)
{
  trace->LastPC = pc;
}

static Trace *FindTrace(RJit *jit, VALUE *pc)
{
  void *trace = hashmap_get(&jit->Traces, pc);
  return (Trace *) trace;
}

static Trace *AddTrace(RJit *jit, rb_control_frame_t *reg_cfp, VALUE *pc, Trace *parent)
{
  Trace *trace = TraceNew(GET_ISEQ(), pc, parent);
  hashmap_set(&jit->Traces, pc, trace);
  return trace;
}

static Trace *GetOrAddTrace(RJit *jit, rb_control_frame_t *reg_cfp, VALUE *pc, Trace *parent)
{
  Trace *trace = FindTrace(jit, pc);
  if (trace) {
    return trace;
  }
  return AddTrace(jit, reg_cfp, pc, parent);
}

// trace event
struct Event {
  rb_control_frame_t *cfp;
  VALUE *pc;
  Trace *trace;
  int opcode;
  TraceErrorStatus reason;
};

static int get_opcode(rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  long pc = GET_PC_COUNT();
  int  op = (int)(GET_ISEQ()->iseq[pc]);
  return op;
}

static Event *EventInit(Event *e, RJit *jit, rb_control_frame_t *cfp, VALUE *pc)
{
  e->cfp = cfp;
  e->pc  = pc;
  e->trace = jit->CurrentTrace;
  e->opcode = get_opcode(e->cfp, e->pc);
  e->reason = TRACE_OK;
  return e;
}

static int isBackwardBranch(Event *e)
{
  if(e->opcode == BIN(branchif)) {
    rb_control_frame_t *reg_cfp = e->cfp;
    OFFSET dst = (OFFSET) GET_OPERAND(1);
    return dst < 0;
  }
  return 0;
}

// List of entry of inline cache that jit generated code use
typedef struct inline_cache_manager {
  CALL_INFO *CallInfoList;
  int CallInfoListSize;
  int CallInfoListCapacity;
  int CallInfoLastFreezed;
} InlineCacheManager;

static CALL_INFO CloneInlineCache(InlineCacheManager *icm, CALL_INFO ci)
{
  CALL_INFO newci = (CALL_INFO) malloc(sizeof(*newci));
  memcpy(newci, ci, sizeof(*newci));

  if (icm->CallInfoListSize == icm->CallInfoListCapacity) {
    unsigned newsize = icm->CallInfoListCapacity * 2;
    icm->CallInfoList = (CALL_INFO *)
        realloc(icm->CallInfoList, sizeof(CALL_INFO) * newsize);
    icm->CallInfoListCapacity = newsize;
  }
  icm->CallInfoList[icm->CallInfoListSize++] = newci;
  return newci;
}

static void FreezeInlineCache(InlineCacheManager *icm)
{
  icm->CallInfoLastFreezed = icm->CallInfoListSize;
}

static void CancelCallInfo(InlineCacheManager *icm)
{
  int i;
  for (i = icm->CallInfoLastFreezed; i < icm->CallInfoListSize; i++) {
    free(icm->CallInfoList[i]);
  }
}

static void InlineCacheManagerInit(InlineCacheManager *icm)
{
  icm->CallInfoList = (CALL_INFO *) malloc(sizeof(CALL_INFO) * 1);
  icm->CallInfoListSize     = 0;
  icm->CallInfoListCapacity = 1;
  icm->CallInfoLastFreezed  = 0;
}

static void InlineCacheManagerDelete(InlineCacheManager *icm)
{
  icm->CallInfoLastFreezed = 0;
  CancelCallInfo(icm);
}

// trace recorder
struct TraceRecorder {
  RJit *jit;
  Event *CurrentEvent;

  BasicBlock *Block;
  BasicBlock *EntryBlock;

  lir_compile_data_header_t *buffer_head;
  lir_compile_data_header_t *buffer_root;
  char *buffer_current;

  reg_t *RegStack;
  int RegStackBottom;
  int RegStackSize;
  int RegStackCapacity;

  struct call_stack_struct *CallStack;
  int CallStackSize;
  int CallStackCapacity;

  int LastInstId;
  int CallDepth;

  InlineCacheManager CacheMng;
};

static void TraceRecorderClear(TraceRecorder *Rec, int AllocBuffer);

static TraceRecorder *TraceRecorderNew(RJit *jit)
{
  TraceRecorder *Rec = (TraceRecorder *) malloc(sizeof(*Rec));
  memset(Rec, 0, sizeof(*Rec));
  Rec->jit = jit;
  TraceRecorderClear(Rec, 0);
  InlineCacheManagerInit(&Rec->CacheMng);
  return Rec;
}

static void TraceRecorderDelete(TraceRecorder *Rec)
{
  TraceRecorderClear(Rec, 0);
  InlineCacheManagerDelete(&Rec->CacheMng);
  free(Rec);
}

#define BUFF_POS(STORAGE)  ((STORAGE)->id)
#define BUFF_SIZE(STORAGE) ((STORAGE)->flag)
static lir_compile_data_header_t *lir_alloc(TraceRecorder *Rec, size_t size)
{
  lir_compile_data_header_t *ptr;
  lir_compile_data_header_t *storage = Rec->buffer_head;
  if (BUFF_POS(storage) + size > BUFF_SIZE(storage)) {
    unsigned alloc_size = BUFF_SIZE(storage) * 2;
    while(alloc_size < size) {
      alloc_size *= 2;
    }
    storage->next = malloc(sizeof(lir_compile_data_header_t) + alloc_size);
    Rec->buffer_head = storage = storage->next;
    storage->next = NULL;
    Rec->buffer_current  = (char *) (storage + 1);
    BUFF_POS(storage)  = 0;
    BUFF_SIZE(storage) = alloc_size;
  }
  ptr = (lir_compile_data_header_t *)
      (Rec->buffer_current + BUFF_POS(storage));
  memset(ptr, 0, size);
  ptr->id = Rec->LastInstId++;
  BUFF_POS(storage) += (unsigned) size;
  return ptr;
}

static lir_compile_data_header_t *lir_realloc(TraceRecorder *Rec, void *oldptr, size_t oldsize, size_t newsize)
{
  void *newptr = NULL;
  if (oldsize >= newsize) {
    return (lir_compile_data_header_t *) oldptr;
  }
  newptr = lir_alloc(Rec, newsize);
  memcpy(newptr, oldptr, oldsize);
  return newptr;
}

static Trace *TraceRecorderGetTrace(TraceRecorder *Rec)
{
  return Rec->jit->CurrentTrace;
}

static void TraceRecorderClear(TraceRecorder *Rec, int AllocBuffer)
{
  lir_compile_data_header_t *storage = Rec->buffer_head;
  Rec->Block      = NULL;
  Rec->EntryBlock = NULL;
  Rec->LastInstId = 0;
  Rec->buffer_head    = NULL;
  Rec->buffer_current = NULL;
  while(storage != NULL) {
    lir_compile_data_header_t *next = storage->next;
    free(storage);
    storage = next;
  }
  if (AllocBuffer) {
    Rec->buffer_head =
        malloc(sizeof(lir_compile_data_header_t) + LIR_COMPILE_DATA_BUFF_SIZE);
    BUFF_POS(Rec->buffer_head)  = 0;
    BUFF_SIZE(Rec->buffer_head) = LIR_COMPILE_DATA_BUFF_SIZE;
    Rec->buffer_head->next = NULL;
  }
  Rec->buffer_root    = Rec->buffer_head;
  Rec->buffer_current = (char *) (Rec->buffer_head + 1);

  Rec->RegStackBottom   = 0;
  Rec->RegStackSize     = 0;
  Rec->RegStackCapacity = 0;
  Rec->RegStack = NULL;
  if (AllocBuffer) {
    reg_t *RegStack;
    Rec->RegStackCapacity = GWIR_RESERVED_REGSTACK_SIZE + 1;
    RegStack = (reg_t *) lir_alloc(Rec, sizeof(reg_t) * Rec->RegStackCapacity);
    Rec->RegStack = RegStack + GWIR_RESERVED_REGSTACK_SIZE;
  }

  Rec->CallStackSize     = 0;
  Rec->CallStackCapacity = 0;
  Rec->CallStack = NULL;
  if (AllocBuffer) {
    Rec->CallStackCapacity = 1;
    Rec->CallStack = (struct call_stack_struct *)
        lir_alloc(Rec, sizeof(struct call_stack_struct) * 1);
  }

  Rec->CallDepth    = 0;
}
#undef BUFF_POS
#undef BUFF_SIZE

static void record_insn(RJit *jit, Event *e);
static void dump_inst(rb_control_frame_t *reg_cfp, VALUE *reg_pc);
static BasicBlock *CreateBlock(TraceRecorder *Rec, VALUE *pc);
static unsigned CountLIRInstSize(TraceRecorder *Rec);
static void TakeStackSnapshot(TraceRecorder *Rec, VALUE *PC);
static int Emit_Exit(TraceRecorder *Rec, VALUEPtr Exit);
static int Emit_Jump(TraceRecorder *Rec, VALUEPtr Exit);
static int Emit_StackAdjust(TraceRecorder *Rec, int argc, RegPtr argv);

static void TraceRecorderAppend(RJit *jit, TraceRecorder *Rec, Event *e)
{
  Rec->CurrentEvent = e;
  if (Rec->EntryBlock == NULL) {
    Rec->EntryBlock = CreateBlock(Rec, NULL);
    Emit_StackAdjust(Rec, 0, NULL);
    Emit_Jump(Rec, e->pc);
    Rec->Block = CreateBlock(Rec, e->pc);
  }
  dump_inst(e->cfp, e->pc);
  record_insn(jit, e);
}

static int TraceRecorderIsFull(TraceRecorder *Rec)
{
  return Rec->LastInstId == GWIR_MAX_TRACE_LENGTH;
}

static void SubmitToCompilation(RJit *jit, TraceRecorder *Rec)
{
  jit->CurrentTrace->Code = NULL;
  dump_lir(Rec);
  if (CountLIRInstSize(Rec) > GWIR_MIN_TRACE_LENGTH) {
    jit->CurrentTrace->Code = TranslateToNativeCode(Rec);
  }
  jit->CurrentTrace = NULL;
  TraceRecorderClear(Rec, 0);
}

static void TraceRecorderAbort(TraceRecorder *Rec, rb_control_frame_t *reg_cfp, VALUE *reg_pc, TraceErrorStatus reason) {
  if (reason != TRACE_OK) {
    const rb_iseq_t *iseq = GET_ISEQ();
    VALUE file = iseq->location.path;
    fprintf(stderr, "failed to trace at file:%s line:%d because %s\n",
            RSTRING_PTR(file),
            rb_iseq_line_no(iseq, reg_pc - iseq->iseq_encoded),
            trace_error_message[reason]);
    TakeStackSnapshot(Rec, reg_pc);
    Emit_Exit(Rec, reg_pc);
    if (reason != TRACE_ERROR_REGSTACK_UNDERFLOW) {
      SubmitToCompilation(Rec->jit, Rec);
    }
  }
  RJitSetMode(Rec->jit, TRACE_MODE_DEFAULT);
  global_rjit->CurrentTrace = NULL;
  TraceRecorderClear(Rec, 0);
}

static TraceExitStatus Invoke(rb_thread_t *th, Trace *trace, Event *e, VALUE **exit_pc_ptr)
{
  return trace->Code(th, e->cfp, e->pc, exit_pc_ptr);
}

// Stopping rule of trace recording
static int AlreadyRecordedOnTrace(RJit *jit, Event *e) {
  Trace *parent = e->trace->Parent;
  // Link to parent trace
  if (parent != NULL) {
    //  [a] ---> [b] ---> [c] ---> [a] (trace-header)
    //       |
    //       --> [d] ---> [c] ---> [a]
    // A Parent trace is [a]->[b]->[c].
    // The beginning point of child trace is [d]
    if (parent->StartPC == e->pc) {
      RJitSetMode(jit, TRACE_MODE_RECORD | TRACE_MODE_EMIT_BACKWARD_BRANCH);
      TraceRecorderAppend(jit, jit->Rec, e);
      return 1;
    }
  }
  // this instruction is backward branch to a trace header.
  else if (e->trace->StartPC == e->pc) {
    RJitSetMode(jit, TRACE_MODE_RECORD | TRACE_MODE_EMIT_BACKWARD_BRANCH);
    TraceRecorderAppend(jit, jit->Rec, e);
    return 1;
  }
  return 0;
}

static int isTracableNativeCall(Event *e)
{
  if(e->opcode != BIN(opt_send_simple) || e->opcode != BIN(send)) {
    /* this instruction is not method call instruction */
    return 1;
  }
  rb_control_frame_t *reg_cfp = e->cfp;
  CALL_INFO ci = (CALL_INFO) GET_OPERAND(1);
  vm_search_method(ci, ci->recv = TOPN(ci->argc));
  if(ci->me) {
    switch (ci->me->def->type) {
      case VM_METHOD_TYPE_IVAR:    return 1;
      case VM_METHOD_TYPE_ATTRSET: return 1;
      case VM_METHOD_TYPE_ISEQ:    return 1;
      case VM_METHOD_TYPE_CFUNC: {
        /* check Math method */
        VALUE cMath = rb_singleton_class(rb_mMath);
        if (ci->me->klass == cMath) {
          return 1;
        }
      }
      default: break;
    }
  }

  /* check ClassA.new(argc, argv) */
  if (check_cfunc(ci->me,  rb_class_new_instance)) {
    if (ci->me->klass == rb_cClass) {
      return 1;
    }
  }

  /* check block_given? */
  extern VALUE rb_f_block_given_p(void);
  if (check_cfunc(ci->me,  rb_f_block_given_p)) {
    return 1;
  }

  // I think this method is c-defined method.
  // abort trace compilation
  return 0;
}

static int isIrregularEvent(Event *e)
{
  if (e->opcode == BIN(throw)) {
    return 1;
  }
  return 0;
}

static int ShouldEndTrace(Event *e, TraceRecorder *Rec)
{
  if (AlreadyRecordedOnTrace(Rec->jit, e)) {
    e->reason = TRACE_ERROR_ALREADY_RECORDED;
    return 1;
  }
  else if (TraceRecorderIsFull(Rec)) {
    e->reason = TRACE_ERROR_BUFFER_FULL;
    return 1;
  }
  else if (!isTracableNativeCall(e)) {
    e->reason = TRACE_ERROR_NATIVE_METHOD;
    return 1;
  }
  else if (isIrregularEvent(e)) {
    e->reason = TRACE_ERROR_THROW;
    return 1;
  }
  return 0;
}

static VALUE *TraceSelection(RJit *jit, rb_thread_t *th, Event *e)
{
  Trace *trace = NULL;
  if (RJitModeIs(jit, TRACE_MODE_RECORD)) {
    if (ShouldEndTrace(e, jit->Rec)) {
      RJitSetMode(jit, TRACE_MODE_DEFAULT);
      SubmitToCompilation(jit, jit->Rec);
    }
    else {
      TraceRecorderAppend(jit, jit->Rec, e);
    }
    return e->pc;
  }
  /* trace dispatch */
  trace = FindTrace(jit, e->pc);
  if (trace && trace->Code) {
    VALUE *exit_pc = NULL;
    TraceExitStatus exit_status = Invoke(th, trace, e, &exit_pc);
    switch (exit_status) {
      case TRACE_EXIT_SIDE_EXIT:
        trace = GetOrAddTrace(jit, th->cfp, exit_pc, trace);
        TraceRecorderClear(jit->Rec, 1);
        RJitSetTrace(jit, TRACE_MODE_RECORD, trace);
        break;
      case TRACE_EXIT_SUCCESS:
        break;
      case TRACE_EXIT_ERROR:
        assert(0 && "unreachable");
        break;
    }
    return exit_pc;
  }
  /* identify potential trace head */
  if (isBackwardBranch(e)) {
    trace = GetOrAddTrace(jit, e->cfp, e->pc, NULL);
  }
  /* trace head selection and start recording */
  if (trace) {
    trace->ctr += 1;
    if (trace->ctr > HOT_TRACE_THRESHOLD) {
      RJitSetTrace(jit, TRACE_MODE_RECORD, trace);
      TraceReset(trace);
      TraceRecorderClear(jit->Rec, 1);
      TraceRecorderAppend(jit, jit->Rec, e);
    }
  }
  return e->pc;
}

VALUE *jit_trace(rb_thread_t *th, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
  Event ebuf;
  Event *e = EventInit(&ebuf, global_rjit, reg_cfp, reg_pc);
  return TraceSelection(global_rjit, th, e);
}

#include "ir.c"
#include "snapshot.c"
#include "record.c"
#include "cgen.c"
