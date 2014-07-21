/**********************************************************************

  jit.c -

  $Author$

  Copyright (C) 2014 Masahiro Ide

 **********************************************************************/

#include <sys/time.h> // gettimeofday
#include "ruby/ruby.h"
#include "ruby/vm.h"
#include "ruby/st.h"
#include "method.h"
#include "vm_core.h"
#include "vm_exec.h"
#include "vm_insnhelper.h"
#include "internal.h"
#include "iseq.h"
#include "gc.h"
#include "insns.inc"
#include "vmrecord.inc"

#include "jit_prelude.c"

#include "jit.h"
#include "jit_opts.h"
#include "hashmap.c"
#define GWJIT_HOST 1
#include "jit_context.h"
#include "gwir_template.h"

static VALUE rb_cMath = Qnil;
static ID id_to_f, id_to_i;

typedef VALUE *VALUEPtr;
typedef void *voidPtr;

struct Event;
typedef struct TraceRecorder TraceRecorder;

static void dump_lir(TraceRecorder *builder);

typedef struct lir_compile_data_header {
    unsigned opcode : 10;
    unsigned flag : 22;
    int id;
} lir_compile_data_header_t;

typedef struct lir_linkedlist_t {
    lir_compile_data_header_t base;
    struct lir_linkedlist_t *next;
} lir_linkedlist_t;

typedef struct lir_basic_block BasicBlock;
typedef struct lir_inst_t      lir_inst_t;

typedef struct lir_list_t {
    lir_compile_data_header_t base;
    lir_inst_t **list;
    unsigned size;
    unsigned capacity;
} lir_list_t;

struct lir_inst_t {
    lir_compile_data_header_t base;
    BasicBlock *parent;
    lir_list_t *user;
};

typedef lir_inst_t *lir_t;
typedef lir_t *LirPtr;

struct lir_basic_block {
    lir_linkedlist_t base;
    lir_inst_t **Insts;
    unsigned size;
    unsigned capacity;
    VALUE *start_pc;
};

typedef BasicBlock *BasicBlockPtr;

#define JIT_PROFILE_ENTER(msg) jit_profile((msg), 0)
#define JIT_PROFILE_LEAVE(msg, cond) jit_profile((msg), (cond))

static void jit_profile(const char* msg, int print_log)
{
    static uint64_t last = 0;
    uint64_t time;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    time = tv.tv_sec * 1000 + tv.tv_usec / 1000;
    if (print_log) {
        fprintf(stderr, "%s : %" PRI_LL_PREFIX "u msec\n", msg, (time - last));
    }
    last = time;
}

static short jit_vm_redefined_flag[JIT_BOP_EXT_LAST_ - BOP_LAST_];
static st_table *jit_opt_method_table = 0;

static int jit_redefinition_check_flag(VALUE klass)
{
    if (klass == rb_cFixnum)
        return FIXNUM_REDEFINED_OP_FLAG;
    if (klass == rb_cMath)
        return MATH_REDEFINED_OP_FLAG;
    return 0;
}

void rb_jit_check_redefinition_opt_method(const rb_method_entry_t *me,
                                          VALUE klass)
{
    st_data_t bop;
    if (st_lookup(jit_opt_method_table, (st_data_t)me, &bop)) {
        int flag = jit_redefinition_check_flag(klass);
        assert(flag != 0);
        jit_vm_redefined_flag[bop - JIT_BOP_LAST_] |= flag;
    }
}

/* copied from vm.c */
static void add_opt_method(VALUE klass, ID mid, VALUE bop)
{
    rb_method_entry_t *me = rb_method_entry_at(klass, mid);

    if (me && me->def && me->def->type == VM_METHOD_TYPE_CFUNC) {
        st_insert(jit_opt_method_table, (st_data_t)me, (st_data_t)bop);
    } else {
        rb_bug("undefined optimized method: %s", rb_id2name(mid));
    }
}

