// MIR backend: builds MIR modules (https://github.com/vnmakarov/mir) from
// the slimcc AST instead of emitting x86-64 assembly. Linked in place of
// codegen.c; provides the same backend boundary symbols. MIR performs
// register allocation, instruction selection, and per-target ABI handling
// (x86-64, aarch64, riscv64, ...), so this file stays target-independent.
//
// Like codegen.c, functions are emitted one at a time during parsing
// (emit_text), because the AST arena is recycled per top-level declaration.
// codegen() then emits file-scope data and finishes the module.
#include "slimcc.h"
#include "mir.h"

struct FuncObj {
  MIR_item_t item;
};

typedef struct {
  const char *key; // interned/persistent string
  MIR_item_t item;
  bool is_defined; // a definition was emitted into this module
} SymItem;

static MIR_context_t mc;
static MIR_module_t cur_mod;
static MIR_module_t result_mod;

static MIR_item_t fn_item;
static MIR_func_t fn_func;
static Obj *cur_fn;
static MIR_reg_t ret_addr_reg; // hidden struct-return buffer arg, 0 if none
static int64_t tmp_cnt;
static int64_t proto_cnt;
static int64_t ctrl_cnt;

static HashMap sym_items;  // symbol name -> SymItem (module lifetime)
static HashMap label_map;  // label key -> MIR_label_t (function lifetime)

static MIR_item_t memset_import, memcpy_import;
static MIR_item_t memset_proto, memcpy_proto;
static MIR_item_t atomic_imports[9], atomic_protos[9]; // cas 1/2/4/8, exch 1/2/4/8, fence
static MIR_item_t emutls_import, emutls_proto;
static MIR_reg_t scratch_slot;  // lazily created 8-byte entry alloca, 0 if none
static MIR_reg_t vla_base_slot; // slot holding entry stack position, 0 if none

static MIR_reg_t gen_expr(Node *node);
static MIR_reg_t gen_addr(Node *node);
static void gen_void_expr(Node *node);
static void gen_stmt(Node *node);
static void gen_cond(Node *node, bool jump_on_true, MIR_label_t lab);
static void emit_data_obj(Obj *var);
static MIR_reg_t i64_op2(MIR_insn_code_t code, MIR_reg_t a, MIR_op_t b);
typedef enum {
  BI_NEG, BI_BITNOT, BI_BITAND, BI_BITOR, BI_BITXOR,
  BI_ADD, BI_SUB, BI_MUL, BI_DIV, BI_SHL, BI_SHR,
  BI_CMP, BI_TO_BOOL, BI_SIGN_EXT, BI_OVERFLOW, BI_BF_LOAD, BI_BF_SAVE,
  BI_CNT
} BitintHelper;

static void gen_mem_copy(MIR_reg_t dst, MIR_reg_t src, int64_t size);
static bool is_big_bitint(Type *ty);
static MIR_reg_t bitint_buf(int64_t size);
static MIR_reg_t bitint_copy(Type *ty, MIR_reg_t src);
static MIR_reg_t bitint_norm(MIR_reg_t r, Type *ty);
static MIR_reg_t bitint_call(BitintHelper h, int nargs, MIR_op_t *args);
static MIR_reg_t bitint_to_bool(Type *ty, MIR_reg_t addr);
static MIR_reg_t load_bitint_bitfield(Member *mem, MIR_reg_t addr);
static MIR_reg_t store_bitint_bitfield(Member *mem, MIR_reg_t addr, MIR_reg_t val);

// Whether a function definition is externally visible (mirrors codegen.c's
// export_fn). Inline definitions that are not externally visible stay as
// module-local functions instead of being dropped like codegen.c does.
static bool fn_is_exported(Obj *fn) {
  if (fn->is_static)
    return false;
  if (fn->is_gnu_inline || opt_gnu89_inline || opt_std < STD_C99)
    return fn->export_fn_gnu;
  return fn->export_fn;
}

//
// Helpers
//

int64_t align_to(int64_t n, int64_t align) {
  return (n + align - 1) / align * align;
}

bool va_arg_need_copy(Type *ty) {
  // Aggregates and big bitints fetched with va_arg go through a buffer
  // the parser allocates; MIR_VA_BLOCK_ARG fills it directly.
  return ty->kind == TY_STRUCT || ty->kind == TY_UNION ||
         (ty->kind == TY_BITINT && ty->bit_cnt > 64);
}

bool bitint_rtn_need_copy(size_t width) {
  return width > 64;
}

static void out(MIR_insn_t insn) {
  MIR_append_insn(mc, fn_item, insn);
}

static void out_lab(MIR_label_t lab) {
  MIR_append_insn(mc, fn_item, lab);
}

static MIR_reg_t new_tmp(MIR_type_t ty) {
  char buf[32];
  snprintf(buf, sizeof(buf), "T%" PRIi64, tmp_cnt++);
  return MIR_new_func_reg(mc, fn_func, ty, buf);
}

static MIR_op_t rop(MIR_reg_t r) {
  return MIR_new_reg_op(mc, r);
}

static MIR_op_t iop(int64_t v) {
  return MIR_new_int_op(mc, v);
}

static bool is_scalar_fp(Type *ty) {
  return ty->kind == TY_FLOAT || ty->kind == TY_DOUBLE || ty->kind == TY_LDOUBLE;
}

// MIR register class for values of this type.
static MIR_type_t mir_class(Type *ty) {
  switch (ty->kind) {
  case TY_FLOAT: return MIR_T_F;
  case TY_DOUBLE: return MIR_T_D;
  case TY_LDOUBLE: return MIR_T_LD;
  default: return MIR_T_I64;
  }
}

// Precise MIR type, used for memory accesses and proto/func signatures.
static MIR_type_t mir_type(Type *ty) {
  switch (ty->kind) {
  case TY_BOOL:
  case TY_PCHAR:
  case TY_CHAR: return ty->is_unsigned ? MIR_T_U8 : MIR_T_I8;
  case TY_SHORT: return ty->is_unsigned ? MIR_T_U16 : MIR_T_I16;
  case TY_INT: return ty->is_unsigned ? MIR_T_U32 : MIR_T_I32;
  case TY_LONG:
  case TY_LONGLONG: return ty->is_unsigned ? MIR_T_U64 : MIR_T_I64;
  case TY_FLOAT: return MIR_T_F;
  case TY_DOUBLE: return MIR_T_D;
  case TY_LDOUBLE: return MIR_T_LD;
  case TY_PTR:
  case TY_NULLPTR:
  case TY_ARRAY:
  case TY_VLA:
  case TY_FUNC: return MIR_T_I64;
  case TY_ENUM:
    switch (ty->size) {
    case 1: return ty->is_unsigned ? MIR_T_U8 : MIR_T_I8;
    case 2: return ty->is_unsigned ? MIR_T_U16 : MIR_T_I16;
    case 4: return ty->is_unsigned ? MIR_T_U32 : MIR_T_I32;
    default: return ty->is_unsigned ? MIR_T_U64 : MIR_T_I64;
    }
  case TY_BITINT:
    if (ty->bit_cnt <= 64) {
      switch (ty->size) {
      case 1: return ty->is_unsigned ? MIR_T_U8 : MIR_T_I8;
      case 2: return ty->is_unsigned ? MIR_T_U16 : MIR_T_I16;
      case 4: return ty->is_unsigned ? MIR_T_U32 : MIR_T_I32;
      default: return ty->is_unsigned ? MIR_T_U64 : MIR_T_I64;
      }
    }
    break;
  }
  internal_error();
}

static const char *sym_name(Obj *var) {
  return var->asm_name ? var->asm_name : var->name;
}

static SymItem *sym_entry(const char *raw_name) {
  // The Obj's name may live in the per-declaration AST arena (e.g.
  // function-static variables); the map outlives it, so intern a copy.
  const char *name = arena_strdup(&cc1_arena, raw_name);
  HashEntry *ent = hashmap_get_or_insert(&sym_items, name, strlen(name));
  SymItem *si = ent->val;
  if (!si) {
    si = calloc(1, sizeof(SymItem));
    si->key = name;
    ent->val = si;
  }
  return si;
}

// Get or create the module item used to reference a symbol. All references
// go through forward items; codegen() adds an import for any name that
// never received a definition in this module, which the forward then
// resolves to at load time.
static MIR_item_t sym_item(Obj *var) {
  SymItem *si = sym_entry(sym_name(var));
  if (!si->item)
    si->item = MIR_new_forward(mc, si->key);
  return si->item;
}

// Record that this module defines the name (emit_text / emit_data_obj).
static void sym_mark_defined(const char *name) {
  sym_entry(name)->is_defined = true;
}

// Labels are keyed by strings so that one map covers source labels
// ("j<id>"), loop/switch control labels ("b<id>", "c<id>"), and case
// labels ("s<id>.<n>").
static MIR_label_t get_label(const char *fmt, ...) {
  char buf[64];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  HashEntry *ent = hashmap_get_or_insert(&label_map, arena_strdup(&cc1_arena, buf), strlen(buf));
  if (!ent->val)
    ent->val = MIR_new_label(mc);
  return ent->val;
}

//
// Local variables
//

// A local Obj's slot-address register is stashed in var->ofs; var->ptr is
// set to this marker so misuse is caught.
static const char mir_local_marker[] = "<mir-local>";

static int32_t obj_align(Obj *var) {
  return var->alt_align ? var->alt_align : var->ty->align;
}

static void alloca_obj(Obj *var) {
  // A VLA local's slot holds the runtime pointer; the ND_ALLOCA emitted by
  // the parser for the declaration stores into it.
  int64_t size = var->ty->kind == TY_VLA ? 8 : var->ty->size;
  if (size < 0)
    error("variable '%s' has incomplete type", var->name ? var->name : "");

  // MIR allocas pack tightly; rounding every slot to 16 bytes keeps slots
  // naturally aligned. Stricter alignment over-allocates and masks.
  int32_t align = obj_align(var);
  MIR_reg_t r = new_tmp(MIR_T_I64);
  if (align <= 16) {
    out(MIR_new_insn(mc, MIR_ALLOCA, rop(r), iop(align_to(MAX(size, 1), 16))));
  } else {
    out(MIR_new_insn(mc, MIR_ALLOCA, rop(r), iop(align_to(size, 16) + align)));
    r = i64_op2(MIR_ADD, r, iop(align - 1));
    r = i64_op2(MIR_AND, r, iop(-(int64_t)align));
  }
  var->ofs = (int)r;
  var->ptr = mir_local_marker;
}

static void alloca_scope(Scope *sc) {
  for (Obj *var = sc->locals; var; var = var->next)
    alloca_obj(var);
  for (Scope *sub = sc->children; sub; sub = sub->sibling_next)
    alloca_scope(sub);
}

static MIR_reg_t local_addr(Obj *var) {
  if (var->ptr != mir_local_marker)
    internal_error();
  return (MIR_reg_t)var->ofs;
}

//
// Loads, stores, conversions
//

static MIR_op_t mem_op(Type *ty, MIR_reg_t base, int64_t disp) {
  return MIR_new_mem_op(mc, mir_type(ty), disp, base, 0, 1);
}

// Load a scalar of type ty from the address in base. Aggregates, arrays
// and functions are represented by their address, which callers pass
// through without calling this. Small-bitint memory may be non-canonical
// (stores write raw 2^64-wrapped results), so every load re-extends.
static MIR_reg_t load_scalar(Type *ty, MIR_reg_t base, int64_t disp) {
  MIR_type_t cls = mir_class(ty);
  MIR_reg_t r = new_tmp(cls);
  MIR_insn_code_t mov = cls == MIR_T_F   ? MIR_FMOV
                        : cls == MIR_T_D ? MIR_DMOV
                        : cls == MIR_T_LD ? MIR_LDMOV
                                          : MIR_MOV;
  out(MIR_new_insn(mc, mov, rop(r), mem_op(ty, base, disp)));
  return bitint_norm(r, ty);
}

static void store_scalar(Type *ty, MIR_reg_t base, int64_t disp, MIR_reg_t val) {
  MIR_type_t cls = mir_class(ty);
  MIR_insn_code_t mov = cls == MIR_T_F   ? MIR_FMOV
                        : cls == MIR_T_D ? MIR_DMOV
                        : cls == MIR_T_LD ? MIR_LDMOV
                                          : MIR_MOV;
  out(MIR_new_insn(mc, mov, mem_op(ty, base, disp), rop(val)));
}

static bool is_aggregate(Type *ty) {
  return ty->kind == TY_STRUCT || ty->kind == TY_UNION || ty->kind == TY_ARRAY ||
         ty->kind == TY_VLA;
}

//
// Bitfields (mirrors codegen.c's gen_bitfield_load/store): aligned fields
// use one load/store of the smallest covering power-of-two unit; fields
// that straddle their unit go byte by byte to avoid over-reading.
//

static MIR_type_t unit_mir_type(int p2bits) {
  switch (p2bits) {
  case 8: return MIR_T_U8;
  case 16: return MIR_T_U16;
  case 32: return MIR_T_U32;
  default: return MIR_T_U64;
  }
}

static MIR_reg_t new_i64(int64_t val) {
  MIR_reg_t r = new_tmp(MIR_T_I64);
  out(MIR_new_insn(mc, MIR_MOV, rop(r), iop(val)));
  return r;
}

