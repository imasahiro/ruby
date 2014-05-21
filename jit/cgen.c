/**********************************************************************

  cgen.c -

  $Author$

  Copyright (C) 2014 Masahiro Ide

 **********************************************************************/

#include <sys/time.h> // gettimeofday

#if defined __APPLE__
#include <mach-o/dyld.h> // _dyld_get_image_name
#elif defined __linux__
#include <unistd.h>      // readlink
#endif

#include <sys/stat.h>

#include <limits.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <unistd.h>
#include <assert.h>
#define GWJIT_HOST 1
#include "jit_context.h"

#include "verconf.h"

static gwjit_context_t jit_host_context = {};

static void gwjit_context_init()
{
  unsigned long i;
  memset(&jit_host_context, 0, sizeof(gwjit_context_t));

  jit_host_context.cArray  = rb_cArray;
  jit_host_context.cFixnum = rb_cFixnum;
  jit_host_context.cFloat  = rb_cFloat;
  jit_host_context.cHash   = rb_cHash;
  jit_host_context.cRegexp = rb_cRegexp;
  jit_host_context.cTime   = rb_cTime;
  jit_host_context.cString = rb_cString;
  jit_host_context.cSymbol = rb_cSymbol;

  jit_host_context.cTrueClass  = rb_cTrueClass;
  jit_host_context.cFalseClass = rb_cTrueClass;
  jit_host_context.cNilClass   = rb_cNilClass;

  jit_host_context._rb_check_array_type = rb_check_array_type;
  jit_host_context._rb_big_plus   = rb_big_plus;
  jit_host_context._rb_big_minus  = rb_big_minus;
  jit_host_context._rb_big_mul    = rb_big_mul;
  jit_host_context._rb_int2big    = rb_int2big;
  jit_host_context._rb_str_length = rb_str_length;
  jit_host_context._rb_range_new  = rb_range_new;
  jit_host_context._rb_hash_new   = rb_hash_new;
  jit_host_context._rb_hash_aref  = rb_hash_aref;
  jit_host_context._rb_hash_aset  = rb_hash_aset;
  jit_host_context._rb_reg_match  = rb_reg_match;
  jit_host_context._rb_ary_new    = rb_ary_new;
  jit_host_context._rb_ary_new_from_values = rb_ary_new_from_values;


  // internal APIs
  jit_host_context._rb_float_new_in_heap = rb_float_new_in_heap;
  jit_host_context._ruby_float_mod  = ruby_float_mod;
  jit_host_context._rb_ary_entry    = rb_ary_entry;
  jit_host_context._rb_ary_store    = rb_ary_store;
  jit_host_context._rb_exc_raise    = rb_exc_raise;
#if SIZEOF_INT < SIZEOF_VALUE
  jit_host_context._rb_out_of_int   = rb_out_of_int;
#endif
  jit_host_context._ruby_current_vm = ruby_current_vm;
  jit_host_context._ruby_vm_redefined_flag = ruby_vm_redefined_flag;
  jit_host_context._ruby_vm_global_method_state = ruby_vm_global_method_state_ptr;
  jit_host_context._make_no_method_exception  = make_no_method_exception;
  jit_host_context._rb_gc_writebarrier  = rb_gc_writebarrier;

  jit_host_context._rb_gvar_get = rb_gvar_get;
  jit_host_context._rb_gvar_set = rb_gvar_set;

  for (i = 0; i < sizeof(gwjit_context_t) / sizeof(VALUE); i++) {
    assert(((VALUE*)&jit_host_context)[i] != 0 &&
           "some field of jit_host_context is not initialized");
  }
  // supress warnings
  (void) insn_name;
  (void) insn_op_types;
  (void) insn_op_type;
}

/* copied from vm.c */
static int
vm_redefinition_check_flag(VALUE klass)
{
  if (klass == rb_cFixnum) return FIXNUM_REDEFINED_OP_FLAG;
  if (klass == rb_cFloat)  return FLOAT_REDEFINED_OP_FLAG;
  if (klass == rb_cString) return STRING_REDEFINED_OP_FLAG;
  if (klass == rb_cArray)  return ARRAY_REDEFINED_OP_FLAG;
  if (klass == rb_cHash)   return HASH_REDEFINED_OP_FLAG;
  if (klass == rb_cBignum) return BIGNUM_REDEFINED_OP_FLAG;
  if (klass == rb_cSymbol) return SYMBOL_REDEFINED_OP_FLAG;
  if (klass == rb_cTime)   return TIME_REDEFINED_OP_FLAG;
  if (klass == rb_cRegexp) return REGEXP_REDEFINED_OP_FLAG;
  return 0;
}

typedef struct Buffer {
  char *buf;
  unsigned size;
  unsigned capacity;
} Buffer;

#define BUFFER_CAPACITY (4096)

static Buffer *buffer_init(Buffer *buf)
{
  buf->buf      = (char *) malloc(BUFFER_CAPACITY);
  buf->size     = 0;
  buf->capacity = BUFFER_CAPACITY;
  return buf;
}

static void buffer_destory(Buffer *buf)
{
  free(buf->buf);
}

static void buffer_setnull(Buffer *buf)
{
  buf->buf[buf->size] = '\0';
}

static int buffer_printf(Buffer *buf, const char *fmt, va_list ap)
{
  unsigned size = buf->size;
  size_t n = vsnprintf(buf->buf + size, buf->capacity - size, fmt, ap);
  if (n < buf->capacity - size) {
    buf->size += n;
    return 1;
  }
  else {
    buffer_setnull(buf);
    return 0;
  }
}