#define DEF(k, mid, bop)                                \
    do {                                                \
        jit_vm_redefined_flag[bop - JIT_BOP_LAST_] = 0; \
        add_opt_method(rb_c##k, rb_intern(mid), bop);   \
    } while (0)

static void rb_jit_init_redefined_flag(void)
{
    jit_opt_method_table = st_init_numtable();
    DEF(Fixnum, "&", JIT_BOP_AND);
    DEF(Fixnum, "|", JIT_BOP_OR);
    DEF(Fixnum, "^", JIT_BOP_XOR);
    DEF(Fixnum, ">>", JIT_BOP_RSHIFT);
    DEF(Fixnum, "~", JIT_BOP_INV);

    DEF(Fixnum, "to_f", JIT_BOP_TO_F);
    DEF(Float, "to_f", JIT_BOP_TO_F);
    DEF(String, "to_f", JIT_BOP_TO_F);

    DEF(Float, "to_i", JIT_BOP_TO_I);
    DEF(String, "to_i", JIT_BOP_TO_I);

    DEF(Fixnum, "to_s", JIT_BOP_TO_S);
    DEF(Float, "to_s", JIT_BOP_TO_S);
    DEF(String, "to_s", JIT_BOP_TO_S);

    DEF(Math, "sin", JIT_BOP_SIN);
    DEF(Math, "cos", JIT_BOP_COS);
    DEF(Math, "tan", JIT_BOP_TAN);
    DEF(Math, "exp", JIT_BOP_EXP);
    DEF(Math, "sqrt", JIT_BOP_SQRT);
    DEF(Math, "log10", JIT_BOP_LOG10);
    DEF(Math, "log2", JIT_BOP_LOG2);
}
#undef DEF

static int check_cfunc(const rb_method_entry_t *me, VALUE (*func)())
{
    if (me && me->def->type == VM_METHOD_TYPE_CFUNC
        && me->def->body.cfunc.func == func) {
        return 1;
    } else {
        return 0;
    }
}

#define HOT_TRACE_THRESHOLD 4

typedef enum trace_mode {
    TRACE_MODE_DEFAULT = 0,
    TRACE_MODE_RECORD = 1,
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
    "this instruction is already recorded on trace",
    "trace buffer is full",
    ""
};

typedef struct Event Event;
typedef struct Trace Trace;
typedef struct RJit RJit;

static TraceRecorder *TraceRecorderNew(RJit *jit);
static void TraceRecorderDelete(TraceRecorder *Rec);

typedef TraceExitStatus (*native_func_t)(rb_thread_t *th,
                                         rb_control_frame_t *reg_cfp,
                                         VALUE *reg_pc, VALUE **exit_pc);

static native_func_t trace_compile(TraceRecorder *Rec, Trace *trace);

struct RJit {
    VALUE self;
    TraceRecorder *Rec;
    TraceMode mode_;
    Trace *CurrentTrace;
    hashmap_t Traces;
};

typedef struct inline_cache_manager {
    CALL_INFO *CallInfoList;
    int CallInfoListSize;
    int CallInfoListCapacity;
    int CallInfoLastFreezed;
} InlineCacheManager;

struct TraceRecorder {
    RJit *jit;
    Event *CurrentEvent;

    BasicBlock *Block;
    BasicBlock *EntryBlock;

    lir_linkedlist_t *buffer_head;
    lir_linkedlist_t *buffer_root;
    char *buffer_current;

    lir_t *RegStack;
    int RegStackBottom;
    int RegStackSize;
    int RegStackCapacity;

    struct call_stack_struct *CallStack;
    int CallStackSize;
    int CallStackCapacity;

    int LastInstId;
    int CallDepth;

    lir_inst_t **constpool;
    unsigned constpool_size;
    unsigned constpool_capacity;

    InlineCacheManager CacheMng;
};

struct Trace {
    native_func_t Code;
    void *precondition;
    VALUE *StartPC;
    VALUE *LastPC;
    Trace *Parent;
    const rb_iseq_t *iseq;
    unsigned counter;
    unsigned flag;
    hashmap_t StackMap;
    hashmap_t *SideExit;
    void *handler;
    VALUE *constpool;
    unsigned constpool_size;
    unsigned constpool_capacity;
};

static int RJitModeIs(RJit *Jit, TraceMode mode)
{
    return (Jit->mode_ & mode) == mode;
}

static void RJitSetMode(RJit *Jit, TraceMode mode)
{
    Jit->mode_ = mode;
#if 1
    fprintf(stderr, "jit mode = ");
    if (mode == TRACE_MODE_DEFAULT) {
        fprintf(stderr, "default");
    }
    else {
        fprintf(stderr, "record");
        if (RJitModeIs(Jit, TRACE_MODE_EMIT_BACKWARD_BRANCH)) {
            fprintf(stderr, "|backward_branch");
        }
    }
    fprintf(stderr, "\n");
#endif
}

static void jit_mark(void *ptr)
{
    RUBY_MARK_ENTER("jit");
    if (ptr) {
        RJit *jit = ptr;
        hashmap_iterator_t itr = { 0, 0 };
        while (hashmap_next(&jit->Traces, &itr)) {
            Trace *trace = (Trace *) itr.entry->val;
            unsigned i;
            for (i = 0; i < trace->constpool_size; i++) {
                VALUE obj = trace->constpool[i];
                if(obj) {
                    rb_gc_mark(obj);
                }
            }
        }
    }
    RUBY_MARK_LEAVE("jit");
}

static size_t jit_memsize(const void *ptr)
{
    if (ptr) {
        RJit *jit = (RJit *) ptr;
        size_t size = sizeof(RJit) + sizeof(TraceRecorder);
        size += sizeof(hashmap_t) * hashmap_size(&jit->Traces);
        return size;
    }
    return 0;
}

static const rb_data_type_t jit_data_type = {
    "JIT",
    {jit_mark, NULL, jit_memsize,},
    NULL, NULL, RUBY_TYPED_FREE_IMMEDIATELY
};

static RJit *RJitInit()
{
    RJit *Jit = (RJit *)malloc(sizeof(*Jit));
    VALUE rb_cJit = rb_define_class("Jit", rb_cObject);
    rb_undef_alloc_func(rb_cJit);
    rb_undef_method(CLASS_OF(rb_cJit), "new");

    Jit->Rec = TraceRecorderNew(Jit);
    RJitSetMode(Jit, TRACE_MODE_DEFAULT);
    Jit->CurrentTrace = NULL;
    hashmap_init(&Jit->Traces, 4);

    Jit->self = TypedData_Wrap_Struct(rb_cJit, &jit_data_type, Jit);
    rb_gc_register_mark_object(Jit->self);
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
    hashmap_dispose(&Jit->Traces, (hashmap_entry_destructor_t)TraceFree);
    free(Jit);
}

static RJit *global_rjit = NULL;
void rb_jit_global_init()
{
    global_rjit = RJitInit();
    rb_cMath = rb_singleton_class(rb_mMath);
    rb_jit_init_redefined_flag();
    id_to_f = rb_intern("to_f");
    id_to_i = rb_intern("to_i");
    Init_jit(); // load jit_prelude
}

void rb_jit_global_destruct()
{
    RJitDelete(global_rjit);
    global_rjit = NULL;
}

// stack map
struct call_stack_struct {
    int regstack_size;
    int method_argc;
};

typedef struct Stackmap {
    int size;
    int flag;
    lir_inst_t *regs[0];
} StackMap;

static StackMap *GetStackMap(TraceRecorder *Rec, VALUE *pc);
static void DeleteStackMap(StackMap *map) { free((void *)map); }

// trace
static Trace *TraceNew(const rb_iseq_t *iseq, VALUE *pc, Trace *parent)
{
    Trace *trace = (Trace *)malloc(sizeof(*trace));
    trace->Code = NULL;
    trace->StartPC = pc;
    trace->LastPC = NULL;
    trace->Parent = parent;
    trace->counter = 0;
    trace->iseq = iseq;
    hashmap_init(&trace->StackMap, 1);
    trace->SideExit = NULL;
    trace->handler = NULL;
    trace->constpool = NULL;
    trace->constpool_size = trace->constpool_capacity = 0;
    return trace;
}

static void TraceAddConst(Trace *trace, VALUE val)
{
    if (trace->constpool_size == trace->constpool_capacity) {
        trace->constpool_capacity *= 2;
        trace->constpool = realloc(trace->constpool,
                sizeof(VALUE) * trace->constpool_capacity);
    }
    trace->constpool[trace->constpool_size] = val;
    trace->constpool_size += 1;
}

static void TraceDropHandler(Trace *trace);
static void TraceClear(Trace *trace, int alloc)
{
    hashmap_dispose(&trace->StackMap, (hashmap_entry_destructor_t)DeleteStackMap);
    if (alloc) {
        hashmap_init(&trace->StackMap, 1);
    }

    if (trace->SideExit) {
        hashmap_dispose(trace->SideExit, NULL);
    }

    if (trace->handler) {
        TraceDropHandler(trace);
        trace->handler = NULL;
    }

    if (trace->constpool) {
        trace->constpool_size = 0;
        trace->constpool_capacity = 0;
        free(trace->constpool);
    }
    if (alloc) {
        trace->constpool = (VALUE *) malloc(sizeof(VALUE));
        trace->constpool[0] = 0;
        trace->constpool_capacity = 1;
    }
}

static void trace_optimize(TraceRecorder *Rec, Trace *trace);

static void TraceFree(Trace *trace)
{
    TraceClear(trace, 0);
    free(trace);
}

static void TraceUpdateLastInst(Trace *trace, VALUE *pc) { trace->LastPC = pc; }

static Trace *FindTrace(RJit *jit, VALUE *pc)
{
    return (Trace *)hashmap_get(&jit->Traces, (hashmap_data_t)pc);
}

static Trace *AddTrace(RJit *jit, rb_control_frame_t *reg_cfp, VALUE *pc,
                       Trace *parent)
{
    Trace *trace = TraceNew(GET_ISEQ(), pc, parent);
    hashmap_set(&jit->Traces, (hashmap_data_t)pc, (hashmap_data_t)trace);
    return trace;
}

static Trace *GetOrAddTrace(RJit *jit, rb_control_frame_t *reg_cfp, VALUE *pc,
                            Trace *parent)
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
    int op = (int)(GET_ISEQ()->iseq[pc]);
    assert(0 <= op && op < VM_INSTRUCTION_SIZE);
    return op;
}