static MIR_reg_t i64_op2(MIR_insn_code_t code, MIR_reg_t a, MIR_op_t b) {
  MIR_reg_t r = new_tmp(MIR_T_I64);
  out(MIR_new_insn(mc, code, rop(r), rop(a), b));
  return r;
}

// Sign- or zero-extend the low `width` bits of val.
static MIR_reg_t extract_field(MIR_reg_t val, int width, bool is_unsigned) {
  if (width >= 64)
    return val;
  MIR_reg_t t = i64_op2(MIR_LSH, val, iop(64 - width));
  return i64_op2(is_unsigned ? MIR_URSH : MIR_RSH, t, iop(64 - width));
}

static MIR_reg_t load_bitfield(Member *mem, MIR_reg_t addr) {
  if (mem->ty->kind == TY_BITINT)
    return load_bitint_bitfield(mem, addr);

  int w = mem->bit_width, o = mem->bit_offset;

  if (mem->is_aligned_bitfield) {
    int p2bits = next_pow_of_two(o + w);
    MIR_reg_t r = new_tmp(MIR_T_I64);
    out(MIR_new_insn(mc, MIR_MOV, rop(r),
                     MIR_new_mem_op(mc, unit_mir_type(p2bits), 0, addr, 0, 1)));
    if (o)
      r = i64_op2(MIR_URSH, r, iop(o));
    return extract_field(r, w, mem->ty->is_unsigned);
  }

  // Unaligned: assemble bytes high-to-low, then splice in the partial
  // first byte.
  int footprint = bitfield_footprint(mem);
  MIR_reg_t r = new_i64(0);
  for (int pofs = footprint - 1; pofs >= !!o; pofs--) {
    MIR_reg_t t = new_tmp(MIR_T_I64);
    out(MIR_new_insn(mc, MIR_MOV, rop(t), MIR_new_mem_op(mc, MIR_T_U8, pofs, addr, 0, 1)));
    r = i64_op2(MIR_LSH, r, iop(8));
    r = i64_op2(MIR_OR, r, rop(t));
  }
  if (o) {
    MIR_reg_t b0 = new_tmp(MIR_T_I64);
    out(MIR_new_insn(mc, MIR_MOV, rop(b0), MIR_new_mem_op(mc, MIR_T_U8, 0, addr, 0, 1)));
    b0 = i64_op2(MIR_URSH, b0, iop(o));
    r = i64_op2(MIR_LSH, r, iop(8 - o));
    r = i64_op2(MIR_OR, r, rop(b0));
  }
  return extract_field(r, w, mem->ty->is_unsigned);
}

// Store val's low bit_width bits into the field; returns the value of the
// assignment expression (the field's new contents, extended).
static MIR_reg_t store_bitfield(Member *mem, MIR_reg_t addr, MIR_reg_t val) {
  if (mem->ty->kind == TY_BITINT)
    return store_bitint_bitfield(mem, addr, val);

  int w = mem->bit_width, o = mem->bit_offset;

  if (mem->is_aligned_bitfield) {
    int p2bits = next_pow_of_two(o + w);
    MIR_type_t ut = unit_mir_type(p2bits);

    if (w == p2bits) {
      out(MIR_new_insn(mc, MIR_MOV, MIR_new_mem_op(mc, ut, 0, addr, 0, 1), rop(val)));
    } else {
      uint64_t field_msk = (((uint64_t)1 << w) - 1) << o;
      MIR_reg_t cur = new_tmp(MIR_T_I64);
      out(MIR_new_insn(mc, MIR_MOV, rop(cur), MIR_new_mem_op(mc, ut, 0, addr, 0, 1)));
      cur = i64_op2(MIR_AND, cur, iop(~field_msk));

      MIR_reg_t t = i64_op2(MIR_LSH, val, iop(64 - w));
      t = i64_op2(MIR_URSH, t, iop(64 - w - o));
      cur = i64_op2(MIR_OR, cur, rop(t));
      out(MIR_new_insn(mc, MIR_MOV, MIR_new_mem_op(mc, ut, 0, addr, 0, 1), rop(cur)));
    }
    return extract_field(val, w, mem->ty->is_unsigned);
  }

  // Unaligned: write byte by byte; partial bytes at both ends are merged.
  MIR_reg_t t = val;
  int rem = o + w;
  int pofs = 0;
  if (o) {
    MIR_reg_t b0 = new_tmp(MIR_T_I64);
    out(MIR_new_insn(mc, MIR_MOV, rop(b0), MIR_new_mem_op(mc, MIR_T_U8, 0, addr, 0, 1)));
    b0 = i64_op2(MIR_AND, b0, iop((1 << o) - 1));
    MIR_reg_t lo = i64_op2(MIR_LSH, val, iop(o));
    lo = i64_op2(MIR_AND, lo, iop(0xff));
    b0 = i64_op2(MIR_OR, b0, rop(lo));
    out(MIR_new_insn(mc, MIR_MOV, MIR_new_mem_op(mc, MIR_T_U8, 0, addr, 0, 1), rop(b0)));
    t = i64_op2(MIR_URSH, val, iop(8 - o));
    pofs++;
    rem -= 8;
  }
  for (int start = rem; rem > 0; rem -= 8, pofs++) {
    if (rem != start)
      t = i64_op2(MIR_URSH, t, iop(8));
    if (rem >= 8) {
      out(MIR_new_insn(mc, MIR_MOV, MIR_new_mem_op(mc, MIR_T_U8, pofs, addr, 0, 1), rop(t)));
      continue;
    }
    MIR_reg_t b = new_tmp(MIR_T_I64);
    out(MIR_new_insn(mc, MIR_MOV, rop(b), MIR_new_mem_op(mc, MIR_T_U8, pofs, addr, 0, 1)));
    b = i64_op2(MIR_AND, b, iop(~((1 << rem) - 1)));
    MIR_reg_t m = i64_op2(MIR_AND, t, iop((1 << rem) - 1));
    b = i64_op2(MIR_OR, b, rop(m));
    out(MIR_new_insn(mc, MIR_MOV, MIR_new_mem_op(mc, MIR_T_U8, pofs, addr, 0, 1), rop(b)));
    break;
  }
  return extract_field(val, w, mem->ty->is_unsigned);
}

// Convert an integer-class value to the in-register representation of an
// integer type: low ty-size bits valid, extended to 64 bits per signedness.
static MIR_reg_t int_extend(MIR_reg_t src, Type *ty) {
  MIR_insn_code_t code;
  switch (ty->size) {
  case 1: code = ty->is_unsigned ? MIR_UEXT8 : MIR_EXT8; break;
  case 2: code = ty->is_unsigned ? MIR_UEXT16 : MIR_EXT16; break;
  case 4: code = ty->is_unsigned ? MIR_UEXT32 : MIR_EXT32; break;
  default: return src;
  }
  MIR_reg_t r = new_tmp(MIR_T_I64);
  out(MIR_new_insn(mc, code, rop(r), rop(src)));
  return r;
}

static MIR_reg_t gen_cast2(MIR_reg_t src, Type *from, Type *to, Token *tok) {
  if (to->kind == TY_VOID)
    return src;

  if (is_big_bitint(to)) {
    if (is_big_bitint(from)) {
      // Truncation reads the low chunks in place; widening re-extends a
      // copy in a buffer of the wider size.
      if (from->bit_cnt >= to->bit_cnt)
        return src;
      MIR_reg_t buf = bitint_buf(to->size);
      gen_mem_copy(buf, src, from->size);
      bitint_call(BI_SIGN_EXT, 4, (MIR_op_t[]){iop(from->bit_cnt), rop(buf), iop(to->bit_cnt),
                                               iop(from->is_unsigned)});
      return buf;
    }
    if (is_scalar_fp(from))
      error_tok(tok, "Unimplemented cast to _BitInt");
    MIR_reg_t buf = bitint_buf(to->size);
    out(MIR_new_insn(mc, MIR_MOV, MIR_new_mem_op(mc, MIR_T_I64, 0, buf, 0, 1), rop(src)));
    bitint_call(BI_SIGN_EXT, 4, (MIR_op_t[]){iop(bit_size(from)), rop(buf), iop(to->bit_cnt),
                                             iop(from->is_unsigned)});
    return buf;
  }

  if (is_big_bitint(from)) {
    if (to->kind == TY_BOOL)
      return bitint_to_bool(from, src);
    if (is_scalar_fp(to))
      error_tok(tok, "Unimplemented cast from _BitInt");
    // The low chunk, normalized to the target's representation.
    MIR_reg_t r = load_scalar(ty_llong, src, 0);
    if (to->kind == TY_BITINT)
      return bitint_norm(r, to);
    return (is_integer(to) && to->size < 8) ? int_extend(r, to) : r;
  }

  if (to->kind == TY_BOOL) {
    MIR_reg_t r = new_tmp(MIR_T_I64);
    switch (from->kind) {
    case TY_FLOAT: out(MIR_new_insn(mc, MIR_FNE, rop(r), rop(src), MIR_new_float_op(mc, 0.0f))); break;
    case TY_DOUBLE: out(MIR_new_insn(mc, MIR_DNE, rop(r), rop(src), MIR_new_double_op(mc, 0.0))); break;
    case TY_LDOUBLE: out(MIR_new_insn(mc, MIR_LDNE, rop(r), rop(src), MIR_new_ldouble_op(mc, 0.0L))); break;
    default: out(MIR_new_insn(mc, MIR_NE, rop(r), rop(src), iop(0)));
    }
    return r;
  }

  bool from_fp = is_scalar_fp(from);
  bool to_fp = is_scalar_fp(to);

  if (!from_fp && !to_fp) {
    // Integer/pointer to integer/pointer: normalize to the target width.
    if (to->kind == TY_BITINT) {
      if (to->bit_cnt != to->size * 8)
        return bitint_norm(src, to);
      return to->size < 8 ? int_extend(src, to) : src;
    }
    if (is_integer(to) && to->size < 8)
      return int_extend(src, to);
    if (from->kind == TY_BITINT)
      return src; // already canonical
    if (is_integer(from) && from->size < 8 && from->size < to->size)
      return int_extend(src, from);
    return src;
  }

  if (from_fp && to_fp) {
    static const MIR_insn_code_t conv[3][3] = {
      {MIR_FMOV, MIR_F2D, MIR_F2LD},
      {MIR_D2F, MIR_DMOV, MIR_D2LD},
      {MIR_LD2F, MIR_LD2D, MIR_LDMOV},
    };
    int fi = from->kind - TY_FLOAT, ti = to->kind - TY_FLOAT;
    if (fi == ti)
      return src;
    MIR_reg_t r = new_tmp(mir_class(to));
    out(MIR_new_insn(mc, conv[fi][ti], rop(r), rop(src)));
    return r;
  }

  if (!from_fp) {
    // Integer to floating point.
    MIR_reg_t i64src = src;
    if (is_integer(from) && from->size < 8)
      i64src = int_extend(src, from);
    bool uns = from->is_unsigned && from->size == 8;
    MIR_insn_code_t code;
    switch (to->kind) {
    case TY_FLOAT: code = uns ? MIR_UI2F : MIR_I2F; break;
    case TY_DOUBLE: code = uns ? MIR_UI2D : MIR_I2D; break;
    default: code = uns ? MIR_UI2LD : MIR_I2LD; break;
    }
    MIR_reg_t r = new_tmp(mir_class(to));
    out(MIR_new_insn(mc, code, rop(r), rop(i64src)));
    return r;
  }

  // Floating point to integer.
  MIR_insn_code_t code;
  switch (from->kind) {
  case TY_FLOAT: code = MIR_F2I; break;
  case TY_DOUBLE: code = MIR_D2I; break;
  default: code = MIR_LD2I; break;
  }
  MIR_reg_t r = new_tmp(MIR_T_I64);
  out(MIR_new_insn(mc, code, rop(r), rop(src)));
  if (to->kind == TY_BITINT)
    return bitint_norm(to->size < 8 ? int_extend(r, to) : r, to);
  if (is_integer(to) && to->size < 8)
    return int_extend(r, to);
  return r;
}

//
// Defer statements (`defer`, __attribute__((cleanup)), VLA dealloc)
//

static bool has_defr(Node *node) {
  return node->dfr_from != node->dfr_dest;
}

// Assign label ids to a block's resolved label list (mirrors codegen.c's
// name_labels); ids 0 means "unreferenced label, no code needed".
static void name_labels(Node *n) {
  for (; n; n = n->lbl.next)
    n->lbl.id = ++ctrl_cnt;
}