static uint64_t getTimeMilliSecond(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static uint64_t timer = 0;

// cgen
enum cgen_mode {
  PROCESS_MODE, // generate native code directly
  FILE_MODE     // generate temporary c-source file
};

typedef struct CGen {
  Buffer buf;
  FILE  *fp;
  void  *hdr;
  const char *path;
  enum cgen_mode mode;
} CGen;

static const char cmd_template[] = "clang -pipe -x c "
"%s %s "
"-Ijit/ -Ibuild/ -Ibuild/.ext/include/x86_64-darwin13 -Iinclude -I. "
"-dynamiclib -O3 -g0 -Wall -o %s %s";

static size_t find_parent_path_end(char *buf, size_t path_len)
{
  size_t end = path_len;
  while(end > 0 && buf[end - 1] != '/') {
    end--;
  }
  return end;

}

static void remove_filename(char *buf)
{
  size_t pos = find_parent_path_end(buf, strlen(buf));
  if (buf > 0) {
    buf[pos] = '\0';
  }
}

static int append_path(char *buf, size_t bufsize, const char *path, size_t psize)
{
  size_t len = strlen(buf);
  if (len + psize >= bufsize) {
    return 0;
  }
  strncpy(buf + len, path, psize);
  buf[len + psize] = '\0';
  return 1;
}

static int file_exists(const char *path)
{
  int res = 0;
#if defined _WIN32
  DWORD attr = GetFileAttributesA(path);
  res = (attr != -1);
#elif defined __linux__ || defined __APPLE__
  struct stat buf;
  res = (stat(path, &buf) != -1);
#endif
  return res;
}

static int get_current_executable_path(char *buf, size_t bufsize)
{
#ifdef __APPLE__
  const char *path = _dyld_get_image_name(0);
  if (realpath(path, buf) != NULL) {
    return 1;
  }
#elif defined __linux__
  if (readlink("/proc/self/exe", buf, bufsiz) != -1) {
    return 1;
  }
#else
#error not supported
#endif
  return 0;
}

#define PCH_FILE_NAME   "ruby_jit.h.pch"

static int construct_pch_path(char *buf, size_t bufsize)
{
  if (get_current_executable_path(buf, bufsize)) {
    remove_filename(buf);
    if (append_path(buf, bufsize, PCH_FILE_NAME, strlen(PCH_FILE_NAME))) {
      if (file_exists(buf)) {
        return 1;
      }
    }
  }
#if defined RUBY_LIB_PREFIX
  buf[0] = '\0';
  if (append_path(buf, bufsize, RUBY_LIB_PREFIX, strlen(RUBY_LIB_PREFIX))) {
    if (append_path(buf, bufsize, PCH_FILE_NAME, strlen(PCH_FILE_NAME))) {
      if (file_exists(buf)) {
        return 1;
      }
    }
  }
#endif /* defined RUBY_LIB_PREFIX */
  return 0;
}

static void cgen_open(CGen *gen, enum cgen_mode mode, const char *path, int id)
{

  buffer_init(&gen->buf);
  gen->mode = mode;
  gen->hdr  = NULL;
  timer = getTimeMilliSecond();
  if (gen->mode == PROCESS_MODE) {
    char cmd[PATH_MAX + 512] = {};
    const char *pch_path = "";
    const char *pch_flag = "";
#if GWJIT_USE_PCH > 0
    char buf[PATH_MAX];
    if (construct_pch_path(buf, PATH_MAX)) {
      pch_path = (const char *) buf;
      pch_flag = "-include-pch";
    }
    else
#endif
    {
      fprintf(stderr, "gwjit cannot use pre compiled header\n");
    }
    snprintf(cmd, 512, cmd_template, pch_flag, pch_path, path, "-");
    gen->fp = popen(cmd, "w");
  }
  else {
    char fpath[512] = {};
    snprintf(fpath, 512, "/tmp/gwjit.%d.%d.c", getpid(), id);
    gen->fp = fopen(fpath, "w");
  }
  gen->path = path;
}

static void cgen_freeze(CGen *gen, int id)
{
  if (gen->buf.size > 0) {
    buffer_setnull(&gen->buf);
    fputs(gen->buf.buf, gen->fp);
  }
  buffer_destory(&gen->buf);
#if GWJIT_DUMP_COMPILE_LOG > 0
  uint64_t end = getTimeMilliSecond();
  fprintf(stderr, "c-code generation time %llu\n", end - timer);
#endif

  if (gen->mode == FILE_MODE) {
    char fpath[512] = {};
    char cmd[PATH_MAX + 512] = {};
    const char *pch_path = "";
    const char *pch_flag = "";
#if GWJIT_USE_PCH > 0
    char buf[PATH_MAX];
    if (construct_pch_path(buf, PATH_MAX)) {
      pch_path = (const char *) buf;
      pch_flag = "-include-pch";
    }
    else
#endif
    {
      fprintf(stderr, "gwjit cannot use pre compiled header\n");
    }

    snprintf(fpath, 512, "/tmp/gwjit.%d.%d.c", getpid(), id);
    snprintf(cmd, 512, cmd_template, pch_flag, pch_path, gen->path, fpath);

#if GWJIT_DUMP_COMPILE_LOG > 1
    fprintf(stderr, "compiling c code : %s\n", cmd);
#endif
#if GWJIT_DUMP_COMPILE_LOG > 0
    fprintf(stderr, "generated c-code is %s\n", gen->path);
#endif
    fclose(gen->fp);
    gen->fp = popen(cmd, "w");
  }
  pclose(gen->fp);
#if GWJIT_DUMP_COMPILE_LOG > 0
  fprintf(stderr, "native code generation time %llu\n", getTimeMilliSecond() - end);
#endif
  gen->fp = NULL;
}

static void cgen_close(CGen *gen)
{
  gen->hdr = NULL;
}

static void cgen_printf(CGen *gen, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void cgen_printf(CGen *gen, const char *fmt, ...)
{
  va_list ap, ap2;
  va_start(ap, fmt);
  va_copy(ap2, ap);
  if (buffer_printf(&gen->buf, fmt, ap) == 0) {
    fputs(gen->buf.buf, gen->fp);
    vfprintf(gen->fp, fmt, ap2);
    gen->buf.size = 0;
  }
  va_end(ap);
}

static void *cgen_get_function(CGen *gen, const char *fname)
{
  if (gen->hdr == NULL) {
    gen->hdr = dlopen(gen->path, RTLD_LAZY);
  }
  if (gen->hdr != NULL) {
    int (*finit)(const gwjit_context_t *jit_context);
    char fname2[128] = {};
    snprintf(fname2, 128, "init_%s", fname);
    finit = dlsym(gen->hdr, fname2);
    if (finit) {
      finit(&jit_host_context);
      return dlsym(gen->hdr, fname);
    }
  }
  return NULL;
}

// cgenerator utility

static long GetBlockId(hashmap_t *SideExitBBs, VALUE *pc)
{
  long val = (long) hashmap_get(SideExitBBs, pc);
  assert(val != 0);
  return val >> 1;
}

static void generate_flonum_cond(CGen *gen, const char *op, long Id, reg_t LHS, reg_t RHS)
{
  cgen_printf(gen,
              "v%ld = RFLOAT_VALUE(v%ld) %s RFLOAT_VALUE(v%ld)"
              "? Qtrue : Qfalse;\n"
              , Id, LHS, op, RHS);
}

static void generate_fixnum_cond(CGen *gen, const char *op, long Id, reg_t LHS, reg_t RHS)
{
  cgen_printf(gen,
              "{\n"
              "  SIGNED_VALUE recv = v%ld;\n"
              "  SIGNED_VALUE obj  = v%ld;\n"
              "  v%ld = (recv %s obj) ? Qtrue : Qfalse;\n"
              "}\n", LHS, RHS, Id, op);
}

// convert LIR to C code

static void TranslateLIR2C(lir_builder_t *builder, CGen *gen, hashmap_t *SideExitBBs, lir_compile_data_header_t *Inst);

static void PrepareSideExit(lir_builder_t *builder, CGen *gen, hashmap_t *SideExitBBs)
{
  long j = 1;
  hashmap_iterator_t itr = {0, 0};
  while(hashmap_next(&builder->Current->SideExit, &itr)) {
    VALUE *pc = itr.entry->k;
    hashmap_set(SideExitBBs, pc, (struct Trace *)(j << 1));
    cgen_printf(gen, "VALUE *exit_pc_%ld = (VALUE *) %p;\n", j, pc);
    j += 1;
  }
}

static void EmitFramePush(lir_builder_t *builder, CGen *cgen, IFramePush *ir);

static void EmitSideExit(lir_builder_t *builder, CGen *gen, hashmap_t *SideExitBBs)
{
  /*
     sp[0..n] = StackMap[0..n]
   * return PC;
   */
  unsigned i, j;
  hashmap_iterator_t itr = {0, 0};
  while(hashmap_next(SideExitBBs, &itr)) {
    VALUE *pc = itr.entry->k;
    long BlockId = ((long) itr.entry->v) >> 1;
    StackMap *stack = GetStackMap(builder, pc);

    cgen_printf(gen, "L_exit%ld:;\n", BlockId);
    cgen_printf(gen, "//fprintf(stderr,\"exit%ld : pc=%p\\n\");\n", BlockId, pc);

    j = 0;
    cgen_printf(gen, "th->cfp = reg_cfp = original_cfp;\n");
    cgen_printf(gen, "SET_SP(original_sp);\n");
    for (i = 0; i < stack->size; i++) {
      lir_compile_data_header_t *Inst = FindLIRById(builder, stack->regs[i]);
      if (Inst->opcode == OPCODE_IFramePush) {
        IFramePush *ir = (IFramePush *) Inst;
        int offset = j - (ir->invokeblock ? 1 : 0);
        cgen_printf(gen, "SET_SP(GET_SP() + %d);\n", offset);
        EmitFramePush(builder, gen, ir);
        j = 0;
      }
      else {
        cgen_printf(gen, "(GET_SP())[%d] = v%ld;\n", j, stack->regs[i]);
        j++;
      }
    }

    cgen_printf(gen,
                "SET_SP(GET_SP() + %d);\n"
                "SET_PC(exit_pc_%ld);\n"
                "return exit_pc_%ld;\n",
                j, BlockId, BlockId);
  }
}

static void EmitFramePush(lir_builder_t *builder, CGen *gen, IFramePush *ir)
{
  int i, begin = ir->invokeblock ? 1 : 0;
  cgen_printf(gen,
              "{\n"
              "  CALL_INFO ci = (CALL_INFO) %p;\n", ir->ci);
  for (i = begin; i < ir->argc; i++) {
    cgen_printf(gen, "(GET_SP())[%d] = v%ld;\n", i - begin, ir->argv[i]);
  }
  cgen_printf(gen, "  SET_SP(GET_SP() + %d);\n", ir->argc - ir->invokeblock);
  if (ir->invokeblock) {
    cgen_printf(gen,
                "  ci->argc = ci->orig_argc;\n"
                "  ci->blockptr = 0;\n"
                "  ci->recv = v%ld;\n"
                "  jit_vm_call_block_setup(th, reg_cfp,\n"
                "                          (rb_block_t *) v%ld, ci, %d);\n"
                "  reg_cfp = th->cfp;\n",
                ir->argv[0], ir->block, ir->argc - 1);
  }
  else {
    if (ir->block != 0) {
      cgen_printf(gen,
                  "  ci->blockptr = (rb_block_t *) v%ld;\n"
                  "  assert(ci->blockptr != 0);\n"
                  "  ci->blockptr->iseq = ci->blockiseq;\n"
                  "  ci->blockptr->proc = 0;\n",
                  ir->block);
    }
    cgen_printf(gen,
                "  jit_vm_call_iseq_setup_normal(th, reg_cfp, ci, %d);\n"
                "  reg_cfp = th->cfp;\n",
                ir->argc - 1);
  }
  cgen_printf(gen, "}\n");
}

static void Translate(lir_builder_t *builder, CGen *gen, hashmap_t *SideExitBBs, int fid)
{
  BasicBlock *block = builder->EntryBlock;
#if GWJIT_DUMP_COMPILE_LOG > 0
  const rb_iseq_t *iseq = builder->Current->iseq;
  VALUE file = iseq->location.path;
#endif

  cgen_printf(gen,
              "#include \"ruby_jit.h\"\n"
              "#include <assert.h>\n"
              "#include <dlfcn.h>\n"
#if GWJIT_DUMP_COMPILE_LOG > 0
              "// This code is translated from file=%s line=%d\n"
#endif
              "const gwjit_context_t *jit_context = NULL;\n"
              "void init_gwjit_%d(const gwjit_context_t *context) {\n"
              "  jit_context = context;\n"
              "  (void) make_no_method_exception;\n"
              "  (void) jit_vm_call_iseq_setup_normal;\n"
              "  (void) jit_vm_yield_setup_block_args;\n"
              "}\n"
              "VALUE *gwjit_%d(rb_thread_t *th,\n"
              "                rb_control_frame_t *reg_cfp,\n"
              "                VALUE *reg_pc)\n"
              "{\n"
              "  VALUE *original_sp = GET_SP();\n"
              "  rb_control_frame_t *original_cfp = reg_cfp;\n",
#if GWJIT_DUMP_COMPILE_LOG > 0
              RSTRING_PTR(file),
              rb_iseq_line_no(iseq,
                              builder->Current->StartPC - iseq->iseq_encoded),
#endif
              fid, fid);

  PrepareSideExit(builder, gen, SideExitBBs);
  cgen_printf(gen, "VALUE v0 = 0; (void) v0;\n");

  while(block != NULL) {
    unsigned i = 0;
    for (i = 0; i < block->size; i++) {
      lir_compile_data_header_t *Inst = block->Insts[i];
      if (lir_inst_define_value(Inst->opcode)) {
        long Id = lir_getid(Inst);
        assert(Id != 0);
        cgen_printf(gen, "VALUE v%ld = 0;\n", Id);
      }
    }
    block = (BasicBlock *) block->base.next;
  }

  block = builder->EntryBlock;
  while(block != NULL) {
    unsigned i = 0;
    cgen_printf(gen, "L_%d:;\n", block->base.id);
    //cgen_printf(gen, "fprintf(stderr, \"block%d\\n\");\n", block->base.id);
    for (i = 0; i < block->size; i++) {
      lir_compile_data_header_t *Inst = block->Insts[i];
      TranslateLIR2C(builder, gen, SideExitBBs, Inst);
    }
    block = (BasicBlock *) block->base.next;
  }
  EmitSideExit(builder, gen, SideExitBBs);
  cgen_printf(gen, "}\n");
}

static native_func_t TranslateToNativeCode(lir_builder_t *builder)
{
  static int serial_id = 0;
  char path[128] = {};
  char fname[128] = {};
  CGen gen;
  native_func_t ptr;

  int id = serial_id++;
  if (id == 0) {
    gwjit_context_init();
  }

  snprintf(fname, 128, "gwjit_%d", id);
  snprintf(path,  128, "/tmp/gwjit.%d.%d.dylib", (unsigned) getpid(), id);

  cgen_open(&gen, FILE_MODE, path, id);

  hashmap_t SideExitBBs;
  hashmap_init(&SideExitBBs, 4);

  Translate(builder, &gen, &SideExitBBs, id);

  cgen_freeze(&gen, id);

  ptr = (native_func_t) cgen_get_function(&gen, fname);

  cgen_close(&gen);
  hashmap_dispose(&SideExitBBs, 0);
  return ptr;
}

static void finalize_codegen(lir_builder_t *builder)
{
}

static void TranslateLIR2C(lir_builder_t *builder, CGen *gen, hashmap_t *SideExitBBs, lir_compile_data_header_t *Inst)
{
  long Id = lir_getid(Inst);
  switch (Inst->opcode) {
    case OPCODE_IGuardTypeFixnum : {
      IGuardTypeFixnum *ir = (IGuardTypeFixnum *) Inst;
      long ExitBlockId = GetBlockId(SideExitBBs, ir->Exit);
      cgen_printf(gen,
                  "if(!FIXNUM_P(v%ld)) {\n"
                  "  goto L_exit%ld;\n"
                  "}\n", ir->R, ExitBlockId);
      break;
    }
    case OPCODE_IGuardTypeFloat : {
      IGuardTypeFloat *ir = (IGuardTypeFloat *) Inst;
      long ExitBlockId = GetBlockId(SideExitBBs, ir->Exit);
      cgen_printf(gen,
                  "if(!(RBASIC_CLASS(v%ld) == jit_context->cFloat)) {\n"
                  "  goto L_exit%ld;\n"
                  "}\n", ir->R, ExitBlockId);
      break;
    }
    case OPCODE_IGuardTypeFlonum : {
      IGuardTypeFlonum *ir = (IGuardTypeFlonum *) Inst;
      long ExitBlockId = GetBlockId(SideExitBBs, ir->Exit);
      cgen_printf(gen,
                  "if(!FLONUM_P(v%ld)) {\n"
                  "  goto L_exit%ld;\n"
                  "}\n", ir->R, ExitBlockId);
      break;
    }
    case OPCODE_IGuardTypeSpecialConst : {
      IGuardTypeSpecialConst *ir = (IGuardTypeSpecialConst *) Inst;
      long ExitBlockId = GetBlockId(SideExitBBs, ir->Exit);
      cgen_printf(gen,
                  "if(!!SPECIAL_CONST_P(v%ld)) {\n"
                  "  goto L_exit%ld;\n"
                  "}\n", ir->R, ExitBlockId);
      break;
    }
    case OPCODE_IGuardTypeArray : {
      IGuardTypeArray *ir = (IGuardTypeArray *) Inst;
      long ExitBlockId = GetBlockId(SideExitBBs, ir->Exit);
      cgen_printf(gen,
                  "if(!(RBASIC_CLASS(v%ld) == jit_context->cArray)) {\n"
                  "  goto L_exit%ld;\n"
                  "}\n", ir->R, ExitBlockId);
      break;
    }
    case OPCODE_IGuardTypeString : {
      IGuardTypeString *ir = (IGuardTypeString *) Inst;
      long ExitBlockId = GetBlockId(SideExitBBs, ir->Exit);
      cgen_printf(gen,
                  "if(!(RBASIC_CLASS(v%ld) == jit_context->cString)) {\n"
                  "  goto L_exit%ld;\n"
                  "}\n", ir->R, ExitBlockId);
      break;
    }
    case OPCODE_IGuardTypeHash : {
      IGuardTypeHash *ir = (IGuardTypeHash *) Inst;
      long ExitBlockId = GetBlockId(SideExitBBs, ir->Exit);
      cgen_printf(gen,
                  "if(!(RBASIC_CLASS(v%ld) == jit_context->cHash)) {\n"
                  "  goto L_exit%ld;\n"
                  "}\n", ir->R, ExitBlockId);
      break;
    }
    case OPCODE_IGuardTypeRegexp : {
      IGuardTypeRegexp *ir = (IGuardTypeRegexp *) Inst;
      long ExitBlockId = GetBlockId(SideExitBBs, ir->Exit);
      cgen_printf(gen,
                  "if(!(RBASIC_CLASS(v%ld) == jit_context->cRegexp)) {\n"
                  "  goto L_exit%ld;\n"
                  "}\n", ir->R, ExitBlockId);
      break;
    }
    case OPCODE_IGuardTypeTime : {
      IGuardTypeTime *ir = (IGuardTypeTime *) Inst;
      long ExitBlockId = GetBlockId(SideExitBBs, ir->Exit);
      cgen_printf(gen,
                  "if(!(RBASIC_CLASS(v%ld) == jit_context->cTime)) {\n"
                  "  goto L_exit%ld;\n"
                  "}\n", ir->R, ExitBlockId);
      break;
    }
    case OPCODE_IGuardTypeObject : {
      IGuardTypeObject *ir = (IGuardTypeObject *) Inst;
      long ExitBlockId = GetBlockId(SideExitBBs, ir->Exit);
      cgen_printf(gen,
                  "if(!(RB_TYPE_P(v%ld, T_OBJECT))) {\n"
                  "  goto L_exit%ld;\n"
                  "}\n", ir->R, ExitBlockId);
      break;
    }
    case OPCODE_IGuardTypeNil : {
      IGuardTypeNil *ir = (IGuardTypeNil *) Inst;
      long ExitBlockId = GetBlockId(SideExitBBs, ir->Exit);
      cgen_printf(gen,
                  "if(!RTEST(v%ld)) {\n"
                  "  goto L_exit%ld;\n"
                  "}\n", ir->R, ExitBlockId);
      break;
    }
    case OPCODE_IGuardTypeNonNil : {
      IGuardTypeNonNil *ir = (IGuardTypeNonNil *) Inst;
      long ExitBlockId = GetBlockId(SideExitBBs, ir->Exit);
      cgen_printf(gen,
                  "if(!!RTEST(v%ld)) {\n"
                  "  goto L_exit%ld;\n"
                  "}\n", ir->R, ExitBlockId);
      break;
    }
    case OPCODE_IGuardBlockEqual : {
      IGuardBlockEqual *ir = (IGuardBlockEqual *) Inst;
      long ExitBlockId = GetBlockId(SideExitBBs, ir->Exit);
      rb_block_t *block = (rb_block_t *) ir->Block;
      cgen_printf(gen,
                  "{\n"
                  "  rb_block_t *block = (rb_block_t *) v%ld;\n"
                  "  const rb_iseq_t *iseq = (const rb_iseq_t *) %p;\n"
                  "  if(!(block->iseq == iseq)) {\n"
                  "    goto L_exit%ld;\n"
                  "  }\n"
                  "}\n", ir->R, block->iseq, ExitBlockId);
      break;
    }
    case OPCODE_IGuardProperty : {
      IGuardProperty *ir = (IGuardProperty *) Inst;
      long ExitBlockId = GetBlockId(SideExitBBs, ir->Exit);
      if (ir->is_attr) {
        CALL_INFO ci = (CALL_INFO) ir->cache;
        cgen_printf(gen,
                    "if(!(%ld > 0)) {\n"
                    "  goto L_exit%ld;\n"
                    "}\n", ci->aux.index, ExitBlockId);
      }
      else {
        IC ci = (IC) ir->cache;
        cgen_printf(gen,
                    "if(!(RCLASS_SERIAL(RBASIC(v%ld)->klass) == 0x%llx)) {\n"
                    "  goto L_exit%ld;\n"
                    "}\n", ir->R, ci->ic_serial, ExitBlockId);
      }
      break;
    }
    case OPCODE_IGuardMethodCache : {
      IGuardMethodCache *ir = (IGuardMethodCache *) Inst;
      long ExitBlockId = GetBlockId(SideExitBBs, ir->Exit);
      cgen_printf(gen,
                  "{ CALL_INFO ci = (CALL_INFO) %p;\n"
                  "  if (!(GET_GLOBAL_METHOD_STATE()     == ci->method_state &&\n"
                  "        RCLASS_SERIAL(CLASS_OF(v%ld)) == ci->class_serial)) {\n"
                  "    goto L_exit%ld;\n"
                  "  }\n"
                  "}\n", ir->ci, ir->R, ExitBlockId);
      break;
    }
    case OPCODE_IGuardMethodRedefine : {
      IGuardMethodRedefine *ir = (IGuardMethodRedefine *) Inst;
      long ExitBlockId = GetBlockId(SideExitBBs, ir->Exit);
      cgen_printf(gen,
                  "if (!BASIC_OP_UNREDEFINED_P(%d, %d)) {\n"
                  "  goto L_exit%ld;\n"
                  "}\n",
                  (int) ir->bop, (int)vm_redefinition_check_flag(ir->klass),
                  ExitBlockId);
      break;
    }
    case OPCODE_IFixnumAdd : {
      IFixnumAdd *ir = (IFixnumAdd *) Inst;
      cgen_printf(gen,
                  "  v%ld = ((v%ld + (v%ld & (~1)))) | FIXNUM_FLAG;\n",
                  Id, ir->LHS, ir->RHS);
      assert(0 && "not implemented");
      break;
    }
    case OPCODE_IFixnumSub : {
      IFixnumSub *ir = (IFixnumSub *) Inst;
      cgen_printf(gen,
                  "  v%ld = LONG2FIX(FIX2LONG(v%ld) - FIX2LONG(v%ld));\n",
                  Id, ir->LHS, ir->RHS);
      assert(0 && "not implemented");
      break;
    }
    case OPCODE_IFixnumMul : {
      IFixnumMul *ir = (IFixnumMul *) Inst;
      cgen_printf(gen,
                  "  v%ld = LONG2FIX(FIX2LONG(v%ld) * FIX2LONG(v%ld));\n",
                  Id, ir->LHS, ir->RHS);
      assert(0 && "not implemented");
      break;
    }
    case OPCODE_IFixnumDiv : {
      IFixnumDiv *ir = (IFixnumDiv *) Inst;
      cgen_printf(gen,
                  "  v%ld = LONG2FIX(FIX2LONG(v%ld) / FIX2LONG(v%ld));\n",
                  Id, ir->LHS, ir->RHS);
      assert(0 && "not implemented");
      break;
    }
    case OPCODE_IFixnumMod : {
      IFixnumMod *ir = (IFixnumMod *) Inst;
      cgen_printf(gen,
                  "  v%ld = LONG2FIX(FIX2LONG(v%ld) %% FIX2LONG(v%ld));\n",
                  Id, ir->LHS, ir->RHS);
      break;
    }
    case OPCODE_IFixnumAddOverflow : {
      IFixnumAddOverflow *ir = (IFixnumAddOverflow *) Inst;
      cgen_printf(gen,
                  "{ VALUE val;\n"
                  "  VALUE recv = v%ld;\n"
                  "  VALUE obj  = v%ld;\n"
                  //"#ifndef LONG_LONG_VALUE\n"
                  "  val = (recv + (obj & (~1)));\n"
                  "  if ((~(recv ^ obj) & (recv ^ val)) &\n"
                  "    ((VALUE)0x01 << ((sizeof(VALUE) * CHAR_BIT) - 1))) {\n"
                  "    val = rb_big_plus(rb_int2big(FIX2LONG(recv)),\n"
                  "                      rb_int2big(FIX2LONG(obj)));\n"
                  "  }\n"
                  //"#else\n"
                  //"  long a, b, c;\n"
                  //"  a = FIX2LONG(recv);\n"
                  //"  b = FIX2LONG(obj);\n"
                  //"  c = a + b;\n"
                  //"  if (FIXABLE(c)) {\n"
                  //"    val = LONG2FIX(c);\n"
                  //"  }\n"
                  //"  else {\n"
                  //"    val = rb_big_plus(rb_int2big(a), rb_int2big(b));\n"
                  //"  }\n"
                  //"#endif\n"
          "  v%ld = val;\n"
          "}\n", ir->LHS, ir->RHS, Id);
      break;
    }
    case OPCODE_IFixnumSubOverflow : {
      IFixnumSubOverflow *ir = (IFixnumSubOverflow *) Inst;
      cgen_printf(gen,
                  "{ VALUE val;\n"
                  "  VALUE recv = v%ld;\n"
                  "  VALUE obj  = v%ld;\n"
                  "  long a, b, c;\n"
                  "  \n"
                  "  a = FIX2LONG(recv);\n"
                  "  b = FIX2LONG(obj);\n"
                  "  c = a - b;\n"
                  "  \n"
                  "  if (FIXABLE(c)) {\n"
                  "      val = LONG2FIX(c);\n"
                  "  }\n"
                  "  else {\n"
                  "      val = rb_big_minus(rb_int2big(a), rb_int2big(b));\n"
                  "  }\n"
                  "  v%ld = val;\n"
                  "}\n", ir->LHS, ir->RHS, Id);
      break;
    }
    case OPCODE_IFixnumMulOverflow : {
      IFixnumMulOverflow *ir = (IFixnumMulOverflow *) Inst;
      cgen_printf(gen,
                  "{ VALUE val;\n"
                  "  VALUE recv = v%ld;\n"
                  "  VALUE obj  = v%ld;\n"
                  "  long a, b;\n"
                  "  \n"
                  "  a = FIX2LONG(recv);\n"
                  "  if (a == 0) {\n"
                  "    val = recv;\n"
                  "  }\n"
                  "  else {\n"
                  "    b = FIX2LONG(obj);\n"
                  "    if (MUL_OVERFLOW_FIXNUM_P(a, b)) {\n"
                  "      val = rb_big_mul(rb_int2big(a), rb_int2big(b));\n"
                  "    }\n"
                  "    else {\n"
                  "      val = LONG2FIX(a * b);\n"
                  "    }\n"
                  "  }\n"
                  "  v%ld = val;\n"
                  "}\n", ir->LHS, ir->RHS, Id);
      break;
    }
    case OPCODE_IFixnumDivOverflow : {
      IFixnumDivOverflow *ir = (IFixnumDivOverflow *) Inst;
      cgen_printf(gen,
                  "{\n"
                  "  VALUE recv = v%ld;\n"
                  "  VALUE obj  = v%ld;\n"
                  "  long x = FIX2LONG(recv);\n"
                  "  long y = FIX2LONG(obj);\n"
                  "  x = (x > 0)? x : -x;\n"
                  "  y = (y > 0)? y : -y;\n"
                  "  long div = x / y;\n"
                  "  long mod = x - div * y;\n"
                  "  if ((mod < 0 && y > 0) || (mod > 0 && y < 0)) {\n"
                  "    mod += y;\n"
                  "    div -= 1;\n"
                  "  }\n"
                  "  v%ld = LONG2NUM(div);\n"
                  "}\n", ir->LHS, ir->RHS, Id);
      break;
    }
    case OPCODE_IFixnumModOverflow : {
      IFixnumModOverflow *ir = (IFixnumModOverflow *) Inst;
      cgen_printf(gen,
                  "{\n"
                  "  VALUE recv = v%ld;\n"
                  "  VALUE obj  = v%ld;\n"
                  "  long x = FIX2LONG(recv);\n"
                  "  long y = FIX2LONG(obj);\n"
                  "  x = (x > 0)? x : -x;\n"
                  "  y = (y > 0)? y : -y;\n"
                  "  long div = x / y;\n"
                  "  long mod = x - div * y;\n"
                  "  if ((mod < 0 && y > 0) || (mod > 0 && y < 0)) {\n"
                  "    mod += y;\n"
                  "    div -= 1;\n"
                  "  }\n"
                  "  v%ld = LONG2NUM(mod);\n"
                  "}\n", ir->LHS, ir->RHS, Id);
      break;
    }
    case OPCODE_IFixnumEq : {
      IFixnumEq *ir = (IFixnumEq *) Inst;
      generate_fixnum_cond(gen, "==", Id, ir->LHS, ir->RHS);
      break;
    }
    case OPCODE_IFixnumNe : {
      IFixnumNe *ir = (IFixnumNe *) Inst;
      generate_fixnum_cond(gen, "!=", Id, ir->LHS, ir->RHS);
      break;
    }
    case OPCODE_IFixnumGt : {
      IFixnumGt *ir = (IFixnumGt *) Inst;
      generate_fixnum_cond(gen, ">", Id, ir->LHS, ir->RHS);
      break;
    }
    case OPCODE_IFixnumGe : {
      IFixnumGe *ir = (IFixnumGe *) Inst;
      generate_fixnum_cond(gen, ">=", Id, ir->LHS, ir->RHS);
      break;
    }
    case OPCODE_IFixnumLt : {
      IFixnumLt *ir = (IFixnumLt *) Inst;
      generate_fixnum_cond(gen, "<", Id, ir->LHS, ir->RHS);
      break;
    }
    case OPCODE_IFixnumLe : {
      IFixnumLe *ir = (IFixnumLe *) Inst;
      generate_fixnum_cond(gen, "<=", Id, ir->LHS, ir->RHS);
      break;
    }
    case OPCODE_IFixnumAnd : {
      IFixnumAnd *ir = (IFixnumAnd *) Inst;
      cgen_printf(gen, "  v%ld = LONG2NUM(FIX2LONG(v%ld) & FIX2LONG(v%ld));\n",
                  Id, ir->LHS, ir->RHS);
      break;
    }
    case OPCODE_IFixnumOr : {
      IFixnumOr *ir = (IFixnumOr *) Inst;
      cgen_printf(gen, "  v%ld = LONG2NUM(FIX2LONG(v%ld) | FIX2LONG(v%ld));\n",
                  Id, ir->LHS, ir->RHS);
      break;
    }
    case OPCODE_IFixnumXor : {
      IFixnumXor *ir = (IFixnumXor *) Inst;
      cgen_printf(gen, "  v%ld = LONG2NUM(FIX2LONG(v%ld) ^ FIX2LONG(v%ld));\n",
                  Id, ir->LHS, ir->RHS);
      break;
    }
    case OPCODE_IFixnumLshift : {
      IFixnumLshift *ir = (IFixnumLshift *) Inst;
      assert(0 && "not implemented");
      cgen_printf(gen, "  v%ld = LONG2NUM(FIX2LONG(v%ld) << FIX2LONG(v%ld));\n",
                  Id, ir->LHS, ir->RHS);
      break;
    }
    case OPCODE_IFixnumRshift : {
      IFixnumRshift *ir = (IFixnumRshift *) Inst;
      assert(0 && "not implemented");
      cgen_printf(gen, "  v%ld = LONG2NUM(FIX2LONG(v%ld) >> FIX2LONG(v%ld));\n",
                  Id, ir->LHS, ir->RHS);
      break;
    }
    case OPCODE_IFixnumComplement : {
      IFixnumComplement *ir = (IFixnumComplement *) Inst;
      cgen_printf(gen, "  v%ld = ~v%ld | FIXNUM_FLAG;\n",
                  Id, ir->Recv);
      assert(0 && "not implemented");
      break;
    }
    case OPCODE_IFloatAdd : {
      IFloatAdd *ir = (IFloatAdd *) Inst;
      cgen_printf(gen,
                  "v%ld = DBL2NUM(RFLOAT_VALUE(v%ld) + RFLOAT_VALUE(v%ld));\n",
                  Id, ir->LHS, ir->RHS);
      break;
    }
    case OPCODE_IFloatSub : {
      IFloatSub *ir = (IFloatSub *) Inst;
      cgen_printf(gen,
                  "v%ld = DBL2NUM(RFLOAT_VALUE(v%ld) - RFLOAT_VALUE(v%ld));\n",
                  Id, ir->LHS, ir->RHS);
      break;
    }
    case OPCODE_IFloatMul : {
      IFloatMul *ir = (IFloatMul *) Inst;
      cgen_printf(gen,
                  "v%ld = DBL2NUM(RFLOAT_VALUE(v%ld) * RFLOAT_VALUE(v%ld));\n",
                  Id, ir->LHS, ir->RHS);
      break;
    }
    case OPCODE_IFloatDiv : {
      IFloatDiv *ir = (IFloatDiv *) Inst;
      cgen_printf(gen,
                  "v%ld = DBL2NUM(RFLOAT_VALUE(v%ld) / RFLOAT_VALUE(v%ld));\n",
                  Id, ir->LHS, ir->RHS);
      break;
    }
    case OPCODE_IFloatMod : {
      IFloatMod *ir = (IFloatMod *) Inst;
      cgen_printf(gen,
                  "v%ld = DBL2NUM(ruby_float_mod(RFLOAT_VALUE(v%ld),"
                  "RFLOAT_VALUE(v%ld)));\n",
                  Id, ir->LHS, ir->RHS);
      break;
    }
    case OPCODE_IFloatEq : {
      IFloatEq *ir = (IFloatEq *) Inst;
      generate_flonum_cond(gen, "==", Id, ir->LHS, ir->RHS);
      break;
    }
    case OPCODE_IFloatNe : {
      IFloatNe *ir = (IFloatNe *) Inst;
      generate_flonum_cond(gen, "!=", Id, ir->LHS, ir->RHS);
      break;
    }
    case OPCODE_IFloatGt : {
      IFloatGt *ir = (IFloatGt *) Inst;
      generate_flonum_cond(gen, ">", Id, ir->LHS, ir->RHS);
      break;
    }
    case OPCODE_IFloatGe : {
      IFloatGe *ir = (IFloatGe *) Inst;
      generate_flonum_cond(gen, ">=", Id, ir->LHS, ir->RHS);
      break;
    }
    case OPCODE_IFloatLt : {
      IFloatLt *ir = (IFloatLt *) Inst;
      generate_flonum_cond(gen, "<", Id, ir->LHS, ir->RHS);
      break;
    }
    case OPCODE_IFloatLe : {
      IFloatLe *ir = (IFloatLe *) Inst;
      generate_flonum_cond(gen, "<=", Id, ir->LHS, ir->RHS);
      break;
    }
    case OPCODE_IFixnumToFloat : {
      IFixnumToFloat *ir = (IFixnumToFloat *) Inst;
      cgen_printf(gen, "v%ld = DBL2NUM((double) v%ld);\n", Id, ir->Val);
      break;
    }
    case OPCODE_IFixnumToString : {
      IFixnumToString *ir = (IFixnumToString *) Inst;
      cgen_printf(gen, "v%ld = flo_to_s(v%ld);\n", Id, ir->Val);
      break;
    }
    case OPCODE_IFloatToFixnum : {
      IFloatToFixnum *ir = (IFloatToFixnum *) Inst;
      assert(0 && "not implemented");
      break;
    }
    case OPCODE_IFloatToString : {
      IFloatToString *ir = (IFloatToString *) Inst;
      assert(0 && "not implemented");
      break;
    }
    case OPCODE_IStringToFixnum : {
      IStringToFixnum *ir = (IStringToFixnum *) Inst;
      assert(0 && "not implemented");
      break;
    }
    case OPCODE_IStringToFloat : {
      IStringToFloat *ir = (IStringToFloat *) Inst;
      assert(0 && "not implemented");
      break;
    }
    case OPCODE_IMathSin : {
      IMathSin *ir = (IMathSin *) Inst;
      cgen_printf(gen, "v%ld = DBL2NUM(sin(RFLOAT_VALUE(v%ld)));\n",
                  Id, ir->Recv);
      break;
    }
    case OPCODE_IMathCos : {
      IMathCos *ir = (IMathCos *) Inst;
      cgen_printf(gen, "v%ld = DBL2NUM(cos(RFLOAT_VALUE(v%ld)));\n",
                  Id, ir->Recv);
      break;
    }
    case OPCODE_IMathTan : {
      IMathTan *ir = (IMathTan *) Inst;
      cgen_printf(gen, "v%ld = DBL2NUM(tan(RFLOAT_VALUE(v%ld)));\n",
                  Id, ir->Recv);
      break;
    }
    case OPCODE_IMathExp : {
      IMathExp *ir = (IMathExp *) Inst;
      cgen_printf(gen, "v%ld = DBL2NUM(exp(RFLOAT_VALUE(v%ld)));\n",
                  Id, ir->Recv);
      break;
    }
    case OPCODE_IMathSqrt : {
      IMathSqrt *ir = (IMathSqrt *) Inst;
      cgen_printf(gen, "v%ld = DBL2NUM(sqrt(RFLOAT_VALUE(v%ld)));\n",
                  Id, ir->Recv);
      break;
    }
    case OPCODE_IMathLog10 : {
      IMathLog10 *ir = (IMathLog10 *) Inst;
      cgen_printf(gen, "v%ld = DBL2NUM(log10(RFLOAT_VALUE(v%ld)));\n",
                  Id, ir->Recv);
      break;
    }
    case OPCODE_IMathLog2 : {
      IMathLog2 *ir = (IMathLog2 *) Inst;
      cgen_printf(gen, "v%ld = DBL2NUM(log2(RFLOAT_VALUE(v%ld)));\n",
                  Id, ir->Recv);
      break;
    }
    case OPCODE_IStringLength : {
      IStringLength *ir = (IStringLength *) Inst;
      cgen_printf(gen, "v%ld = rb_str_length(v%ld);\n", Id, ir->Recv);
      break;
    }
    case OPCODE_IStringEmptyP : {
      IStringEmptyP *ir = (IStringEmptyP *) Inst;
      cgen_printf(gen, "v%ld = (RSTRING_LEN(v%ld) == 0) ? Qtrue : Qfalse;\n",
                  Id, ir->Recv);
      break;
    }
    case OPCODE_IStringConcat : {
      IStringConcat *ir = (IStringConcat *) Inst;
      assert(0 && "not implemented");
      break;
    }
    case OPCODE_IArrayLength : {
      IArrayLength *ir = (IArrayLength *) Inst;
      cgen_printf(gen, "v%ld = LONG2NUM(RARRAY_LEN(v%ld));\n",
                  Id, ir->Recv);
      break;
    }
    case OPCODE_IArrayEmptyP : {
      IArrayEmptyP *ir = (IArrayEmptyP *) Inst;
      cgen_printf(gen, "v%ld = (RARRAY_LEN(v%ld) == 0) ? Qtrue : Qfalse;\n",
                  Id, ir->Recv);
      break;
    }
    case OPCODE_IArrayConcat : {
      IArrayConcat *ir = (IArrayConcat *) Inst;
      assert(0 && "not implemented");
      break;
    }
    case OPCODE_IArrayGet : {
      IArrayGet *ir = (IArrayGet *) Inst;
      cgen_printf(gen, "v%ld = rb_ary_entry(v%ld, FIX2LONG(v%ld));\n",
                  Id, ir->Recv, ir->Index);
      break;
    }
    case OPCODE_IArraySet : {
      IArraySet *ir = (IArraySet *) Inst;
      cgen_printf(gen,
                  "rb_ary_store(v%ld, FIX2LONG(v%ld), v%ld);\n"
                  "v%ld = v%ld;\n", ir->Recv, ir->Index, ir->Val, Id, ir->Val);
      break;
    }
    case OPCODE_IHashLength : {
      IHashLength *ir = (IHashLength *) Inst;
      cgen_printf(gen, "v%ld = INT2FIX(RHASH_SIZE(v%ld));\n",
                  Id, ir->Recv);
      break;
    }
    case OPCODE_IHashEmptyP : {
      IHashEmptyP *ir = (IHashEmptyP *) Inst;
      cgen_printf(gen, "v%ld = (RHASH_EMPTY_P(v%ld) == 0) ? Qtrue : Qfalse;\n",
                  Id, ir->Recv);
      break;
    }
    case OPCODE_IHashGet : {
      IHashGet *ir = (IHashGet *) Inst;
      cgen_printf(gen,
                  "  v%ld = rb_hash_aref(v%ld, v%ld);\n",
                  Id, ir->Recv, ir->Index);
      break;
    }
    case OPCODE_IHashSet : {
      IHashSet *ir = (IHashSet *) Inst;
      assert(0 && "not implemented");
      cgen_printf(gen,
                  "  rb_hash_aset(v%ld, v%ld, v%ld);\n"
                  "  v%ld = v%ld;\n",
                  ir->Recv, ir->Index, ir->Val, Id, ir->Val);
      break;
    }
    case OPCODE_IRegExpMatch : {
      IRegExpMatch *ir = (IRegExpMatch *) Inst;
      cgen_printf(gen,
                  "  v%ld = rb_reg_match(v%ld, v%ld);\n",
                  Id, ir->Re, ir->Str);
      break;
    }
    case OPCODE_IAllocArray : {
      IAllocArray *ir = (IAllocArray *) Inst;
      int i;
      cgen_printf(gen,
                  "{\n"
                  "  long num = %d;\n"
                  "  VALUE argv[%d];\n", ir->argc, ir->argc);
      for (i = 0; i < ir->argc; i++) {
        cgen_printf(gen, "argv[%d] = v%ld;\n", i, ir->argv[i]);
      }
      cgen_printf(gen,
                  "  v%ld = rb_ary_new4(num, argv);\n"
                  "}\n", Id);
      break;
    }
    case OPCODE_IAllocHash : {
      IAllocHash *ir = (IAllocHash *) Inst;
      int i;
      cgen_printf(gen,
                  "{\n"
                  "  VALUE val = rb_hash_new();\n");
      for (i = 0; i < ir->argc; i += 2) {
        cgen_printf(gen, "rb_hash_aset(val, v%ld, v%ld);\n",
                    ir->argv[i], ir->argv[i+1]);
      }
      cgen_printf(gen,
                  "  v%ld = val;\n"
                  "}\n", Id);

      break;
    }
    case OPCODE_IAllocString : {
      IAllocString *ir = (IAllocString *) Inst;
      assert(0 && "not implemented");
      break;
    }
    case OPCODE_IAllocRange : {
      IAllocRange *ir = (IAllocRange *) Inst;
      cgen_printf(gen,
                  "{\n"
                  "  long index = %d;\n"
                  "  VALUE low  = v%ld;\n"
                  "  VALUE high = v%ld;\n"
                  "  v%ld = rb_range_new(low, high, flag);\n"
                  "}\n", ir->Flag, ir->Low, ir->High, Id);

      break;
    }
    case OPCODE_IGetGlobal : {
      IGetGlobal *ir = (IGetGlobal *) Inst;
      cgen_printf(gen,
                  "  v%ld = GET_GLOBAL(v%ld);\n",
                  Id, ir->Entry);
      break;
    }
    case OPCODE_ISetGlobal : {
      ISetGlobal *ir = (ISetGlobal *) Inst;
      cgen_printf(gen,
                  "  SET_GLOBAL(v%ld, v%ld);\n",
                  ir->Entry, ir->Val);
      break;
    }
    case OPCODE_IGetPropertyName : {
      IGetPropertyName *ir = (IGetPropertyName *) Inst;
      cgen_printf(gen,
                  "{\n"
                  "  long index = %ld;\n"
                  "  VALUE  obj = v%ld;\n"
                  "  VALUE *ptr = ROBJECT_IVPTR(obj);\n"
                  "  v%ld = ptr[index];\n"
                  "}\n", ir->Index, ir->Recv, Id);
      break;
    }
    case OPCODE_ISetPropertyName : {
      ISetPropertyName *ir = (ISetPropertyName *) Inst;
      cgen_printf(gen,
                  "{\n"
                  "  long index = %ld;\n"
                  "  VALUE  obj = v%ld;\n"
                  "  VALUE  val = v%ld;\n"
                  "  VALUE *ptr = ROBJECT_IVPTR(obj);\n"
                  "  RB_OBJ_WRITE(obj, &ptr[index], val);\n"
                  "  v%ld = ptr[index] = val;\n"
                  "}\n", ir->Index, ir->Recv, ir->Val, Id);
      break;
    }
    case OPCODE_ILoadSelf : {
      cgen_printf(gen, "v%ld = GET_SELF();\n", Id);
      break;
    }
    case OPCODE_ILoadSelfAsBlock : {
      ILoadSelfAsBlock *ir = (ILoadSelfAsBlock *) Inst;
      cgen_printf(gen,
                  "{\n"
                  "  ISEQ blockiseq = (ISEQ) %p;\n"
                  "  v%ld = (VALUE) RUBY_VM_GET_BLOCK_PTR_IN_CFP(reg_cfp);\n"
                  "  assert(((rb_block_t *)v%ld)->iseq == NULL);\n"
                  "  ((rb_block_t *)v%ld)->iseq = blockiseq;\n"
                  "}\n",
                  ir->iseq, Id, Id, Id);
      break;
    }
    case OPCODE_ILoadBlock : {
      ILoadBlock *ir = (ILoadBlock *) Inst;
      cgen_printf(gen,
                  "{\n"
                  "  v%ld = (VALUE) VM_CF_BLOCK_PTR(reg_cfp);\n"
                  "}\n",
                  Id);
      break;
    }
    case OPCODE_ILoadConstNil : {
      cgen_printf(gen, "v%ld = Qnil;\n", Id);
      break;
    }
    case OPCODE_ILoadConstObject : {
      ILoadConstObject *ir = (ILoadConstObject *) Inst;
      cgen_printf(gen, "v%ld = (VALUE) 0x%lx;\n", Id, ir->Val);
      break;
    }
    case OPCODE_ILoadConstFixnum : {
      ILoadConstFixnum *ir = (ILoadConstFixnum *) Inst;
      cgen_printf(gen, "v%ld = (VALUE) 0x%lx;\n", Id, ir->Val);
      break;
    }
    case OPCODE_ILoadConstFloat : {
      ILoadConstFloat *ir = (ILoadConstFloat *) Inst;
      cgen_printf(gen, "v%ld = (VALUE) 0x%lx;\n", Id, ir->Val);
      break;
    }
    case OPCODE_ILoadConstString : {
      ILoadConstString *ir = (ILoadConstString *) Inst;
      cgen_printf(gen, "v%ld = (VALUE) 0x%lx;\n", Id, ir->Val);
      break;
    }
    case OPCODE_IStackStore : {
      IStackStore *ir = (IStackStore *) Inst;
      if (ir->Level > 0) {
        cgen_printf(gen,
                    "{\n"
                    "  int i, lev = (int)%d;\n"
                    "  VALUE *ep = GET_EP();\n"
                    "\n"
                    "  for (i = 0; i < lev; i++) {\n"
                    "      ep = GET_PREV_EP(ep);\n"
                    "  }\n"
                    "  *(ep - %d) = v%ld;\n"
                    "}\n", ir->Level, ir->Index, ir->Val);

      }
      else {
        cgen_printf(gen, "  *(GET_EP() - %d) = v%ld;\n", ir->Index, ir->Val);

      }
      break;
    }
    case OPCODE_IStackLoad : {
      IStackLoad *ir = (IStackLoad *) Inst;
      if (ir->Level > 0) {
        cgen_printf(gen,
                    "{\n"
                    "  int i, lev = (int)%d;\n"
                    "  VALUE *ep = GET_EP();\n"
                    "\n"
                    "  for (i = 0; i < lev; i++) {\n"
                    "      ep = GET_PREV_EP(ep);\n"
                    "  }\n"
                    "  v%ld = *(ep - %d);\n"
                    "}\n",
                    ir->Level, Id, ir->Index);
      }
      else {
        cgen_printf(gen, "  v%ld = *(GET_EP() - %d);\n", Id, ir->Index);
      }
      break;
    }
    case OPCODE_IInvokeMethod : {
      IInvokeMethod *ir = (IInvokeMethod *) Inst;
      int i;
      cgen_printf(gen,
                  "{\n"
                  "  VALUE *basesp = GET_SP();\n"
                  "  CALL_INFO ci = (CALL_INFO) %p;\n", ir->ci);
      for (i = 0; i < ir->argc; i++) {
        cgen_printf(gen, "(GET_SP())[%d] = v%ld;\n", i, ir->argv[i]);
      }
      cgen_printf(gen, "SET_SP(basesp + %d);\n"
                  "  ci->recv = v%ld;\n"
                  "  v%ld = (*(ci)->call)(th, GET_CFP(), (ci));\n"
                  "  assert(v%ld != Qundef && \"method must c-defined method\");\n"
                  "  SET_SP(basesp);\n"
                  "}\n", ir->argc, ir->argv[0], Id, Id);
      break;
    }
    case OPCODE_IInvokeNative : {
      int i;
      IInvokeNative *ir = (IInvokeNative *) Inst;
      cgen_printf(gen, "  v%ld = ((gwjit_native_func%d_t)%p)(",
                  Id, ir->argc, ir->fptr);
      for (i = 0; i < ir->argc; i++) {
        if (i != 0) {
          cgen_printf(gen, ", ");
        }
        cgen_printf(gen, "v%ld", ir->argv[i]);
      }
      cgen_printf(gen, ");\n");
      break;
    }
    case OPCODE_IJump : {
      IJump *ir = (IJump *) Inst;
      BasicBlock *BB = FindBasicBlockByPC(builder, ir->TargetBB);
      cgen_printf(gen, "goto L_%d;\n", BB->base.id);
      break;
    }
    case OPCODE_IJumpIf : {
      IJumpIf *ir = (IJumpIf *) Inst;
      assert(0 && "not implemented");
      break;
    }
    case OPCODE_IThrow : {
      IThrow *ir = (IThrow *) Inst;
      assert(0 && "not implemented");
      break;
    }
    case OPCODE_IFramePush : {
      IFramePush *ir = (IFramePush *) Inst;
      EmitFramePush(builder, gen, ir);
      break;
    }
    case OPCODE_IFramePop : {
      //IFramePop *ir = (IFramePop *) Inst;
      cgen_printf(gen,
                  "vm_pop_frame(th);\n"
                  "reg_cfp = th->cfp;\n"
                 );
      break;
    }
    //case OPCODE_IPhi : {
    //  IPhi *ir = (IPhi *) Inst;
    //  assert(0 && "not implemented");
    //  break;
    //}
    case OPCODE_ITrace : {
      // FIXME
      // When we enable trace code, clang saied error.
      // >> error: Must have a valid dtrace stability entry'
      // >> ld: error creating dtrace DOF section for architecture x86_64'
      // We want to enable dtrace for compatibility but we have no time to
      // implement.
#if 0
      ITrace *ir = (ITrace *) Inst;
      cgen_printf(gen,
                  "{\n"
                  "  rb_event_flag_t flag = (rb_event_flag_t)%ld;\n"
                  "  if (RUBY_DTRACE_METHOD_ENTRY_ENABLED() ||\n"
                  "      RUBY_DTRACE_METHOD_RETURN_ENABLED() ||\n"
                  "      RUBY_DTRACE_CMETHOD_ENTRY_ENABLED() ||\n"
                  "      RUBY_DTRACE_CMETHOD_RETURN_ENABLED()) {\n"
                  "\n"
                  "    switch(flag) {\n"
                  "      case RUBY_EVENT_CALL:\n"
                  "        RUBY_DTRACE_METHOD_ENTRY_HOOK(th, 0, 0);\n"
                  "        break;\n"
                  "      case RUBY_EVENT_C_CALL:\n"
                  "        RUBY_DTRACE_CMETHOD_ENTRY_HOOK(th, 0, 0);\n"
                  "        break;\n"
                  "      case RUBY_EVENT_RETURN:\n"
                  "        RUBY_DTRACE_METHOD_RETURN_HOOK(th, 0, 0);\n"
                  "        break;\n"
                  "      case RUBY_EVENT_C_RETURN:\n"
                  "        RUBY_DTRACE_CMETHOD_RETURN_HOOK(th, 0, 0);\n"
                  "        break;\n"
                  "    }\n"
                  "  }\n"
                  "\n"
                  "  EXEC_EVENT_HOOK(th, flag, GET_SELF(), 0,\n"
                  "                  0/*id and klass are resolved at callee */,\n"
                  "                  (flag & (RUBY_EVENT_RETURN |\n"
                  "                  RUBY_EVENT_B_RETURN)) ? TOPN(0) : Qundef);\n"
                  "}\n", ir->Flag);
#endif
      break;
    }
    default:
      assert(false && "unreachable");
  }
}