static Event *EventInit(Event *e, RJit *jit, rb_control_frame_t *cfp, VALUE *pc)
{
    e->cfp = cfp;
    e->pc = pc;
    e->trace = jit->CurrentTrace;
    e->opcode = get_opcode(e->cfp, e->pc);
    e->reason = TRACE_OK;
    return e;
}

static int isBackwardBranch(Event *e)
{
    if (e->opcode == BIN(branchif)) {
        rb_control_frame_t *reg_cfp = e->cfp;
        OFFSET dst = (OFFSET)GET_OPERAND(1);
        return dst < 0;
    }
    return 0;
}

// InlineCacheManager
static CALL_INFO CloneInlineCache(InlineCacheManager *icm, CALL_INFO ci)
{
    CALL_INFO newci = (CALL_INFO)malloc(sizeof(*newci));
    memcpy(newci, ci, sizeof(*newci));

    if (icm->CallInfoListSize == icm->CallInfoListCapacity) {
        unsigned newsize = icm->CallInfoListCapacity * 2;
        icm->CallInfoList = (CALL_INFO *)realloc(icm->CallInfoList,
                                                 sizeof(CALL_INFO) * newsize);
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
    icm->CallInfoList = (CALL_INFO *)malloc(sizeof(CALL_INFO) * 1);
    icm->CallInfoListSize = 0;
    icm->CallInfoListCapacity = 1;
    icm->CallInfoLastFreezed = 0;
}

static void InlineCacheManagerDelete(InlineCacheManager *icm)
{
    icm->CallInfoLastFreezed = 0;
    CancelCallInfo(icm);
}

// TraceRecorder
static void TraceRecorderClear(TraceRecorder *Rec, int AllocBuffer);

static TraceRecorder *TraceRecorderNew(RJit *jit)
{
    TraceRecorder *Rec = (TraceRecorder *)malloc(sizeof(*Rec));
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

#define BUFF_POS(STORAGE) ((STORAGE)->base.id)
#define BUFF_SIZE(STORAGE) ((STORAGE)->base.flag)
static void *lir_alloc(TraceRecorder *Rec, size_t size)
{
    lir_compile_data_header_t *ptr;
    lir_linkedlist_t *storage = Rec->buffer_head;
    if (BUFF_POS(storage) + size > BUFF_SIZE(storage)) {
        unsigned alloc_size = BUFF_SIZE(storage) * 2;
        while (alloc_size < size) {
            alloc_size *= 2;
        }
        storage->next = (lir_linkedlist_t *)malloc(sizeof(lir_linkedlist_t) + alloc_size);
        Rec->buffer_head = storage = (lir_linkedlist_t *)storage->next;
        storage->next = NULL;
        Rec->buffer_current = (char *)(storage + 1);
        BUFF_POS(storage) = 0;
        BUFF_SIZE(storage) = alloc_size;
    }
    ptr = (lir_compile_data_header_t *)(Rec->buffer_current + BUFF_POS(storage));
    memset(ptr, 0, size);
    ptr->id = Rec->LastInstId++;
    BUFF_POS(storage) += (unsigned)size;
    return ptr;
}

static void *lir_realloc(TraceRecorder *Rec, void *oldptr, size_t oldsize, size_t newsize)
{
    void *newptr = NULL;
    if (oldsize >= newsize) {
        return oldptr;
    }
    newptr = lir_alloc(Rec, newsize);
    memcpy(newptr, oldptr, oldsize);
    return newptr;
}

static Trace *TraceRecorderGetTrace(TraceRecorder *Rec)
{
    return Rec->jit->CurrentTrace;
}

static void TraceRecorderAddConst(TraceRecorder *Rec, lir_inst_t *inst, VALUE val)
{
    assert(Rec->jit->CurrentTrace->constpool_size == Rec->constpool_size);

    TraceAddConst(Rec->jit->CurrentTrace, val);
    if (Rec->constpool_size == Rec->constpool_capacity) {
        unsigned newsize = Rec->constpool_capacity * 2;
        Rec->constpool = (lir_inst_t **) lir_realloc(Rec, Rec->constpool,
                sizeof(lir_inst_t*) * Rec->constpool_capacity,
                sizeof(lir_inst_t*) * newsize);
        Rec->constpool_capacity = newsize;
    }
    Rec->constpool[Rec->constpool_size] = inst;
    Rec->constpool_size += 1;
}

static void TraceRecorderClear(TraceRecorder *Rec, int AllocBuffer)
{
    lir_linkedlist_t *storage = Rec->buffer_head;
    Rec->Block = NULL;
    Rec->EntryBlock = NULL;
    Rec->LastInstId = 0;
    Rec->buffer_head = NULL;
    Rec->buffer_current = NULL;
    while (storage != NULL) {
        lir_linkedlist_t *next = storage->next;
        free(storage);
        storage = next;
    }
    if (AllocBuffer) {
        Rec->buffer_head = malloc(sizeof(lir_linkedlist_t)
                                  + LIR_COMPILE_DATA_BUFF_SIZE);
        BUFF_POS(Rec->buffer_head) = 0;
        BUFF_SIZE(Rec->buffer_head) = LIR_COMPILE_DATA_BUFF_SIZE;
        Rec->buffer_head->next = NULL;
    }
    Rec->buffer_root = Rec->buffer_head;
    Rec->buffer_current = (char *)(Rec->buffer_head + 1);

    Rec->RegStackBottom = 0;
    Rec->RegStackSize = 0;
    Rec->RegStackCapacity = 0;
    Rec->RegStack = NULL;
    if (AllocBuffer) {
        lir_t *RegStack;
        Rec->RegStackCapacity = GWIR_RESERVED_REGSTACK_SIZE + 1;
        RegStack
            = (lir_t *)lir_alloc(Rec, sizeof(lir_t) * Rec->RegStackCapacity);
        Rec->RegStack = RegStack + GWIR_RESERVED_REGSTACK_SIZE;
    }

    Rec->CallStackSize = 0;
    Rec->CallStackCapacity = 0;
    Rec->CallStack = NULL;
    if (AllocBuffer) {
        Rec->CallStackCapacity = 1;
        Rec->CallStack = (struct call_stack_struct *)lir_alloc(
            Rec, sizeof(struct call_stack_struct) * 1);
    }

    Rec->constpool_size = 0;
    Rec->constpool_capacity = 0;
    Rec->constpool = NULL;
    if (AllocBuffer) {
        Rec->constpool = (lir_inst_t **) lir_alloc(Rec, sizeof(lir_inst_t *));
        Rec->constpool[0] = 0;
        Rec->constpool_capacity = 1;
    }

    Rec->CallDepth = 0;
}
#undef BUFF_POS
#undef BUFF_SIZE

static void record_insn(RJit *jit, Event *e);
static void dump_inst(rb_control_frame_t *reg_cfp, VALUE *reg_pc);
static BasicBlock *CreateBlock(TraceRecorder *Rec, VALUE *pc);
static unsigned CountLIRInstSize(TraceRecorder *Rec);
static void TakeStackSnapshot(TraceRecorder *Rec, VALUE *PC);
static lir_t Emit_Exit(TraceRecorder *Rec, VALUEPtr Exit);
static lir_t Emit_Jump(TraceRecorder *Rec, BasicBlock *BB);
static lir_t Emit_StackAdjust(TraceRecorder *Rec, int argc, LirPtr argv);

static void TraceRecorderAppend(RJit *jit, TraceRecorder *Rec, Event *e)
{
    Rec->CurrentEvent = e;
    if (Rec->EntryBlock == NULL) {
        BasicBlock* bb;
        Rec->EntryBlock = Rec->Block = CreateBlock(Rec, NULL);
        Emit_StackAdjust(Rec, 0, NULL);
        bb = CreateBlock(Rec, e->pc);
        Emit_Jump(Rec, bb);
        Rec->Block = bb;
    }
    dump_inst(e->cfp, e->pc);
    record_insn(jit, e);
}

static int TraceRecorderIsFull(TraceRecorder *Rec)
{
    return Rec->LastInstId >= GWIR_MAX_TRACE_LENGTH;
}

static void SubmitToCompilation(RJit *jit, TraceRecorder *Rec)
{
    if (CountLIRInstSize(Rec) > GWIR_MIN_TRACE_LENGTH) {
        Trace *trace = TraceRecorderGetTrace(Rec);
        trace_optimize(Rec, trace);
        dump_lir(Rec);
        trace_compile(Rec, trace);
    }
    jit->CurrentTrace = NULL;
    TraceRecorderClear(Rec, 0);
}

static void TraceRecorderAbort(TraceRecorder* Rec, rb_control_frame_t* reg_cfp,
                               VALUE* reg_pc, TraceErrorStatus reason)
{
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

static VALUE *Invoke(RJit *jit, rb_thread_t *th, Trace *trace, Event *e)
{
    VALUE *exit_pc = NULL;
    //JIT_PROFILE_ENTER("invoke trace");
    TraceExitStatus exit_status;
    exit_status = trace->Code(th, e->cfp, e->pc, &exit_pc);
#if GWIT_LOG_SIDE_EXIT > 0
    fprintf(stderr, "trace for %p is exit from %p\n", e->pc, exit_pc);
#endif
    switch (exit_status) {
    case TRACE_EXIT_SIDE_EXIT:
        trace = GetOrAddTrace(jit, th->cfp, exit_pc, trace);
        TraceClear(trace, 1);
        TraceRecorderClear(jit->Rec, 1);
        RJitSetTrace(jit, TRACE_MODE_RECORD, trace);
        break;
    case TRACE_EXIT_SUCCESS:
        break;
    case TRACE_EXIT_ERROR:
        assert(0 && "unreachable");
        break;
    }
    //JIT_PROFILE_LEAVE("invoke trace", 0);
    return exit_pc;
}

// Stopping rule of trace recording
static int AlreadyRecordedOnTrace(RJit *jit, Event *e)
{
    Trace *parent = e->trace->Parent;
    // Link to parent trace
    if (parent != NULL) {
        //  [a] ---> [b] ---> [c] ---> [a] (trace-header)
        //       |
        //       --> [d] ---> [c] ---> [a]
        // A Parent trace is [a]->[b]->[c].
        // The beginning point of child trace is [d]
        if (parent->StartPC == e->pc) {
            RJitSetMode(jit,
                        TRACE_MODE_RECORD | TRACE_MODE_EMIT_BACKWARD_BRANCH);
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
    rb_control_frame_t* reg_cfp;
    CALL_INFO ci;

    if (e->opcode != BIN(opt_send_simple) || e->opcode != BIN(send)) {
        /* this instruction is not method call instruction */
        return 1;
    }
    reg_cfp = e->cfp;
    ci = (CALL_INFO)GET_OPERAND(1);
    vm_search_method(ci, ci->recv = TOPN(ci->argc));
    if (ci->me) {
        switch (ci->me->def->type) {
        case VM_METHOD_TYPE_IVAR:
            return 1;
        case VM_METHOD_TYPE_ATTRSET:
            return 1;
        case VM_METHOD_TYPE_ISEQ:
            return 1;
        case VM_METHOD_TYPE_CFUNC:
            /* check Math method */
            if (ci->me->klass == rb_cMath) {
                return 1;
            }
        default:
            break;
        }
    }

    /* check ClassA.new(argc, argv) */
    if (check_cfunc(ci->me, rb_class_new_instance)) {
        if (ci->me->klass == rb_cClass) {
            return 1;
        }
    }

    /* check block_given? */
    {
        extern VALUE rb_f_block_given_p(void);
        if (check_cfunc(ci->me, rb_f_block_given_p)) {
            return 1;
        }
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
    } else if (TraceRecorderIsFull(Rec)) {
        e->reason = TRACE_ERROR_BUFFER_FULL;
        return 1;
    } else if (!isTracableNativeCall(e)) {
        e->reason = TRACE_ERROR_NATIVE_METHOD;
        return 1;
    } else if (isIrregularEvent(e)) {
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
        } else {
            TraceRecorderAppend(jit, jit->Rec, e);
        }
        return e->pc;
    }
    /* trace dispatch */
    trace = FindTrace(jit, e->pc);
    if (trace && trace->Code && trace->Code != (void *) 0xdeadbeaf) {
        return Invoke(jit, th, trace, e);
    }
    /* identify potential trace head */
    if (isBackwardBranch(e)) {
        trace = GetOrAddTrace(jit, e->cfp, e->pc, NULL);
    }
    /* trace head selection and start recording */
    if (trace) {
        trace->counter += 1;
        if (trace->Code != (void *) 0xdeadbeaf &&
                trace->counter > HOT_TRACE_THRESHOLD) {
            RJitSetTrace(jit, TRACE_MODE_RECORD, trace);
            TraceClear(trace, 1);
            TraceRecorderClear(jit->Rec, 1);
            TraceRecorderAppend(jit, jit->Rec, e);
        }
    }
    return e->pc;
}

VALUE *rb_jit_trace(rb_thread_t *th, rb_control_frame_t *reg_cfp, VALUE *reg_pc)
{
    Event ebuf;
    Event *e = EventInit(&ebuf, global_rjit, reg_cfp, reg_pc);
    return TraceSelection(global_rjit, th, e);
}

#include "ir.c"
#include "optimizer.c"
#include "snapshot.c"
#include "record.c"
#include "cgen.c"