static void gen_defr(Node *node) {
  DeferStmt *defr = node->dfr_from;
  DeferStmt *end = node->dfr_dest;

  while (defr != end) {
    switch (defr->kind) {
    case DF_VLA_DEALLOC: {
      // Release VLA stack at scope exit (mirrors codegen.c): restore the
      // stack position to the enclosing VLA's pointer if one exists in
      // the surviving defer chain, else to the function entry position.
      // A VLA's slot holds its allocation pointer, which equals the stack
      // position right after its alloca.
      while (defr->next != end && defr->next->kind == DF_VLA_DEALLOC)
        defr = defr->next;

      if (!cur_fn->dealloc_vla) {
        defr = defr->next;
        continue;
      }

      Obj *vla = defr->next ? defr->next->vla : NULL;
      MIR_reg_t pos = load_scalar(ty_long, vla ? local_addr(vla) : vla_base_slot, 0);
      out(MIR_new_insn(mc, MIR_BEND, rop(pos)));
      defr = defr->next;
      continue;
    }
    case DF_CLEANUP_FN:
      gen_void_expr(defr->cleanup_fn);
      defr = defr->next;
      continue;
    case DF_DEFER_STMT:
      gen_stmt(defr->stmt);
      defr = defr->next;
      continue;
    }
    internal_error();
  }
}

//
// Expressions
//

// Comparison insn for operands of the given type: idx 0..5 = EQ NE LT LE GT GE.
static MIR_insn_code_t cmp_code(Type *ty, int idx) {
  static const MIR_insn_code_t f[6] = {MIR_FEQ, MIR_FNE, MIR_FLT, MIR_FLE, MIR_FGT, MIR_FGE};
  static const MIR_insn_code_t d[6] = {MIR_DEQ, MIR_DNE, MIR_DLT, MIR_DLE, MIR_DGT, MIR_DGE};
  static const MIR_insn_code_t ld[6] = {MIR_LDEQ, MIR_LDNE, MIR_LDLT, MIR_LDLE, MIR_LDGT, MIR_LDGE};
  static const MIR_insn_code_t s[6] = {MIR_EQS, MIR_NES, MIR_LTS, MIR_LES, MIR_GTS, MIR_GES};
  static const MIR_insn_code_t us[6] = {MIR_EQS, MIR_NES, MIR_ULTS, MIR_ULES, MIR_UGTS, MIR_UGES};
  static const MIR_insn_code_t i[6] = {MIR_EQ, MIR_NE, MIR_LT, MIR_LE, MIR_GT, MIR_GE};
  static const MIR_insn_code_t u[6] = {MIR_EQ, MIR_NE, MIR_ULT, MIR_ULE, MIR_UGT, MIR_UGE};

  switch (ty->kind) {
  case TY_FLOAT: return f[idx];
  case TY_DOUBLE: return d[idx];
  case TY_LDOUBLE: return ld[idx];
  default:
    if (is_integer(ty) && ty->size <= 4)
      return ty->is_unsigned ? us[idx] : s[idx];
    return (!is_integer(ty) || ty->is_unsigned) ? u[idx] : i[idx];
  }
}

// Arithmetic insn for values of the given type. Operations on types
// narrower than int do not occur: the usual arithmetic conversions are
// explicit casts in the AST.
static MIR_insn_code_t arith_code(NodeKind kind, Type *ty, Token *tok) {
  bool s4 = ty->size <= 4;
  switch (ty->kind) {
  case TY_FLOAT:
    switch (kind) {
    case ND_ADD: return MIR_FADD;
    case ND_SUB: return MIR_FSUB;
    case ND_MUL: return MIR_FMUL;
    case ND_DIV: return MIR_FDIV;
    }
    break;
  case TY_DOUBLE:
    switch (kind) {
    case ND_ADD: return MIR_DADD;
    case ND_SUB: return MIR_DSUB;
    case ND_MUL: return MIR_DMUL;
    case ND_DIV: return MIR_DDIV;
    }
    break;
  case TY_LDOUBLE:
    switch (kind) {
    case ND_ADD: return MIR_LDADD;
    case ND_SUB: return MIR_LDSUB;
    case ND_MUL: return MIR_LDMUL;
    case ND_DIV: return MIR_LDDIV;
    }
    break;
  default:
    switch (kind) {
    case ND_ADD: return s4 ? MIR_ADDS : MIR_ADD;
    case ND_SUB: return s4 ? MIR_SUBS : MIR_SUB;
    case ND_MUL: return s4 ? MIR_MULS : MIR_MUL;
    case ND_DIV:
      if (ty->is_unsigned)
        return s4 ? MIR_UDIVS : MIR_UDIV;
      return s4 ? MIR_DIVS : MIR_DIV;
    case ND_MOD:
      if (ty->is_unsigned)
        return s4 ? MIR_UMODS : MIR_UMOD;
      return s4 ? MIR_MODS : MIR_MOD;
    case ND_BITAND: return s4 ? MIR_ANDS : MIR_AND;
    case ND_BITOR: return s4 ? MIR_ORS : MIR_OR;
    case ND_BITXOR: return s4 ? MIR_XORS : MIR_XOR;
    case ND_SHL: return s4 ? MIR_LSHS : MIR_LSH;
    case ND_SHR: return s4 ? MIR_URSHS : MIR_URSH;
    case ND_SAR: return s4 ? MIR_RSHS : MIR_RSH;
    }
  }
  error_tok(tok, "unsupported arithmetic for this type in the MIR backend");
}

static MIR_reg_t gen_binop(NodeKind kind, Type *ty, MIR_reg_t lhs, MIR_reg_t rhs, Token *tok) {
  MIR_reg_t r = new_tmp(mir_class(ty));
  out(MIR_new_insn(mc, arith_code(kind, ty, tok), rop(r), rop(lhs), rop(rhs)));
  return r;
}

static MIR_item_t get_memset(void) {
  if (!memset_import) {
    memset_import = MIR_new_import(mc, "memset");
    MIR_var_t vars[3] = {
      {MIR_T_P, "s", 0}, {MIR_T_I64, "c", 0}, {MIR_T_I64, "n", 0}};
    MIR_type_t res = MIR_T_P;
    memset_proto = MIR_new_proto_arr(mc, "memset.p", 1, &res, 3, vars);
  }
  return memset_import;
}

static MIR_item_t get_memcpy(void) {
  if (!memcpy_import) {
    memcpy_import = MIR_new_import(mc, "memcpy");
    MIR_var_t vars[3] = {
      {MIR_T_P, "d", 0}, {MIR_T_P, "s", 0}, {MIR_T_I64, "n", 0}};
    MIR_type_t res = MIR_T_P;
    memcpy_proto = MIR_new_proto_arr(mc, "memcpy.p", 1, &res, 3, vars);
  }
  return memcpy_import;
}

static MIR_item_t get_emutls(MIR_item_t *proto) {
  if (!emutls_import) {
    emutls_import = MIR_new_import(mc, "__slimcc_emutls_get_address");
    MIR_var_t var = {MIR_T_P, "c", 0};
    MIR_type_t res = MIR_T_P;
    emutls_proto = MIR_new_proto_arr(mc, "__slimcc_emutls_get_address.p", 1, &res, 1, &var);
  }
  *proto = emutls_proto;
  return emutls_import;
}

// The control object a thread-local variable is accessed through (see
// emit_data_obj); the variable's own name never becomes a MIR symbol.
static MIR_item_t emutls_ctrl_item(Obj *var) {
  SymItem *si = sym_entry(arena_format(&cc1_arena, "__emutls_v.%s", sym_name(var)));
  if (!si->item)
    si->item = MIR_new_forward(mc, si->key);
  return si->item;
}

// Index into atomic_imports/protos: 0-3 cas by log2 size, 4-7 exch, 8 fence.
static MIR_item_t get_atomic(int idx, MIR_item_t *proto) {
  if (!atomic_imports[idx]) {
    static const char *const names[9] = {
      "__slimcc_jit_cas_1", "__slimcc_jit_cas_2", "__slimcc_jit_cas_4", "__slimcc_jit_cas_8",
      "__slimcc_jit_exch_1", "__slimcc_jit_exch_2", "__slimcc_jit_exch_4", "__slimcc_jit_exch_8",
      "__slimcc_jit_fence",
    };
    atomic_imports[idx] = MIR_new_import(mc, names[idx]);
    char pname[40];
    snprintf(pname, sizeof(pname), "%s.p", names[idx]);
    MIR_type_t res = MIR_T_I8;
    if (idx < 4) {
      MIR_var_t vars[3] = {{MIR_T_P, "p", 0}, {MIR_T_P, "e", 0}, {MIR_T_I64, "d", 0}};
      atomic_protos[idx] = MIR_new_proto_arr(mc, pname, 1, &res, 3, vars);
    } else if (idx < 8) {
      MIR_var_t vars[2] = {{MIR_T_P, "p", 0}, {MIR_T_I64, "v", 0}};
      res = MIR_T_I64;
      atomic_protos[idx] = MIR_new_proto_arr(mc, pname, 1, &res, 2, vars);
    } else {
      atomic_protos[idx] = MIR_new_proto_arr(mc, pname, 0, NULL, 0, NULL);
    }
  }
  *proto = atomic_protos[idx];
  return atomic_imports[idx];
}

static int atomic_size_idx(Type *ty, Token *tok) {
  switch (ty->size) {
  case 1: return 0;
  case 2: return 1;
  case 4: return 2;
  case 8: return 3;
  }
  error_tok(tok, "unsupported type size for atomic operation in the MIR backend");
}

// One 8-byte stack slot per function for FP<->int bit transfers, created
// on demand by prepending the alloca to the function entry.
static MIR_reg_t get_scratch_slot(void) {
  if (!scratch_slot) {
    scratch_slot = new_tmp(MIR_T_I64);
    MIR_prepend_insn(mc, fn_item,
                     MIR_new_insn(mc, MIR_ALLOCA, rop(scratch_slot), iop(16)));
  }
  return scratch_slot;
}

// Move a value into an integer register carrying the bit pattern, via the
// scratch slot for floating-point classes.
static MIR_reg_t bits_of(Type *ty, MIR_reg_t val) {
  if (!is_scalar_fp(ty))
    return val;
  MIR_reg_t slot = get_scratch_slot();
  store_scalar(ty, slot, 0, val);
  MIR_reg_t r = new_tmp(MIR_T_I64);
  MIR_type_t ity = ty->size == 4 ? MIR_T_U32 : MIR_T_U64;
  out(MIR_new_insn(mc, MIR_MOV, rop(r), MIR_new_mem_op(mc, ity, 0, slot, 0, 1)));
  return r;
}

static MIR_reg_t value_of_bits(Type *ty, MIR_reg_t bits) {
  if (!is_scalar_fp(ty))
    return bits;
  MIR_reg_t slot = get_scratch_slot();
  MIR_type_t ity = ty->size == 4 ? MIR_T_U32 : MIR_T_U64;
  out(MIR_new_insn(mc, MIR_MOV, MIR_new_mem_op(mc, ity, 0, slot, 0, 1), rop(bits)));
  return load_scalar(ty, slot, 0);
}

static void gen_mem_zero(MIR_reg_t addr, int64_t size) {
  MIR_item_t ms = get_memset();
  MIR_reg_t res = new_tmp(MIR_T_I64);
  out(MIR_new_insn_arr(mc, MIR_CALL, 6,
                       (MIR_op_t[]){MIR_new_ref_op(mc, memset_proto), MIR_new_ref_op(mc, ms),
                                    rop(res), rop(addr), iop(0), iop(size)}));
}

static void gen_mem_copy(MIR_reg_t dst, MIR_reg_t src, int64_t size) {
  MIR_item_t mcp = get_memcpy();
  MIR_reg_t res = new_tmp(MIR_T_I64);
  out(MIR_new_insn_arr(mc, MIR_CALL, 6,
                       (MIR_op_t[]){MIR_new_ref_op(mc, memcpy_proto), MIR_new_ref_op(mc, mcp),
                                    rop(res), rop(dst), rop(src), iop(size)}));
}

//
// _BitInt wider than 64 bits ("big"): the value is the address of a
// little-endian 64-bit-chunk buffer, and all operations go through the
// __slimcc_bitint_* helpers, which are host-compiled into the library
// (slimcc-mir-helpers.c) and reached through imports that
// slimcc_register_helpers() resolves - modules don't carry their own
// compiled copies the way the CLI compiler's injected bitint_builtins
// header works. Helper convention: lh/src operands are
// read-only and results land in rh/dst, except that div clobbers both
// and to_bool canonicalizes its operand in place — operands that alias
// program memory are copied into fresh buffers accordingly.
//
// _BitInt up to 64 bits stays in registers like other integers, kept
// canonical: sign/zero-extended from bit_cnt, maintained by bitint_norm
// at every value-producing site.
//

static bool is_big_bitint(Type *ty) {
  return ty->kind == TY_BITINT && ty->bit_cnt > 64;
}

// Values of these types are represented by their address.
static bool is_addr_value(Type *ty) {
  return is_aggregate(ty) || is_big_bitint(ty);
}

// A fresh buffer as a function-entry alloca: executes once even inside
// loops, and lands before the BSTART that VLA deallocation restores to.
static MIR_reg_t bitint_buf(int64_t size) {
  MIR_reg_t r = new_tmp(MIR_T_I64);
  MIR_prepend_insn(mc, fn_item,
                   MIR_new_insn(mc, MIR_ALLOCA, rop(r), iop(align_to(MAX(size, 8), 16))));
  return r;
}

static MIR_reg_t bitint_copy(Type *ty, MIR_reg_t src) {
  MIR_reg_t buf = bitint_buf(ty->size);
  gen_mem_copy(buf, src, ty->size);
  return buf;
}

// Re-extend a small-bitint register value from bit_cnt (no-op for other
// types and for widths the size-based extension already covers).
static MIR_reg_t bitint_norm(MIR_reg_t r, Type *ty) {
  if (ty->kind != TY_BITINT || ty->bit_cnt > 64 || ty->bit_cnt == ty->size * 8)
    return r;
  MIR_reg_t t = i64_op2(MIR_LSH, r, iop(64 - ty->bit_cnt));
  return i64_op2(ty->is_unsigned ? MIR_URSH : MIR_RSH, t, iop(64 - ty->bit_cnt));
}

// Signatures match the helper definitions: 'i' int, 'p' pointer, 'b' bool.
static const struct {
  const char *name;
  char res; // 0 for void
  const char *args;
} bitint_fns[BI_CNT] = {
  [BI_NEG] = {"__slimcc_bitint_neg", 0, "ip"},
  [BI_BITNOT] = {"__slimcc_bitint_bitnot", 0, "ip"},
  [BI_BITAND] = {"__slimcc_bitint_bitand", 0, "ipp"},
  [BI_BITOR] = {"__slimcc_bitint_bitor", 0, "ipp"},
  [BI_BITXOR] = {"__slimcc_bitint_bitxor", 0, "ipp"},
  [BI_ADD] = {"__slimcc_bitint_add", 0, "ipp"},
  [BI_SUB] = {"__slimcc_bitint_sub", 0, "ipp"},
  [BI_MUL] = {"__slimcc_bitint_mul", 0, "ipp"},
  [BI_DIV] = {"__slimcc_bitint_div", 0, "ippbb"},
  [BI_SHL] = {"__slimcc_bitint_shl", 0, "ippi"},
  [BI_SHR] = {"__slimcc_bitint_shr", 0, "ippib"},
  [BI_CMP] = {"__slimcc_bitint_cmp", 'i', "ippb"},
  [BI_TO_BOOL] = {"__slimcc_bitint_to_bool", 'b', "ip"},
  [BI_SIGN_EXT] = {"__slimcc_bitint_sign_ext", 0, "ipib"},
  [BI_OVERFLOW] = {"__slimcc_bitint_overflow", 'b', "ipib"},
  [BI_BF_LOAD] = {"__slimcc_bitint_bitfield_load", 'p', "ippiib"},
  [BI_BF_SAVE] = {"__slimcc_bitint_bitfield_save", 0, "ippii"},
};

static MIR_item_t bitint_items[BI_CNT], bitint_protos[BI_CNT];

static MIR_type_t bitint_arg_type(char c) {
  switch (c) {
  case 'i': return MIR_T_I32;
  case 'b': return MIR_T_U8;
  default: return MIR_T_I64;
  }
}

static MIR_item_t get_bitint_fn(BitintHelper h, MIR_item_t *proto) {
  if (!bitint_items[h]) {
    const char *name = bitint_fns[h].name;
    SymItem *si = sym_entry(name);
    if (!si->item)
      si->item = MIR_new_forward(mc, si->key);
    bitint_items[h] = si->item;

    static const char *const argnames[6] = {"a0", "a1", "a2", "a3", "a4", "a5"};
    MIR_var_t vars[6];
    int n = strlen(bitint_fns[h].args);
    for (int i = 0; i < n; i++) {
      vars[i].type = bitint_arg_type(bitint_fns[h].args[i]);
      vars[i].name = argnames[i];
      vars[i].size = 0;
    }
    size_t nres = bitint_fns[h].res != 0;
    MIR_type_t res_ty = nres ? bitint_arg_type(bitint_fns[h].res) : MIR_T_UNDEF;
    char pname[48];
    snprintf(pname, sizeof(pname), "%s.p", name);
    bitint_protos[h] = MIR_new_proto_arr(mc, pname, nres, &res_ty, n, vars);
  }
  *proto = bitint_protos[h];
  return bitint_items[h];
}

// Returns the result register (0 for void helpers).
static MIR_reg_t bitint_call(BitintHelper h, int nargs, MIR_op_t *args) {
  MIR_item_t proto;
  MIR_item_t fn = get_bitint_fn(h, &proto);
  MIR_op_t ops[9];
  ops[0] = MIR_new_ref_op(mc, proto);
  ops[1] = MIR_new_ref_op(mc, fn);
  int i = 2;
  MIR_reg_t res = 0;
  if (bitint_fns[h].res) {
    res = new_tmp(MIR_T_I64);
    ops[i++] = rop(res);
  }
  for (int a = 0; a < nargs; a++)
    ops[i++] = args[a];
  out(MIR_new_insn_arr(mc, MIR_CALL, i, ops));
  return res;
}

// Truth value of a big-bitint value (copied first: to_bool canonicalizes
// its operand in place).
static MIR_reg_t bitint_to_bool(Type *ty, MIR_reg_t addr) {
  MIR_reg_t buf = bitint_copy(ty, addr);
  return bitint_call(BI_TO_BOOL, 2, (MIR_op_t[]){iop(ty->bit_cnt), rop(buf)});
}

// Binary arithmetic on big-bitint operands. Both sides are copied: the
// copy of lhs shields it from side effects of evaluating rhs (and div
// clobbers it); rhs is the buffer the result lands in.
static MIR_reg_t gen_bitint_arith(Node *node) {
  Type *ty = node->ty;
  MIR_op_t bits = iop(ty->bit_cnt);
  MIR_reg_t lhs = bitint_copy(ty, gen_expr(node->m.lhs));

  switch (node->kind) {
  case ND_SHL:
  case ND_SHR:
  case ND_SAR: {
    MIR_reg_t amount = gen_expr(node->m.rhs);
    MIR_reg_t dst = bitint_buf(ty->size);
    if (node->kind == ND_SHL)
      bitint_call(BI_SHL, 4, (MIR_op_t[]){bits, rop(lhs), rop(dst), rop(amount)});
    else
      bitint_call(BI_SHR, 5,
                  (MIR_op_t[]){bits, rop(lhs), rop(dst), rop(amount), iop(ty->is_unsigned)});
    return dst;
  }
  }

  MIR_reg_t rhs = bitint_copy(ty, gen_expr(node->m.rhs));
  switch (node->kind) {
  case ND_BITAND: bitint_call(BI_BITAND, 3, (MIR_op_t[]){bits, rop(lhs), rop(rhs)}); break;
  case ND_BITOR: bitint_call(BI_BITOR, 3, (MIR_op_t[]){bits, rop(lhs), rop(rhs)}); break;
  case ND_BITXOR: bitint_call(BI_BITXOR, 3, (MIR_op_t[]){bits, rop(lhs), rop(rhs)}); break;
  case ND_ADD: bitint_call(BI_ADD, 3, (MIR_op_t[]){bits, rop(lhs), rop(rhs)}); break;
  case ND_SUB: bitint_call(BI_SUB, 3, (MIR_op_t[]){bits, rop(lhs), rop(rhs)}); break;
  case ND_MUL: bitint_call(BI_MUL, 3, (MIR_op_t[]){bits, rop(lhs), rop(rhs)}); break;
  case ND_DIV:
  case ND_MOD:
    bitint_call(BI_DIV, 5, (MIR_op_t[]){bits, rop(lhs), rop(rhs), iop(ty->is_unsigned),
                                        iop(node->kind == ND_DIV)});
    break;
  default:
    internal_error();
  }
  return rhs;
}

// Comparison of big-bitint operands via __slimcc_bitint_cmp, which
// returns 0 (equal), 1 (lh < rh) or 2 (lh > rh) and reads both buffers
// without mutating; lhs is still copied against rhs side effects.
static MIR_reg_t gen_bitint_cmp(Node *node) {
  Type *ty = node->m.lhs->ty;
  MIR_reg_t lhs = bitint_copy(ty, gen_expr(node->m.lhs));
  MIR_reg_t rhs = gen_expr(node->m.rhs);
  MIR_reg_t c = bitint_call(
      BI_CMP, 4, (MIR_op_t[]){iop(ty->bit_cnt), rop(lhs), rop(rhs), iop(ty->is_unsigned)});

  static const int cmp_val[6] = {0, 0, 1, 2, 2, 1};   // EQ NE LT LE GT GE
  static const bool cmp_eq[6] = {true, false, true, false, true, false};
  int idx = node->kind - ND_EQ;
  MIR_reg_t r = new_tmp(MIR_T_I64);
  out(MIR_new_insn(mc, cmp_eq[idx] ? MIR_EQ : MIR_NE, rop(r), rop(c), iop(cmp_val[idx])));
  return r;
}

static int32_t ovf_headroom(Type *ty, NodeKind kind) {
  int32_t bits = (ty->kind == TY_BOOL) ? 1 : bit_size(ty);

  if (kind == ND_MUL)
    return bits * 2 + ty->is_unsigned;
  return bits + 1 + ty->is_unsigned;
}

// Evaluate a checked-arithmetic operand cast to _BitInt(bits) into a
// fresh buffer (bits is a multiple of 64; the 64-bit case is a register
// value spilled into the buffer).
static MIR_reg_t gen_ckd_operand(Node *operand, Type *bty) {
  Node expr = *operand;
  MIR_reg_t v = gen_expr(new_cast(&expr, bty));
  if (bty->bit_cnt > 64)
    return bitint_copy(bty, v);
  MIR_reg_t buf = bitint_buf(8);
  out(MIR_new_insn(mc, MIR_MOV, MIR_new_mem_op(mc, MIR_T_I64, 0, buf, 0, 1), rop(v)));
  return buf;
}

// _BitInt bitfields of any width go through the bitfield helpers, which
// work on whole-chunk buffers (mirrors codegen.c's gen_bitfield_load).
static MIR_reg_t load_bitint_bitfield(Member *mem, MIR_reg_t addr) {
  Type *ty = mem->ty;
  MIR_reg_t buf = bitint_buf(ty->size);
  bitint_call(BI_BF_LOAD, 6,
              (MIR_op_t[]){iop(ty->bit_cnt), rop(addr), rop(buf), iop(mem->bit_width),
                           iop(mem->bit_offset), iop(ty->is_unsigned)});
  if (ty->bit_cnt > 64)
    return buf;
  return load_scalar(ty, buf, 0);
}

static MIR_reg_t store_bitint_bitfield(Member *mem, MIR_reg_t addr, MIR_reg_t val) {
  Type *ty = mem->ty;

  // The helper reads the value buffer without mutating, so a big value's
  // address is passed as-is; register values are spilled to a buffer.
  MIR_reg_t vbuf;
  if (ty->bit_cnt > 64) {
    vbuf = val;
  } else {
    vbuf = bitint_buf(8);
    out(MIR_new_insn(mc, MIR_MOV, MIR_new_mem_op(mc, MIR_T_I64, 0, vbuf, 0, 1), rop(val)));
  }
  bitint_call(BI_BF_SAVE, 5, (MIR_op_t[]){iop(ty->bit_cnt), rop(vbuf), rop(addr),
                                          iop(mem->bit_width), iop(mem->bit_offset)});

  // The assignment expression's value: the stored field re-extended from
  // its width to the full type.
  if (ty->bit_cnt > 64) {
    MIR_reg_t res = bitint_copy(ty, vbuf);
    bitint_call(BI_SIGN_EXT, 4, (MIR_op_t[]){iop(mem->bit_width), rop(res), iop(ty->bit_cnt),
                                             iop(ty->is_unsigned)});
    return res;
  }
  return extract_field(val, mem->bit_width, ty->is_unsigned);
}

static MIR_reg_t gen_funcall(Node *node) {
  Node *fn_expr = node->call.expr;
  Type *fn_ty = fn_expr->ty;
  if (fn_ty->kind == TY_PTR)
    fn_ty = fn_ty->base;
  if (fn_ty->kind != TY_FUNC)
    internal_error();

  Type *rt = node->ty;

  // Struct and big-bitint returns go through the parser-allocated buffer,
  // passed as a hidden first RBLK argument.
  bool rtn_by_addr = rt->kind == TY_STRUCT || rt->kind == TY_UNION || is_big_bitint(rt);
  if (rtn_by_addr && !node->call.rtn_buf)
    internal_error();

  // Callee: a direct reference or a function pointer value.
  MIR_op_t fn_op;
  if (fn_expr->kind == ND_VAR && fn_expr->m.var->ty->kind == TY_FUNC)
    fn_op = MIR_new_ref_op(mc, sym_item(fn_expr->m.var));
  else
    fn_op = rop(gen_expr(fn_expr));

  int nparams = 0;
  for (Obj *arg = node->call.args; arg; arg = arg->param_next)
    nparams++;
  int nargs = nparams + rtn_by_addr;

  MIR_var_t *vars = arena_malloc(&cc1_arena, MAX(nargs, 1) * sizeof(MIR_var_t));
  MIR_op_t *argops = arena_malloc(&cc1_arena, MAX(nargs, 1) * sizeof(MIR_op_t));

  int i = 0;
  if (rtn_by_addr) {
    vars[0].type = MIR_T_RBLK;
    vars[0].name = "Ret.Addr";
    vars[0].size = rt->size;
    argops[0] = MIR_new_mem_op(mc, MIR_T_RBLK, rt->size, local_addr(node->call.rtn_buf), 0, 1);
    i++;
  }

  // Evaluate arguments left to right.
  for (Obj *arg = node->call.args; arg; arg = arg->param_next, i++) {
    Type *ty = arg->ty;
    vars[i].name = arena_format(&cc1_arena, "a%d", i);
    if (ty->kind == TY_STRUCT || ty->kind == TY_UNION || is_big_bitint(ty)) {
      // Aggregate values are their address; MIR copies the block per the
      // target convention. The mem op's disp field carries the size.
      vars[i].type = MIR_T_BLK;
      vars[i].size = ty->size;
      argops[i] = MIR_new_mem_op(mc, MIR_T_BLK, ty->size, gen_expr(arg->arg_expr), 0, 1);
    } else {
      vars[i].type = mir_type(ty);
      vars[i].size = 0;
      argops[i] = rop(gen_expr(arg->arg_expr));
    }
  }

  // Build a per-call prototype carrying the actual argument types. The
  // dot keeps the name out of the C identifier namespace.
  char pname[32];
  snprintf(pname, sizeof(pname), "proto.%" PRIi64, proto_cnt++);
  size_t nres = (rt->kind == TY_VOID || rtn_by_addr) ? 0 : 1;
  MIR_type_t res_ty = nres ? mir_type(rt) : MIR_T_UNDEF;
  MIR_item_t proto = fn_ty->is_variadic
                         ? MIR_new_vararg_proto_arr(mc, pname, nres, &res_ty, nargs, vars)
                         : MIR_new_proto_arr(mc, pname, nres, &res_ty, nargs, vars);

  MIR_reg_t res = 0;
  size_t nops = 2 + nres + nargs;
  MIR_op_t *ops = arena_malloc(&cc1_arena, nops * sizeof(MIR_op_t));
  ops[0] = MIR_new_ref_op(mc, proto);
  ops[1] = fn_op;
  if (nres) {
    res = new_tmp(mir_class(rt));
    ops[2] = rop(res);
  }
  for (i = 0; i < nargs; i++)
    ops[2 + nres + i] = argops[i];

  out(MIR_new_insn_arr(mc, MIR_CALL, nops, ops));

  if (rtn_by_addr)
    return local_addr(node->call.rtn_buf);
  // Small integer returns: normalize to the extended representation.
  if (nres && rt->kind == TY_BITINT)
    return bitint_norm(rt->size < 8 ? int_extend(res, rt) : res, rt);
  if (nres && is_integer(rt) && rt->size < 8)
    return int_extend(res, rt);
  return res;
}

static MIR_reg_t gen_addr(Node *node) {
  switch (node->kind) {
  case ND_VAR: {
    Obj *var = node->m.var;
    if (var->is_local && !var->is_static_lvar) {
      // A VLA variable's slot holds the pointer to its allocation.
      if (var->ty->kind == TY_VLA)
        return load_scalar(ty_long, local_addr(var), 0);
      return local_addr(var);
    }
    // Thread-local: resolve the per-thread copy through the emutls runtime
    // (native TLS needs dynamic-linker cooperation that JIT-loaded modules
    // cannot get). The runtime honors the control object's alignment, so
    // the over-aligned rounding below doesn't apply.
    if (var->is_tls) {
      MIR_item_t proto;
      MIR_item_t fn = get_emutls(&proto);
      MIR_reg_t ctrl = new_tmp(MIR_T_I64);
      out(MIR_new_insn(mc, MIR_MOV, rop(ctrl), MIR_new_ref_op(mc, emutls_ctrl_item(var))));
      MIR_reg_t res = new_tmp(MIR_T_I64);
      out(MIR_new_insn_arr(mc, MIR_CALL, 4,
                           (MIR_op_t[]){MIR_new_ref_op(mc, proto), MIR_new_ref_op(mc, fn),
                                        rop(res), rop(ctrl)}));
      return res;
    }
    MIR_reg_t r = new_tmp(MIR_T_I64);
    out(MIR_new_insn(mc, MIR_MOV, rop(r), MIR_new_ref_op(mc, sym_item(var))));
    // Over-aligned objects live at the rounded-up address inside their
    // over-allocated bss (see emit_data_obj); every code reference rounds
    // the same way, so the convention holds across modules, and a host
    // symbol that is already aligned masks to itself.
    int32_t align = obj_align(var);
    if (align > 16 && var->ty->kind != TY_FUNC) {
      r = i64_op2(MIR_ADD, r, iop(align - 1));
      r = i64_op2(MIR_AND, r, iop(-(int64_t)align));
    }
    return r;
  }
  case ND_DEREF:
    return gen_expr(node->m.lhs);
  case ND_MEMBER: {
    MIR_reg_t base = gen_addr(node->m.lhs);
    if (!node->m.member->offset)
      return base;
    MIR_reg_t r = new_tmp(MIR_T_I64);
    out(MIR_new_insn(mc, MIR_ADD, rop(r), rop(base), iop(node->m.member->offset)));
    return r;
  }
  case ND_CHAIN:
  case ND_COMMA:
    gen_void_expr(node->m.lhs);
    return gen_addr(node->m.rhs);
  case ND_ASSIGN:
  case ND_COND:
  case ND_STMT_EXPR:
  case ND_FUNCALL:
    if (is_addr_value(node->ty) || node->kind == ND_STMT_EXPR)
      return gen_expr(node);
    break;
  case ND_CAST:
    // e.g. (*(T *)&x).m
    return gen_expr(node);
  }
  error_tok(node->tok, "not an lvalue");
}

// Load the value at the address for nodes whose type is scalar; aggregate
// types are represented by their address.
static MIR_reg_t load_node(Node *node, MIR_reg_t addr) {
  Type *ty = node->ty;
  if (node->kind == ND_MEMBER && node->m.member->is_bitfield)
    return load_bitfield(node->m.member, addr);
  if (is_addr_value(ty) || ty->kind == TY_FUNC)
    return addr;
  return load_scalar(ty, addr, 0);
}

// Store val through the lvalue; returns the value of the assignment
// expression (which for bitfields is the truncated, re-extended field).
static MIR_reg_t gen_store(Node *lhs, MIR_reg_t addr, MIR_reg_t val) {
  Type *ty = lhs->ty;
  if (lhs->kind == ND_MEMBER && lhs->m.member->is_bitfield)
    return store_bitfield(lhs->m.member, addr, val);
  if (is_addr_value(ty)) {
    gen_mem_copy(addr, val, ty->size);
    return val;
  }
  store_scalar(ty, addr, 0, val);
  return val;
}

// Value plugged in for ND_NULL_EXPR while generating compound assignments
// (the already-loaded left-hand side).
static MIR_reg_t null_expr_reg;

static MIR_reg_t gen_arith_assign(Node *node, bool want_old) {
  // ND_ARITH_ASSIGN / ND_POST_INCDEC: load from lhs address, combine with
  // rhs per arith_kind in the usual-conversion type, convert back, store.
  // Mirrors codegen.c's gen_expr_null_lhs: a synthetic expression with an
  // ND_NULL_EXPR lhs lets add_type insert the conversion casts.
  Node *lhs = node->m.lhs;
  MIR_reg_t addr = gen_addr(lhs);
  MIR_reg_t old = load_node(lhs, addr);

  // A plain big-bitint lvalue's "value" is its storage address; the
  // postfix result must be a snapshot taken before the store.
  if (want_old && is_big_bitint(lhs->ty) &&
      !(lhs->kind == ND_MEMBER && lhs->m.member->is_bitfield))
    old = bitint_copy(lhs->ty, old);

  NodeKind kind = node->kind == ND_POST_INCDEC ? ND_ADD : node->arith_kind;
  Node null = {.kind = ND_NULL_EXPR, .ty = lhs->ty, .tok = node->tok};
  Node expr = {.kind = kind, .m.lhs = &null, .m.rhs = node->m.rhs, .tok = node->tok};
  add_type(&expr);

  MIR_reg_t saved = null_expr_reg;
  null_expr_reg = old;
  MIR_reg_t res = gen_expr(new_cast(&expr, lhs->ty));
  null_expr_reg = saved;

  res = gen_store(lhs, addr, res);
  return want_old ? old : res;
}

static MIR_reg_t gen_expr(Node *node) {
  switch (node->kind) {
  case ND_NULL_EXPR:
    return null_expr_reg;
  case ND_UNREACHABLE:
    return 0;
  case ND_NUM: {
    Type *ty = node->ty;
    // _BitInt literal values of any width live in num.bitint_data.
    if (ty->kind == TY_BITINT) {
      if (ty->bit_cnt > 64) {
        MIR_reg_t buf = bitint_buf(ty->size);
        for (int64_t i = 0; i < ty->size / 8; i++)
          out(MIR_new_insn(mc, MIR_MOV, MIR_new_mem_op(mc, MIR_T_I64, i * 8, buf, 0, 1),
                           iop((&node->num.bitint_data->as64)[i])));
        return buf;
      }
      int64_t val = node->num.bitint_data->as64;
      if (ty->bit_cnt < 64) { // canonicalize the immediate
        val <<= 64 - ty->bit_cnt;
        val = ty->is_unsigned ? (int64_t)((uint64_t)val >> (64 - ty->bit_cnt))
                              : val >> (64 - ty->bit_cnt);
      }
      return new_i64(val);
    }
    MIR_reg_t r = new_tmp(mir_class(ty));
    switch (ty->kind) {
    case TY_FLOAT:
      out(MIR_new_insn(mc, MIR_FMOV, rop(r), MIR_new_float_op(mc, (float)node->num.fval)));
      break;
    case TY_DOUBLE:
      out(MIR_new_insn(mc, MIR_DMOV, rop(r), MIR_new_double_op(mc, (double)node->num.fval)));
      break;
    case TY_LDOUBLE:
      out(MIR_new_insn(mc, MIR_LDMOV, rop(r), MIR_new_ldouble_op(mc, (long double)node->num.fval)));
      break;
    default:
      out(MIR_new_insn(mc, MIR_MOV, rop(r), iop(node->num.val)));
    }
    return r;
  }
  case ND_POS:
    return gen_expr(node->m.lhs);
  case ND_NEG: {
    if (is_big_bitint(node->ty)) {
      MIR_reg_t buf = bitint_copy(node->ty, gen_expr(node->m.lhs));
      bitint_call(BI_NEG, 2, (MIR_op_t[]){iop(node->ty->bit_cnt), rop(buf)});
      return buf;
    }
    MIR_reg_t v = gen_expr(node->m.lhs);
    MIR_reg_t r = new_tmp(mir_class(node->ty));
    MIR_insn_code_t code;
    switch (node->ty->kind) {
    case TY_FLOAT: code = MIR_FNEG; break;
    case TY_DOUBLE: code = MIR_DNEG; break;
    case TY_LDOUBLE: code = MIR_LDNEG; break;
    default: code = node->ty->size <= 4 ? MIR_NEGS : MIR_NEG;
    }
    out(MIR_new_insn(mc, code, rop(r), rop(v)));
    return bitint_norm(r, node->ty);
  }
  case ND_VAR:
  case ND_MEMBER:
    return load_node(node, gen_addr(node));
  case ND_DEREF:
    return load_node(node, gen_expr(node->m.lhs));
  case ND_ADDR:
    return gen_addr(node->m.lhs);
  case ND_ASSIGN: {
    // Plain assignment to _Atomic lvalues is an ordinary store, like
    // codegen.c (atomic read-modify-write is lowered to CAS loops by the
    // parser and arrives as ND_CAS).
    MIR_reg_t addr = gen_addr(node->m.lhs);
    MIR_reg_t val = gen_expr(node->m.rhs);
    return gen_store(node->m.lhs, addr, val);
  }
  case ND_ARITH_ASSIGN:
    return gen_arith_assign(node, false);
  case ND_POST_INCDEC:
    return gen_arith_assign(node, true);
  case ND_STMT_EXPR: {
    name_labels(node->blk.local_labels);
    for (Node *n = node->blk.body; n != node->blk.result; n = n->next)
      gen_stmt(n);
    MIR_reg_t r = 0;
    if (node->blk.result)
      r = gen_expr(node->blk.result->m.lhs);
    if (has_defr(node))
      gen_defr(node);
    return r;
  }
  case ND_CHAIN:
  case ND_COMMA:
    gen_void_expr(node->m.lhs);
    return gen_expr(node->m.rhs);
  case ND_CAST:
    return gen_cast2(gen_expr(node->m.lhs), node->m.lhs->ty, node->ty, node->tok);
  case ND_INIT_SEQ: {
    gen_mem_zero(local_addr(node->m.var), node->m.var->ty->size);
    for (Node *n = node->m.lhs; n; n = n->next)
      gen_void_expr(n);
    return 0;
  }
  case ND_COND: {
    MIR_label_t els = MIR_new_label(mc);
    MIR_label_t end = MIR_new_label(mc);
    MIR_type_t cls = mir_class(node->ty);
    // Aggregate-typed conditionals carry the value's address in an
    // integer register, like all aggregate values here.
    bool is_void = node->ty->kind == TY_VOID;
    MIR_reg_t r = is_void ? 0 : new_tmp(cls);
    MIR_insn_code_t mov = cls == MIR_T_F   ? MIR_FMOV
                          : cls == MIR_T_D ? MIR_DMOV
                          : cls == MIR_T_LD ? MIR_LDMOV
                                            : MIR_MOV;

    gen_cond(node->ctrl.cond, false, els);
    MIR_reg_t v1 = gen_expr(node->ctrl.then);
    if (!is_void)
      out(MIR_new_insn(mc, mov, rop(r), rop(v1)));
    out(MIR_new_insn(mc, MIR_JMP, MIR_new_label_op(mc, end)));
    out_lab(els);
    MIR_reg_t v2 = gen_expr(node->ctrl.els);
    if (!is_void)
      out(MIR_new_insn(mc, mov, rop(r), rop(v2)));
    out_lab(end);
    return r;
  }
  case ND_NOT: {
    if (is_big_bitint(node->m.lhs->ty)) {
      MIR_reg_t b = bitint_to_bool(node->m.lhs->ty, gen_expr(node->m.lhs));
      MIR_reg_t r = new_tmp(MIR_T_I64);
      out(MIR_new_insn(mc, MIR_EQ, rop(r), rop(b), iop(0)));
      return r;
    }
    MIR_reg_t v = gen_expr(node->m.lhs);
    MIR_reg_t r = new_tmp(MIR_T_I64);
    switch (node->m.lhs->ty->kind) {
    case TY_FLOAT: out(MIR_new_insn(mc, MIR_FEQ, rop(r), rop(v), MIR_new_float_op(mc, 0.0f))); break;
    case TY_DOUBLE: out(MIR_new_insn(mc, MIR_DEQ, rop(r), rop(v), MIR_new_double_op(mc, 0.0))); break;
    case TY_LDOUBLE: out(MIR_new_insn(mc, MIR_LDEQ, rop(r), rop(v), MIR_new_ldouble_op(mc, 0.0L))); break;
    default: out(MIR_new_insn(mc, MIR_EQ, rop(r), rop(v), iop(0)));
    }
    return r;
  }
  case ND_BITNOT: {
    if (is_big_bitint(node->ty)) {
      MIR_reg_t buf = bitint_copy(node->ty, gen_expr(node->m.lhs));
      bitint_call(BI_BITNOT, 2, (MIR_op_t[]){iop(node->ty->bit_cnt), rop(buf)});
      return buf;
    }
    MIR_reg_t v = gen_expr(node->m.lhs);
    MIR_reg_t r = new_tmp(MIR_T_I64);
    out(MIR_new_insn(mc, node->ty->size <= 4 ? MIR_XORS : MIR_XOR, rop(r), rop(v), iop(-1)));
    return bitint_norm(r, node->ty);
  }
  case ND_LOGAND:
  case ND_LOGOR: {
    MIR_label_t shortcut = MIR_new_label(mc);
    MIR_label_t end = MIR_new_label(mc);
    bool short_cond = node->kind == ND_LOGOR;
    MIR_reg_t r = new_tmp(MIR_T_I64);

    gen_cond(node->m.lhs, short_cond, shortcut);
    gen_cond(node->m.rhs, short_cond, shortcut);
    out(MIR_new_insn(mc, MIR_MOV, rop(r), iop(!short_cond)));
    out(MIR_new_insn(mc, MIR_JMP, MIR_new_label_op(mc, end)));
    out_lab(shortcut);
    out(MIR_new_insn(mc, MIR_MOV, rop(r), iop(short_cond)));
    out_lab(end);
    return r;
  }
  case ND_EQ: case ND_NE: case ND_LT: case ND_LE: case ND_GT: case ND_GE: {
    if (is_big_bitint(node->m.lhs->ty))
      return gen_bitint_cmp(node);
    MIR_reg_t lhs = gen_expr(node->m.lhs);
    MIR_reg_t rhs = gen_expr(node->m.rhs);
    MIR_reg_t r = new_tmp(MIR_T_I64);
    out(MIR_new_insn(mc, cmp_code(node->m.lhs->ty, node->kind - ND_EQ), rop(r), rop(lhs), rop(rhs)));
    return r;
  }
  case ND_ADD: case ND_SUB: case ND_MUL: case ND_DIV: case ND_MOD:
  case ND_BITAND: case ND_BITOR: case ND_BITXOR:
  case ND_SHL: case ND_SHR: case ND_SAR: {
    if (is_big_bitint(node->m.lhs->ty))
      return gen_bitint_arith(node);
    MIR_reg_t lhs = gen_expr(node->m.lhs);
    MIR_reg_t rhs = gen_expr(node->m.rhs);
    return bitint_norm(gen_binop(node->kind, node->ty, lhs, rhs, node->tok), node->ty);
  }
  case ND_FUNCALL:
    return gen_funcall(node);
  case ND_ALLOCA:
  case ND_ALLOCA_ZINIT: {
    MIR_reg_t sz = gen_expr(node->m.lhs);
    MIR_reg_t r = new_tmp(MIR_T_I64);
    // Over-aligned VLAs: over-allocate and round the pointer up. The
    // rounded pointer lands in the variable's slot, so VLA deallocation
    // restores to it; the sub-alignment padding stays allocated until an
    // enclosing scope exits, which is harmless.
    int32_t align = node->m.var ? obj_align(node->m.var) : 16;
    if (align > 16) {
      MIR_reg_t padded = i64_op2(MIR_ADD, sz, iop(align));
      out(MIR_new_insn(mc, MIR_ALLOCA, rop(r), rop(padded)));
      r = i64_op2(MIR_ADD, r, iop(align - 1));
      r = i64_op2(MIR_AND, r, iop(-(int64_t)align));
    } else {
      out(MIR_new_insn(mc, MIR_ALLOCA, rop(r), rop(sz)));
    }
    if (node->kind == ND_ALLOCA_ZINIT) {
      MIR_item_t ms = get_memset();
      MIR_reg_t res = new_tmp(MIR_T_I64);
      out(MIR_new_insn_arr(mc, MIR_CALL, 6,
                           (MIR_op_t[]){MIR_new_ref_op(mc, memset_proto),
                                        MIR_new_ref_op(mc, ms), rop(res), rop(r), iop(0),
                                        rop(sz)}));
    }
    // A VLA declaration stores the allocation into the variable's slot.
    if (node->m.var)
      store_scalar(ty_long, local_addr(node->m.var), 0, r);
    return r;
  }
  case ND_LABEL_VAL: {
    MIR_reg_t r = new_tmp(MIR_T_I64);
    out(MIR_new_insn(mc, MIR_LADDR, rop(r),
                     MIR_new_label_op(mc, get_label("j%" PRIi64, node->jmp.target->lbl.id))));
    return r;
  }
  case ND_GOTO_EXPR: {
    MIR_reg_t v = gen_expr(node->m.lhs);
    out(MIR_new_insn(mc, MIR_JMPI, rop(v)));
    return 0;
  }
  case ND_CAS: {
    // C11 compare-exchange through a host helper: *expected is updated on
    // failure (matching codegen.c); float/double travel as bit patterns.
    Type *ty = node->cas.addr->ty->base;
    if (!is_scalar_fp(ty) && !is_integer(ty) && !is_ptr(ty))
      error_tok(node->tok, "unsupported type for atomic CAS");
    MIR_reg_t addr = gen_expr(node->cas.addr);
    MIR_reg_t expected = gen_expr(node->cas.old_val);
    MIR_reg_t desired = bits_of(ty, gen_expr(node->cas.new_val));

    MIR_item_t proto;
    MIR_item_t fn = get_atomic(atomic_size_idx(ty, node->tok), &proto);
    MIR_reg_t res = new_tmp(MIR_T_I64);
    out(MIR_new_insn_arr(mc, MIR_CALL, 6,
                         (MIR_op_t[]){MIR_new_ref_op(mc, proto), MIR_new_ref_op(mc, fn),
                                      rop(res), rop(addr), rop(expected), rop(desired)}));
    return int_extend(res, ty_bool);
  }
  case ND_EXCH: {
    Type *ty = node->m.lhs->ty->base;
    MIR_reg_t addr = gen_expr(node->m.lhs);
    MIR_reg_t val = bits_of(ty, gen_expr(node->m.rhs));

    MIR_item_t proto;
    MIR_item_t fn = get_atomic(4 + atomic_size_idx(ty, node->tok), &proto);
    MIR_reg_t res = new_tmp(MIR_T_I64);
    out(MIR_new_insn_arr(mc, MIR_CALL, 5,
                         (MIR_op_t[]){MIR_new_ref_op(mc, proto), MIR_new_ref_op(mc, fn),
                                      rop(res), rop(addr), rop(val)}));
    if (is_scalar_fp(ty))
      return value_of_bits(ty, res);
    return is_integer(ty) && ty->size < 8 ? int_extend(res, ty) : res;
  }
  case ND_THREAD_FENCE: {
    MIR_item_t proto;
    MIR_item_t fn = get_atomic(8, &proto);
    out(MIR_new_insn_arr(mc, MIR_CALL, 2,
                         (MIR_op_t[]){MIR_new_ref_op(mc, proto), MIR_new_ref_op(mc, fn)}));
    return 0;
  }
  case ND_VA_START: {
    MIR_reg_t ap = gen_expr(node->m.lhs);
    out(MIR_new_insn(mc, MIR_VA_START, rop(ap)));
    return 0;
  }
  case ND_VA_COPY: {
    MIR_reg_t dst = gen_expr(node->m.lhs);
    MIR_reg_t src = gen_expr(node->m.rhs);
    Type *base = node->m.lhs->ty->base;
    gen_mem_copy(dst, src, base ? base->size : 24);
    return 0;
  }
  case ND_VA_ARG: {
    // The node's value is the address of the fetched argument (the parser
    // wraps it in ND_DEREF); MIR_VA_ARG has matching semantics. Aggregates
    // are copied into the parser-allocated buffer via VA_BLOCK_ARG.
    MIR_reg_t ap = gen_expr(node->m.lhs);
    Type *ty = node->ty->base;
    if (ty->kind == TY_STRUCT || ty->kind == TY_UNION || is_big_bitint(ty)) {
      MIR_reg_t buf = local_addr(node->m.var);
      out(MIR_new_insn(mc, MIR_VA_BLOCK_ARG, rop(buf), rop(ap), iop(ty->size), iop(0)));
      return buf;
    }
    MIR_reg_t r = new_tmp(MIR_T_I64);
    out(MIR_new_insn(mc, MIR_VA_ARG, rop(r), rop(ap),
                     MIR_new_mem_op(mc, mir_type(ty), 0, 0, 0, 1)));
    return r;
  }
  case ND_CKD_ARITH: {
    // Checked arithmetic: compute in a _BitInt wide enough that the exact
    // result fits, store the truncation, and test whether the full result
    // exceeds the destination's range (mirrors codegen.c).
    Type *res_ty = node->m.target->ty->base;
    int32_t chk_bits = res_ty->is_unsigned + bit_size(res_ty);
    int32_t bits = MAX(chk_bits, res_ty->size * 8);
    bits = MAX(bits, ovf_headroom(node->m.lhs->ty, node->arith_kind));
    bits = MAX(bits, ovf_headroom(node->m.rhs->ty, node->arith_kind));
    bits = align_to(bits, 64);

    Type *bty = new_bitint(bits, node->tok);
    MIR_reg_t target = gen_expr(node->m.target);
    MIR_reg_t lhs = gen_ckd_operand(node->m.lhs, bty);
    MIR_reg_t rhs = gen_ckd_operand(node->m.rhs, bty);

    BitintHelper h;
    switch (node->arith_kind) {
    case ND_ADD: h = BI_ADD; break;
    case ND_SUB: h = BI_SUB; break;
    case ND_MUL: h = BI_MUL; break;
    default: internal_error();
    }
    bitint_call(h, 3, (MIR_op_t[]){iop(bits), rop(lhs), rop(rhs)});

    gen_mem_copy(target, rhs, res_ty->size);
    return bitint_call(BI_OVERFLOW, 4, (MIR_op_t[]){iop(bits), rop(rhs), iop(chk_bits),
                                                    iop(res_ty->is_unsigned)});
  }
  case ND_FRAME_ADDR:
  case ND_RTN_ADDR:
    error_tok(node->tok, "frame/return address builtins are not supported by the MIR backend");
  case ND_ASM:
    error_tok(node->tok, "inline assembly is not supported by the MIR backend");
  }
  error_tok(node->tok, "invalid expression for the MIR backend");
}

static void gen_void_expr(Node *node) {
  gen_expr(node);
}

// Generate a conditional branch to lab, taken when the condition's truth
// value equals jump_on_true. Comparisons fuse into compare-and-branch.
static void gen_cond(Node *node, bool jump_on_true, MIR_label_t lab) {
  switch (node->kind) {
  case ND_NOT:
    gen_cond(node->m.lhs, !jump_on_true, lab);
    return;
  case ND_CHAIN:
  case ND_COMMA:
    gen_void_expr(node->m.lhs);
    gen_cond(node->m.rhs, jump_on_true, lab);
    return;
  case ND_EQ: case ND_NE: case ND_LT: case ND_LE: case ND_GT: case ND_GE: {
    // Branch-fused comparisons; invert the comparison when branching on false.
    static const int inverse[6] = {1, 0, 5, 4, 3, 2}; // EQ<->NE LT<->GE LE<->GT
    int idx = node->kind - ND_EQ;
    if (!jump_on_true)
      idx = inverse[idx];

    Type *ty = node->m.lhs->ty;
    static const MIR_insn_code_t f[6] = {MIR_FBEQ, MIR_FBNE, MIR_FBLT, MIR_FBLE, MIR_FBGT, MIR_FBGE};
    static const MIR_insn_code_t d[6] = {MIR_DBEQ, MIR_DBNE, MIR_DBLT, MIR_DBLE, MIR_DBGT, MIR_DBGE};
    static const MIR_insn_code_t ld[6] = {MIR_LDBEQ, MIR_LDBNE, MIR_LDBLT, MIR_LDBLE, MIR_LDBGT, MIR_LDBGE};
    static const MIR_insn_code_t s[6] = {MIR_BEQS, MIR_BNES, MIR_BLTS, MIR_BLES, MIR_BGTS, MIR_BGES};
    static const MIR_insn_code_t us[6] = {MIR_BEQS, MIR_BNES, MIR_UBLTS, MIR_UBLES, MIR_UBGTS, MIR_UBGES};
    static const MIR_insn_code_t i[6] = {MIR_BEQ, MIR_BNE, MIR_BLT, MIR_BLE, MIR_BGT, MIR_BGE};
    static const MIR_insn_code_t u[6] = {MIR_BEQ, MIR_BNE, MIR_UBLT, MIR_UBLE, MIR_UBGT, MIR_UBGE};

    // FP comparisons cannot be inverted (NaN), and big-bitint comparisons
    // go through a helper call; fall back to value+branch.
    if (is_big_bitint(ty) ||
        (is_scalar_fp(ty) && !jump_on_true && node->kind != ND_EQ && node->kind != ND_NE))
      break;

    MIR_insn_code_t code;
    switch (ty->kind) {
    case TY_FLOAT: code = f[idx]; break;
    case TY_DOUBLE: code = d[idx]; break;
    case TY_LDOUBLE: code = ld[idx]; break;
    default:
      if (is_integer(ty) && ty->size <= 4)
        code = ty->is_unsigned ? us[idx] : s[idx];
      else
        code = (!is_integer(ty) || ty->is_unsigned) ? u[idx] : i[idx];
    }
    MIR_reg_t lhs = gen_expr(node->m.lhs);
    MIR_reg_t rhs = gen_expr(node->m.rhs);
    out(MIR_new_insn(mc, code, MIR_new_label_op(mc, lab), rop(lhs), rop(rhs)));
    return;
  }
  case ND_LOGAND:
    if (!jump_on_true) {
      gen_cond(node->m.lhs, false, lab);
      gen_cond(node->m.rhs, false, lab);
    } else {
      MIR_label_t fall = MIR_new_label(mc);
      gen_cond(node->m.lhs, false, fall);
      gen_cond(node->m.rhs, true, lab);
      out_lab(fall);
    }
    return;
  case ND_LOGOR:
    if (jump_on_true) {
      gen_cond(node->m.lhs, true, lab);
      gen_cond(node->m.rhs, true, lab);
    } else {
      MIR_label_t fall = MIR_new_label(mc);
      gen_cond(node->m.lhs, true, fall);
      gen_cond(node->m.rhs, false, lab);
      out_lab(fall);
    }
    return;
  }

  MIR_reg_t v = gen_expr(node);
  if (is_big_bitint(node->ty)) {
    MIR_reg_t b = bitint_to_bool(node->ty, v);
    out(MIR_new_insn(mc, jump_on_true ? MIR_BT : MIR_BF, MIR_new_label_op(mc, lab), rop(b)));
    return;
  }
  switch (node->ty->kind) {
  case TY_FLOAT: {
    MIR_reg_t b = new_tmp(MIR_T_I64);
    out(MIR_new_insn(mc, MIR_FNE, rop(b), rop(v), MIR_new_float_op(mc, 0.0f)));
    out(MIR_new_insn(mc, jump_on_true ? MIR_BT : MIR_BF, MIR_new_label_op(mc, lab), rop(b)));
    return;
  }
  case TY_DOUBLE: {
    MIR_reg_t b = new_tmp(MIR_T_I64);
    out(MIR_new_insn(mc, MIR_DNE, rop(b), rop(v), MIR_new_double_op(mc, 0.0)));
    out(MIR_new_insn(mc, jump_on_true ? MIR_BT : MIR_BF, MIR_new_label_op(mc, lab), rop(b)));
    return;
  }
  case TY_LDOUBLE: {
    MIR_reg_t b = new_tmp(MIR_T_I64);
    out(MIR_new_insn(mc, MIR_LDNE, rop(b), rop(v), MIR_new_ldouble_op(mc, 0.0L)));
    out(MIR_new_insn(mc, jump_on_true ? MIR_BT : MIR_BF, MIR_new_label_op(mc, lab), rop(b)));
    return;
  }
  default:
    out(MIR_new_insn(mc, jump_on_true ? MIR_BT : MIR_BF, MIR_new_label_op(mc, lab), rop(v)));
  }
}

//
// Statements
//

static void gen_return(Node *node) {
  Type *rt = cur_fn->ty->return_ty;

  if (rt->kind == TY_STRUCT || rt->kind == TY_UNION || is_big_bitint(rt)) {
    if (node->m.lhs) {
      MIR_reg_t v = gen_expr(node->m.lhs); // aggregate value = its address
      gen_mem_copy(ret_addr_reg, v, rt->size);
    }
    if (has_defr(node))
      gen_defr(node);
    out(MIR_new_ret_insn(mc, 0));
    return;
  }

  if (!node->m.lhs) {
    if (has_defr(node))
      gen_defr(node);
    if (rt->kind == TY_VOID) {
      out(MIR_new_ret_insn(mc, 0));
    } else {
      MIR_reg_t r = new_tmp(mir_class(rt));
      if (!is_scalar_fp(rt))
        out(MIR_new_insn(mc, MIR_MOV, rop(r), iop(0)));
      else if (rt->kind == TY_FLOAT)
        out(MIR_new_insn(mc, MIR_FMOV, rop(r), MIR_new_float_op(mc, 0.0f)));
      else if (rt->kind == TY_DOUBLE)
        out(MIR_new_insn(mc, MIR_DMOV, rop(r), MIR_new_double_op(mc, 0.0)));
      else
        out(MIR_new_insn(mc, MIR_LDMOV, rop(r), MIR_new_ldouble_op(mc, 0.0L)));
      out(MIR_new_ret_insn(mc, 1, rop(r)));
    }
    return;
  }

  MIR_reg_t v = gen_expr(node->m.lhs);
  if (has_defr(node))
    gen_defr(node);
  out(MIR_new_ret_insn(mc, 1, rop(v)));
}

static void gen_stmt(Node *node) {
  switch (node->kind) {
  case ND_NULL_STMT:
    return;
  case ND_IF: {
    MIR_label_t els = MIR_new_label(mc);
    gen_cond(node->ctrl.cond, false, els);
    gen_stmt(node->ctrl.then);
    if (node->ctrl.els) {
      MIR_label_t end = MIR_new_label(mc);
      out(MIR_new_insn(mc, MIR_JMP, MIR_new_label_op(mc, end)));
      out_lab(els);
      gen_stmt(node->ctrl.els);
      out_lab(end);
    } else {
      out_lab(els);
    }
    return;
  }
  case ND_FOR: {
    int64_t c = node->ctrl.id = ++ctrl_cnt;
    MIR_label_t begin = MIR_new_label(mc);

    if (node->ctrl.for_init)
      gen_stmt(node->ctrl.for_init);
    out_lab(begin);
    if (node->ctrl.cond)
      gen_cond(node->ctrl.cond, false, get_label("b%" PRIi64, c));
    gen_stmt(node->ctrl.then);
    out_lab(get_label("c%" PRIi64, c));
    if (node->ctrl.for_inc)
      gen_void_expr(node->ctrl.for_inc);
    gen_defr(node);
    out(MIR_new_insn(mc, MIR_JMP, MIR_new_label_op(mc, begin)));
    out_lab(get_label("b%" PRIi64, c));
    return;
  }
  case ND_DO: {
    int64_t c = node->ctrl.id = ++ctrl_cnt;
    MIR_label_t begin = MIR_new_label(mc);

    out_lab(begin);
    gen_stmt(node->ctrl.then);
    out_lab(get_label("c%" PRIi64, c));
    gen_cond(node->ctrl.cond, true, begin);
    out_lab(get_label("b%" PRIi64, c));
    return;
  }
  case ND_SWITCH: {
    int64_t c = node->ctrl.id = ++ctrl_cnt;
    MIR_reg_t v = gen_expr(node->ctrl.cond);
    bool s4 = node->ctrl.cond->ty->size <= 4;

    int64_t case_cnt = 1;
    Node *label = NULL;
    for (CaseRange *cr = node->ctrl.sw_cases; cr; cr = cr->next) {
      if (cr->label == node->ctrl.sw_default)
        continue;
      if (label != cr->label) {
        label = cr->label;
        label->cases.id = ++case_cnt;
      }
      MIR_label_t target = get_label("s%" PRIi64 ".%" PRIi64, c, case_cnt);
      if (cr->hi == cr->lo) {
        out(MIR_new_insn(mc, s4 ? MIR_BEQS : MIR_BEQ, MIR_new_label_op(mc, target), rop(v),
                         iop(cr->lo)));
        continue;
      }
      // (unsigned)(v - lo) <= hi - lo
      MIR_reg_t t = new_tmp(MIR_T_I64);
      out(MIR_new_insn(mc, s4 ? MIR_SUBS : MIR_SUB, rop(t), rop(v), iop(cr->lo)));
      out(MIR_new_insn(mc, s4 ? MIR_UBLES : MIR_UBLE, MIR_new_label_op(mc, target), rop(t),
                       iop((uint64_t)cr->hi - cr->lo)));
    }

    if (node->ctrl.sw_default)
      out(MIR_new_insn(mc, MIR_JMP,
                       MIR_new_label_op(mc, get_label("d%" PRIi64, c))));
    else
      out(MIR_new_insn(mc, MIR_JMP,
                       MIR_new_label_op(mc, get_label("b%" PRIi64, c))));
    gen_stmt(node->ctrl.then);
    out_lab(get_label("b%" PRIi64, c));
    return;
  }
  case ND_CASE:
    out_lab(get_label("s%" PRIi64 ".%" PRIi64, node->cases.parent_sw->ctrl.id, node->cases.id));
    return;
  case ND_DEFAULT:
    out_lab(get_label("d%" PRIi64, node->cases.parent_sw->ctrl.id));
    return;
  case ND_BLOCK:
    name_labels(node->blk.local_labels);
    for (Node *n = node->blk.body; n; n = n->next)
      gen_stmt(n);
    // Defers run when control falls out of the block. After a terminating
    // statement this emits unreachable code, which is harmless.
    gen_defr(node);
    return;
  case ND_BREAK:
    gen_defr(node);
    out(MIR_new_insn(mc, MIR_JMP,
                     MIR_new_label_op(mc, get_label("b%" PRIi64, node->jmp.parent_loop->ctrl.id))));
    return;
  case ND_CONT:
    gen_defr(node);
    out(MIR_new_insn(mc, MIR_JMP,
                     MIR_new_label_op(mc, get_label("c%" PRIi64, node->jmp.parent_loop->ctrl.id))));
    return;
  case ND_GOTO:
    gen_defr(node);
    out(MIR_new_insn(mc, MIR_JMP,
                     MIR_new_label_op(mc, get_label("j%" PRIi64, node->jmp.target->lbl.id))));
    return;
  case ND_GOTO_EXPR: {
    MIR_reg_t v = gen_expr(node->m.lhs);
    out(MIR_new_insn(mc, MIR_JMPI, rop(v)));
    return;
  }
  case ND_LABEL:
    if (node->lbl.id)
      out_lab(get_label("j%" PRIi64, node->lbl.id));
    return;
  case ND_RETURN:
    gen_return(node);
    return;
  case ND_EXPR_STMT:
    gen_void_expr(node->m.lhs);
    return;
  case ND_ASM:
    error_tok(node->tok, "inline assembly is not supported by the MIR backend");
  }
  error_tok(node->tok, "invalid statement for the MIR backend");
}

//
// Backend boundary
//

void prepare_funcall(Node *node, Scope *scope) {
  // MIR handles per-target calling conventions; nothing to precompute.
  (void)node, (void)scope;
}

void prepare_inline_asm(Node *node) {
  error_tok(node->tok, "inline assembly is not supported by the MIR backend");
}

void emit_text(Obj *fn) {
  if (fn->is_naked)
    error("naked functions are not supported by the MIR backend");
  if (fn->alias_name)
    error("symbol aliases are not supported by the MIR backend");
  if (fn->is_ctor || fn->is_dtor)
    error("constructor/destructor functions are not yet supported by the MIR backend");

  Type *rt = fn->ty->return_ty;
  cur_fn = fn;
  tmp_cnt = 0;
  scratch_slot = 0;
  free(label_map.buckets);
  label_map = (HashMap){0};

  // Signature. Aggregates and big bitints travel as MIR block args;
  // struct and big-bitint returns become a hidden first RBLK argument
  // carrying the caller's buffer.
  bool rtn_by_addr = rt->kind == TY_STRUCT || rt->kind == TY_UNION || is_big_bitint(rt);

  int nparams = 0;
  for (Obj *p = fn->ty->param_list; p; p = p->param_next)
    nparams++;

  int nargs = nparams + rtn_by_addr;
  MIR_var_t *vars = arena_malloc(&cc1_arena, MAX(nargs, 1) * sizeof(MIR_var_t));
  int i = 0;
  if (rtn_by_addr) {
    vars[0].type = MIR_T_RBLK;
    vars[0].name = "Ret.Addr";
    vars[0].size = rt->size;
    i++;
  }
  for (Obj *p = fn->ty->param_list; p; p = p->param_next, i++) {
    if (p->ty->kind == TY_STRUCT || p->ty->kind == TY_UNION || is_big_bitint(p->ty)) {
      vars[i].type = MIR_T_BLK;
      vars[i].size = p->ty->size;
    } else {
      vars[i].type = mir_type(p->ty);
      vars[i].size = 0;
    }
    vars[i].name = arena_format(&cc1_arena, "A%d", i);
  }

  size_t nres = (rt->kind == TY_VOID || rtn_by_addr) ? 0 : 1;
  MIR_type_t res_ty = nres ? mir_type(rt) : MIR_T_UNDEF;
  const char *name = sym_name(fn);

  fn_item = fn->ty->is_variadic
                ? MIR_new_vararg_func_arr(mc, name, nres, &res_ty, nargs, vars)
                : MIR_new_func_arr(mc, name, nres, &res_ty, nargs, vars);
  fn_func = MIR_get_item_func(mc, fn_item);
  sym_mark_defined(name);

  ret_addr_reg = rtn_by_addr ? MIR_reg(mc, "Ret.Addr", fn_func) : 0;

  fn->output = arena_calloc(&cc1_arena, sizeof(FuncObj));
  fn->output->item = fn_item;

  // Slots for all locals (parameters included), then spill the incoming
  // arguments into their slots. A block argument's register holds the
  // block's address; copy it for by-value semantics.
  alloca_scope(fn->ty->scopes);

  // Record the entry stack position for VLA deallocation. This comes
  // after the entry allocas so that releasing all VLAs keeps them intact.
  vla_base_slot = 0;
  if (fn->dealloc_vla) {
    vla_base_slot = new_tmp(MIR_T_I64);
    out(MIR_new_insn(mc, MIR_ALLOCA, rop(vla_base_slot), iop(16)));
    MIR_reg_t pos = new_tmp(MIR_T_I64);
    out(MIR_new_insn(mc, MIR_BSTART, rop(pos)));
    store_scalar(ty_long, vla_base_slot, 0, pos);
  }

  i = rtn_by_addr;
  for (Obj *p = fn->ty->param_list; p; p = p->param_next, i++) {
    MIR_reg_t arg = MIR_reg(mc, vars[i].name, fn_func);
    if (p->ty->kind == TY_STRUCT || p->ty->kind == TY_UNION || is_big_bitint(p->ty))
      gen_mem_copy(local_addr(p), arg, p->ty->size);
    else
      store_scalar(p->ty, local_addr(p), 0, arg);
  }

  gen_stmt(fn->body);

  // Implicit return: 0 for main (and harmlessly for other value-returning
  // functions that flow off the end), plain ret otherwise.
  if (nres) {
    MIR_reg_t r = new_tmp(mir_class(rt));
    if (!is_scalar_fp(rt))
      out(MIR_new_insn(mc, MIR_MOV, rop(r), iop(0)));
    else if (rt->kind == TY_FLOAT)
      out(MIR_new_insn(mc, MIR_FMOV, rop(r), MIR_new_float_op(mc, 0.0f)));
    else if (rt->kind == TY_DOUBLE)
      out(MIR_new_insn(mc, MIR_DMOV, rop(r), MIR_new_double_op(mc, 0.0)));
    else
      out(MIR_new_insn(mc, MIR_LDMOV, rop(r), MIR_new_ldouble_op(mc, 0.0L)));
    out(MIR_new_ret_insn(mc, 1, rop(r)));
  } else {
    out(MIR_new_ret_insn(mc, 0));
  }

  // Function-scope static variables live in the per-declaration AST arena,
  // so they are emitted now. This must happen before MIR_finish_func: an
  // initializer may hold &&label references, and lref data items attach to
  // the function being built.
  for (Obj *var = fn->static_lvars; var; var = var->next)
    emit_data_obj(var);
  fn->static_lvars = NULL;

  MIR_finish_func(mc);
  fn_item = NULL;
  fn_func = NULL;
  cur_fn = NULL;

  if (fn_is_exported(fn))
    MIR_new_export(mc, name);
}

//
// File-scope data
//

// First nonzero byte, or NULL if the range is all zero.
static const void *memchr_inv_zero(const char *p, int64_t n) {
  for (int64_t i = 0; i < n; i++)
    if (p[i])
      return p + i;
  return NULL;
}

static void emit_data_obj(Obj *var) {
  if (var->alias_name)
    error("symbol aliases are not supported by the MIR backend");

  // Thread-local: emit the gcc -femulated-tls shape - an __emutls_v.<name>
  // control object {size, align, index, template} consumed by the runtime's
  // __slimcc_emutls_get_address, plus the init image as an ordinary
  // module-local data object. All code references go through the control
  // object (see gen_addr), so the variable's own name is never defined.
  if (var->is_tls) {
    const char *name = sym_name(var);
    const char *name_t = arena_format(&cc1_arena, "__emutls_t.%s", name);
    const char *name_v = arena_format(&cc1_arena, "__emutls_v.%s", name);

    int64_t size = MAX(var->ty->size, 1);
    if (var->ty->kind == TY_ARRAY && var->ty->size < 0)
      size = MAX(var->ty->base->size, 1);
    else if (var->ty->size < 0)
      error("object '%s' has incomplete type", name);

    // Forward for the ref_data below; created before the template item so
    // it isn't interleaved between that object's data chunks.
    SymItem *ti = sym_entry(name_t);
    if (!ti->item)
      ti->item = MIR_new_forward(mc, ti->key);

    // The per-thread copy is made from the template by the runtime, so the
    // template's own address and alignment conventions don't matter; clamp
    // the alignment so emission doesn't take the over-aligned bss path.
    Obj tmp = *var;
    tmp.is_tls = false;
    tmp.is_static = true;
    tmp.name = (char *)name_t;
    tmp.asm_name = tmp.alias_name = NULL;
    if (obj_align(var) > 16)
      tmp.alt_align = 16;
    emit_data_obj(&tmp);

    int64_t words[3] = {size, obj_align(var), 0};
    MIR_new_data(mc, name_v, MIR_T_I64, 3, words);
    MIR_new_ref_data(mc, NULL, ti->item, 0);
    sym_mark_defined(name_v);
    if (!var->is_static)
      MIR_new_export(mc, name_v);
    return;
  }

  const char *name = sym_name(var);
  int64_t size = MAX(var->ty->size, 1);
  sym_mark_defined(name);

  // A tentative array of unknown size has one element (C 6.9.2p2).
  if (var->ty->kind == TY_ARRAY && var->ty->size < 0)
    size = MAX(var->ty->base->size, 1);
  else if (var->ty->size < 0)
    error("object '%s' has incomplete type", name);

  // MIR data items only guarantee malloc alignment. Stricter alignments
  // work for zero-initialized objects by over-allocating bss; the object
  // lives at the rounded-up address, which is what every code reference
  // computes (see gen_addr). Initialized data would need its image at a
  // load-time-dependent offset, which MIR cannot express.
  int32_t align = obj_align(var);
  bool zero_init = !var->init_data ||
                   (!var->rel && !memchr_inv_zero(var->init_data, size));
  if (align > 16) {
    if (!zero_init)
      error("initialized object '%s' with alignment %d is not supported by the MIR backend "
            "(only up to 16, or zero-initialized)", name, align);
    MIR_new_bss(mc, name, size + align);
    if (!var->is_static)
      MIR_new_export(mc, name);
    return;
  }

  if (!var->init_data) {
    MIR_new_bss(mc, name, size);
    if (!var->is_static)
      MIR_new_export(mc, name);
    return;
  }

  // Resolve referenced symbols first: sym_item may create forward items,
  // and a non-data item interleaved between this object's chunks would
  // split MIR's contiguous data section. The address a relocation stores
  // is the item start, so references to over-aligned objects (whose code
  // address is rounded up past that) cannot be expressed.
  for (Relocation *rel = var->rel; rel; rel = rel->next)
    if (rel->var) {
      if (rel->var->is_tls)
        error("static initializer takes the address of thread-local '%s', "
              "which is not supported by the MIR backend", sym_name(rel->var));
      if (obj_align(rel->var) > 16 && rel->var->ty->kind != TY_FUNC)
        error("static initializer takes the address of over-aligned object '%s', "
              "which is not supported by the MIR backend", sym_name(rel->var));
      sym_item(rel->var);
    }

  // Emit the object as one named section followed by anonymous
  // continuation items; MIR lays consecutive anonymous items contiguously.
  const char *next_name = name;
  int64_t pos = 0;
  for (Relocation *rel = var->rel; rel; rel = rel->next) {
    if (rel->offset > pos) {
      MIR_new_data(mc, next_name, MIR_T_U8, rel->offset - pos, var->init_data + pos);
      next_name = NULL;
      pos = rel->offset;
    }
    if (rel->label) {
      // &&label in a function-static initializer (rel->label is the
      // ND_LABEL_VAL node); emitted from emit_text while the function's
      // labels are still in scope.
      MIR_new_lref_data(mc, next_name,
                        get_label("j%" PRIi64, rel->label->jmp.target->lbl.id), NULL,
                        rel->addend);
    } else {
      MIR_new_ref_data(mc, next_name, sym_item(rel->var), rel->addend);
    }
    next_name = NULL;
    pos += 8;
  }
  if (pos < size || next_name)
    MIR_new_data(mc, next_name, MIR_T_U8, size - pos, var->init_data + pos);

  if (!var->is_static)
    MIR_new_export(mc, name);
}

int codegen(Obj *prog, FILE *out_file) {
  (void)out_file;

  for (Obj *var = prog; var; var = var->next) {
    if (!var->is_definition)
      continue;
    if (var->ty->kind == TY_ASM)
      error("file-scope assembly is not supported by the MIR backend");
    if (var->ty->kind == TY_FUNC)
      continue; // already emitted by emit_text
    emit_data_obj(var);
  }

  // References were created as forward items because definedness isn't
  // known until the whole unit is parsed. Forwards to names this module
  // never defines are now retyped in place as imports (the name lives in
  // the same union slot), so the embedder resolves them at link time.
  for (int32_t i = 0; i < sym_items.capacity; i++) {
    HashEntry *ent = &sym_items.buckets[i];
    if (!ent->key || ent->key == TOMBSTONE)
      continue;
    SymItem *si = ent->val;
    if (si->item && !si->is_defined)
      si->item->item_type = MIR_import_item;
  }

  MIR_finish_module(mc);
  result_mod = cur_mod;
  cur_mod = NULL;
  return 0;
}

//
// Module lifecycle (used by libslimcc.c)
//

// Free the module-lifetime symbol table (and its SymItem values) and the
// function-lifetime label map. Called at the end of each compile via reset_all
// so this state is not retained idle until the next compile; codegen_mir_begin
// also calls it defensively in case a prior compile ended abnormally.
void codegen_mir_reset(void) {
  for (int32_t i = 0; i < sym_items.capacity; i++) {
    HashEntry *ent = &sym_items.buckets[i];
    if (ent->key && ent->key != TOMBSTONE)
      free(ent->val);
  }
  free(sym_items.buckets);
  sym_items = (HashMap){0};
  free(label_map.buckets);
  label_map = (HashMap){0};
}

void codegen_mir_begin(MIR_context_t ctx, const char *module_name) {
  mc = ctx;
  cur_mod = MIR_new_module(mc, module_name);
  result_mod = NULL;
  fn_item = NULL;
  fn_func = NULL;
  cur_fn = NULL;
  tmp_cnt = proto_cnt = ctrl_cnt = 0;
  scratch_slot = 0;
  memset_import = memcpy_import = memset_proto = memcpy_proto = NULL;
  emutls_import = emutls_proto = NULL;
  memset(atomic_imports, 0, sizeof(atomic_imports));
  memset(atomic_protos, 0, sizeof(atomic_protos));
  memset(bitint_items, 0, sizeof(bitint_items));
  memset(bitint_protos, 0, sizeof(bitint_protos));

  codegen_mir_reset();
}

MIR_module_t codegen_mir_result(void) {
  return result_mod;
}

void codegen_mir_abort(void) {
  if (!mc)
    return;
  // Clear our state first so a re-entered abort skips the failing step.
  MIR_item_t item = fn_item;
  MIR_module_t mod = cur_mod;
  fn_item = NULL;
  fn_func = NULL;
  cur_fn = NULL;
  cur_mod = NULL;
  if (item)
    MIR_finish_func(mc);
  if (mod)
    MIR_finish_module(mc);
  mc = NULL;
}
