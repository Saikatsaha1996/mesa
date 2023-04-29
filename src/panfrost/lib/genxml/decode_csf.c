/*
 * Copyright (C) 2022-2023 Collabora, Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "genxml/gen_macros.h"
#include "decode.h"

#if PAN_ARCH >= 10
/* Limit for Mali-G610. -1 because we're not including the active frame */
#define MAX_CALL_STACK_DEPTH (8 - 1)

struct queue_ctx {
   /* Size of CSHWIF register file in 32-bit registers */
   unsigned nr_regs;

   /* CSHWIF register file */
   uint32_t *regs;

   /* Current instruction pointer (CPU pointer for convenience) */
   uint64_t *ip;

   /* Current instruction end pointer */
   uint64_t *end;

   /* Call stack. Depth=0 means root */
   struct {
      /* Link register to return to */
      uint64_t *lr;

      /* End pointer, there is a return (or exit) after */
      uint64_t *end;
   } call_stack[MAX_CALL_STACK_DEPTH];
   uint8_t call_stack_depth;

   unsigned gpu_id;
};

static void
dump_cshwif_registers(struct queue_ctx *ctx)
{
   for (unsigned i = 0; i < ctx->nr_regs; ++i)
      fprintf(pandecode_dump_stream, "r%u: %X\n", i, ctx->regs[i]);
}

static uint32_t
cs_get_u32(struct queue_ctx *ctx, uint8_t reg)
{
   assert(reg < ctx->nr_regs);
   return ctx->regs[reg];
}

static uint64_t
cs_get_u64(struct queue_ctx *ctx, uint8_t reg)
{
   return (((uint64_t)cs_get_u32(ctx, reg + 1)) << 32) | cs_get_u32(ctx, reg);
}

static void
pandecode_run_compute(FILE *fp, struct queue_ctx *ctx,
                      struct MALI_CEU_RUN_COMPUTE *I)
{
   const char *axes[4] = {"x_axis", "y_axis", "z_axis"};

   /* Print the instruction. Ignore the selects and the flags override
    * since we'll print them implicitly later.
    */
   fprintf(fp, "RUN_COMPUTE.%s #%u\n", axes[I->task_axis], I->task_increment);

   pandecode_indent++;

   unsigned reg_srt = 0 + (I->srt_select * 2);
   unsigned reg_fau = 8 + (I->fau_select * 2);
   unsigned reg_spd = 16 + (I->spd_select * 2);
   unsigned reg_tsd = 24 + (I->tsd_select * 2);

   GENX(pandecode_resource_tables)(cs_get_u64(ctx, reg_srt), "Resources");

   mali_ptr fau = cs_get_u64(ctx, reg_fau);

   if (fau)
      GENX(pandecode_fau)(fau & BITFIELD64_MASK(48), fau >> 56, "FAU");

   GENX(pandecode_shader)(cs_get_u64(ctx, reg_spd), "Shader", ctx->gpu_id);

   DUMP_ADDR(LOCAL_STORAGE, cs_get_u64(ctx, reg_tsd),
             "Local Storage @%" PRIx64 ":\n", cs_get_u64(ctx, reg_tsd));

   pandecode_log("Global attribute offset: %u\n", cs_get_u32(ctx, 32));
   DUMP_CL(COMPUTE_SIZE_WORKGROUP, &ctx->regs[33], "Workgroup size\n");
   pandecode_log("Job offset X: %u\n", cs_get_u32(ctx, 34));
   pandecode_log("Job offset Y: %u\n", cs_get_u32(ctx, 35));
   pandecode_log("Job offset Z: %u\n", cs_get_u32(ctx, 36));
   pandecode_log("Job size X: %u\n", cs_get_u32(ctx, 37));
   pandecode_log("Job size Y: %u\n", cs_get_u32(ctx, 38));
   pandecode_log("Job size Z: %u\n", cs_get_u32(ctx, 39));

   pandecode_indent--;
}

static void
pandecode_run_idvs(FILE *fp, struct queue_ctx *ctx, struct MALI_CEU_RUN_IDVS *I)
{
   /* Print the instruction. Ignore the selects and the flags override
    * since we'll print them implicitly later.
    */
   fprintf(fp, "RUN_IDVS%s", I->malloc_enable ? "" : ".no_malloc");

   if (I->draw_id_register_enable)
      fprintf(fp, " r%u", I->draw_id);

   fprintf(fp, "\n");

   pandecode_indent++;

   /* Merge flag overrides with the register flags */
   uint32_t tiler_flags_raw = cs_get_u64(ctx, 56);
   tiler_flags_raw |= I->flags_override;
   pan_unpack(&tiler_flags_raw, PRIMITIVE_FLAGS, tiler_flags);

   unsigned reg_position_srt = 0;
   unsigned reg_position_fau = 8;
   unsigned reg_position_tsd = 24;

   unsigned reg_vary_srt = I->varying_srt_select ? 2 : 0;
   unsigned reg_vary_fau = I->varying_fau_select ? 10 : 8;
   unsigned reg_vary_tsd = I->varying_tsd_select ? 26 : 24;

   unsigned reg_frag_srt = I->fragment_srt_select ? 4 : 0;
   unsigned reg_frag_fau = 12;
   unsigned reg_frag_tsd = I->fragment_tsd_select ? 28 : 24;

   uint64_t position_srt = cs_get_u64(ctx, reg_position_srt);
   uint64_t vary_srt = cs_get_u64(ctx, reg_vary_srt);
   uint64_t frag_srt = cs_get_u64(ctx, reg_frag_srt);

   GENX(pandecode_resource_tables)(position_srt, "Position resources");
   GENX(pandecode_resource_tables)(vary_srt, "Varying resources");
   GENX(pandecode_resource_tables)(frag_srt, "Fragment resources");

   mali_ptr position_fau = cs_get_u64(ctx, reg_position_fau);
   mali_ptr vary_fau = cs_get_u64(ctx, reg_vary_fau);
   mali_ptr fragment_fau = cs_get_u64(ctx, reg_frag_fau);

   if (position_fau) {
      uint64_t lo = position_fau & BITFIELD64_MASK(48);
      uint64_t hi = position_fau >> 56;

      GENX(pandecode_fau)(lo, hi, "Position FAU");
   }

   if (vary_fau) {
      uint64_t lo = vary_fau & BITFIELD64_MASK(48);
      uint64_t hi = vary_fau >> 56;

      GENX(pandecode_fau)(lo, hi, "Varying FAU");
   }

   if (fragment_fau) {
      uint64_t lo = fragment_fau & BITFIELD64_MASK(48);
      uint64_t hi = fragment_fau >> 56;

      GENX(pandecode_fau)(lo, hi, "Fragment FAU");
   }

   GENX(pandecode_shader)(cs_get_u64(ctx, 16), "Position shader", ctx->gpu_id);

   if (tiler_flags.secondary_shader) {
      uint64_t ptr = cs_get_u64(ctx, 18);

      GENX(pandecode_shader)(ptr, "Varying shader", ctx->gpu_id);
   }

   GENX(pandecode_shader)(cs_get_u64(ctx, 20), "Fragment shader", ctx->gpu_id);

   DUMP_ADDR(LOCAL_STORAGE, cs_get_u64(ctx, 24),
             "Position Local Storage @%" PRIx64 ":\n",
             cs_get_u64(ctx, reg_position_tsd));
   DUMP_ADDR(LOCAL_STORAGE, cs_get_u64(ctx, 24),
             "Varying Local Storage @%" PRIx64 ":\n",
             cs_get_u64(ctx, reg_vary_tsd));
   DUMP_ADDR(LOCAL_STORAGE, cs_get_u64(ctx, 30),
             "Fragment Local Storage @%" PRIx64 ":\n",
             cs_get_u64(ctx, reg_frag_tsd));

   pandecode_log("Global attribute offset: %u\n", cs_get_u32(ctx, 32));
   pandecode_log("Index count: %u\n", cs_get_u32(ctx, 33));
   pandecode_log("Instance count: %u\n", cs_get_u32(ctx, 34));

   if (tiler_flags.index_type)
      pandecode_log("Index offset: %u\n", cs_get_u32(ctx, 35));

   pandecode_log("Vertex offset: %d\n", cs_get_u32(ctx, 36));
   pandecode_log("Instance offset: %u\n", cs_get_u32(ctx, 37));
   pandecode_log("Tiler DCD flags2: %X\n", cs_get_u32(ctx, 38));

   if (tiler_flags.index_type)
      pandecode_log("Index array size: %u\n", cs_get_u32(ctx, 39));

   GENX(pandecode_tiler)(cs_get_u64(ctx, 40), ctx->gpu_id);

   DUMP_CL(SCISSOR, &ctx->regs[42], "Scissor\n");
   pandecode_log("Low depth clamp: %f\n", uif(cs_get_u32(ctx, 44)));
   pandecode_log("High depth clamp: %f\n", uif(cs_get_u32(ctx, 45)));
   pandecode_log("Occlusion: %" PRIx64 "\n", cs_get_u64(ctx, 46));

   if (tiler_flags.secondary_shader)
      pandecode_log("Varying allocation: %u\n", cs_get_u32(ctx, 48));

   mali_ptr blend = cs_get_u64(ctx, 50);
   GENX(pandecode_blend_descs)(blend & ~7, blend & 7, 0, ctx->gpu_id);

   DUMP_ADDR(DEPTH_STENCIL, cs_get_u64(ctx, 52), "Depth/stencil");

   if (tiler_flags.index_type)
      pandecode_log("Indices: %" PRIx64 "\n", cs_get_u64(ctx, 54));

   DUMP_UNPACKED(PRIMITIVE_FLAGS, tiler_flags, "Primitive flags\n");
   DUMP_CL(DCD_FLAGS_0, &ctx->regs[57], "DCD Flags 0\n");
   DUMP_CL(DCD_FLAGS_1, &ctx->regs[58], "DCD Flags 1\n");
   DUMP_CL(PRIMITIVE_SIZE, &ctx->regs[60], "Primitive size\n");

   pandecode_indent--;
}

static void
pandecode_run_fragment(struct queue_ctx *ctx, struct MALI_CEU_RUN_FRAGMENT *I)
{
   pandecode_indent++;

   DUMP_CL(SCISSOR, &ctx->regs[42], "Scissor\n");

   /* TODO: Tile enable map */
   GENX(pandecode_fbd)(cs_get_u64(ctx, 40), true, ctx->gpu_id);

   pandecode_indent--;
}

static void
print_indirect(unsigned address, int16_t offset, FILE *fp)
{
   if (offset)
      fprintf(fp, "[d%u + %d]", address, offset);
   else
      fprintf(fp, "[d%u]", address);
}

static void
print_reg_tuple(unsigned base, uint16_t mask, FILE *fp)
{
   bool first_reg = true;

   u_foreach_bit(i, mask) {
      fprintf(fp, "%sr%u", first_reg ? "" : ":", base + i);
      first_reg = false;
   }

   if (mask == 0)
      fprintf(fp, "_");
}

static void
disassemble_ceu_instr(uint64_t dword, unsigned indent, bool verbose, FILE *fp,
                      struct queue_ctx *ctx)
{
   if (verbose) {
      fprintf(fp, " ");
      for (unsigned b = 0; b < 8; ++b)
         fprintf(fp, " %02x", (uint8_t)(dword >> (8 * b)));
   }

   for (int i = 0; i < indent; ++i)
      fprintf(fp, "  ");

   /* Unpack the base so we get the opcode */
   uint8_t *bytes = (uint8_t *)&dword;
   pan_unpack(bytes, CEU_BASE, base);

   switch (base.opcode) {
   case MALI_CEU_OPCODE_NOP: {
      pan_unpack(bytes, CEU_NOP, I);

      if (I.ignored)
         fprintf(fp, "NOP // 0x%" PRIX64 "\n", I.ignored);
      else
         fprintf(fp, "NOP\n");
      break;
   }

   case MALI_CEU_OPCODE_MOVE: {
      pan_unpack(bytes, CEU_MOVE, I);

      fprintf(fp, "MOVE d%u, #0x%" PRIX64 "\n", I.destination, I.immediate);
      break;
   }

   case MALI_CEU_OPCODE_MOVE32: {
      pan_unpack(bytes, CEU_MOVE32, I);
      fprintf(fp, "MOVE32 r%u, #0x%X\n", I.destination, I.immediate);
      break;
   }

   case MALI_CEU_OPCODE_WAIT: {
      bool first = true;
      pan_unpack(bytes, CEU_WAIT, I);
      fprintf(fp, "WAIT ");

      u_foreach_bit(i, I.slots) {
         fprintf(fp, "%s%u", first ? "" : ",", i);
         first = false;
      }

      fprintf(fp, "\n");
      break;
   }

   case MALI_CEU_OPCODE_RUN_COMPUTE: {
      pan_unpack(bytes, CEU_RUN_COMPUTE, I);
      pandecode_run_compute(fp, ctx, &I);
      break;
   }

   case MALI_CEU_OPCODE_RUN_IDVS: {
      pan_unpack(bytes, CEU_RUN_IDVS, I);
      pandecode_run_idvs(fp, ctx, &I);
      break;
   }

   case MALI_CEU_OPCODE_RUN_FRAGMENT: {
      pan_unpack(bytes, CEU_RUN_FRAGMENT, I);
      fprintf(fp, "RUN_FRAGMENT%s\n",
              I.enable_tem ? ".tile_enable_map_enable" : "");
      pandecode_run_fragment(ctx, &I);
      break;
   }

   case MALI_CEU_OPCODE_ADD_IMMEDIATE32: {
      pan_unpack(bytes, CEU_ADD_IMMEDIATE32, I);

      fprintf(fp, "ADD_IMMEDIATE32 r%u, r%u, #%d\n", I.destination, I.source,
              I.immediate);
      break;
   }

   case MALI_CEU_OPCODE_ADD_IMMEDIATE64: {
      pan_unpack(bytes, CEU_ADD_IMMEDIATE64, I);

      fprintf(fp, "ADD_IMMEDIATE64 d%u, d%u, #%d\n", I.destination, I.source,
              I.immediate);
      break;
   }

   case MALI_CEU_OPCODE_LOAD_MULTIPLE: {
      pan_unpack(bytes, CEU_LOAD_MULTIPLE, I);

      fprintf(fp, "LOAD_MULTIPLE ");
      print_reg_tuple(I.base, I.mask, fp);
      fprintf(fp, ", ");
      print_indirect(I.address, I.offset, fp);
      fprintf(fp, "\n");
      break;
   }

   case MALI_CEU_OPCODE_STORE_MULTIPLE: {
      pan_unpack(bytes, CEU_STORE_MULTIPLE, I);

      fprintf(fp, "STORE_MULTIPLE ");
      print_indirect(I.address, I.offset, fp);
      fprintf(fp, ", ");
      print_reg_tuple(I.base, I.mask, fp);
      fprintf(fp, "\n");
      break;
   }

   case MALI_CEU_OPCODE_SET_SB_ENTRY: {
      pan_unpack(bytes, CEU_SET_SB_ENTRY, I);

      fprintf(fp, "SET_SB_ENTRY #%u, #%u\n", I.endpoint_entry, I.other_entry);
      break;
   }

   case MALI_CEU_OPCODE_SYNC_ADD32: {
      pan_unpack(bytes, CEU_SYNC_ADD32, I);
      bool first = true;
      fprintf(fp, "SYNC_ADD32%s%s signal(%u), wait(",
              I.error_propagate ? ".error_propagate" : "",
              I.scope_csg ? ".csg" : ".system", I.scoreboard_slot);

      u_foreach_bit(i, I.wait_mask) {
         fprintf(fp, "%s%u", first ? "" : ",", i);
         first = false;
      }

      fprintf(fp, ") [d%u], r%u\n", I.address, I.data);
      break;
   }

   case MALI_CEU_OPCODE_SYNC_ADD64: {
      pan_unpack(bytes, CEU_SYNC_ADD64, I);
      bool first = true;
      fprintf(fp, "SYNC_ADD64%s%s signal(%u), wait(",
              I.error_propagate ? ".error_propagate" : "",
              I.scope_csg ? ".csg" : ".system", I.scoreboard_slot);

      u_foreach_bit(i, I.wait_mask) {
         fprintf(fp, "%s%u", first ? "" : ",", i);
         first = false;
      }

      fprintf(fp, ") [d%u], d%u\n", I.address, I.data);
      break;
   }

   case MALI_CEU_OPCODE_SYNC_SET32: {
      pan_unpack(bytes, CEU_SYNC_SET32, I);
      bool first = true;
      fprintf(fp, "SYNC_SET32.%s%s signal(%u), wait(",
              I.error_propagate ? ".error_propagate" : "",
              I.scope_csg ? ".csg" : ".system", I.scoreboard_slot);

      u_foreach_bit(i, I.wait_mask) {
         fprintf(fp, "%s%u", first ? "" : ",", i);
         first = false;
      }

      fprintf(fp, ") [d%u], r%u\n", I.address, I.data);
      break;
   }

   case MALI_CEU_OPCODE_SYNC_SET64: {
      pan_unpack(bytes, CEU_SYNC_SET64, I);
      bool first = true;
      fprintf(fp, "SYNC_SET64.%s%s signal(%u), wait(",
              I.error_propagate ? ".error_propagate" : "",
              I.scope_csg ? ".csg" : ".system", I.scoreboard_slot);

      u_foreach_bit(i, I.wait_mask) {
         fprintf(fp, "%s%u", first ? "" : ",", i);
         first = false;
      }

      fprintf(fp, ") [d%u], d%u\n", I.address, I.data);
      break;
   }

   case MALI_CEU_OPCODE_CALL: {
      pan_unpack(bytes, CEU_CALL, I);
      fprintf(fp, "CALL d%u, r%u\n", I.address, I.length);
      break;
   }

   case MALI_CEU_OPCODE_JUMP: {
      pan_unpack(bytes, CEU_JUMP, I);
      fprintf(fp, "JUMP d%u, r%u\n", I.address, I.length);
      break;
   }

   case MALI_CEU_OPCODE_REQ_RESOURCE: {
      pan_unpack(bytes, CEU_REQ_RESOURCE, I);

      fprintf(fp, "REQ_RESOURCE");
      if (I.compute)
         fprintf(fp, ".compute");
      if (I.fragment)
         fprintf(fp, ".fragment");
      if (I.tiler)
         fprintf(fp, ".tiler");
      if (I.idvs)
         fprintf(fp, ".idvs");
      fprintf(fp, "\n");
      break;
   }

   case MALI_CEU_OPCODE_SYNC_WAIT32: {
      pan_unpack(bytes, CEU_SYNC_WAIT32, I);

      fprintf(fp, "SYNC_WAIT32%s%s d%u, r%u\n", I.invert ? ".gt" : ".le",
              I.error_reject ? ".reject" : ".inherit", I.address, I.data);
      break;
   }

   case MALI_CEU_OPCODE_SYNC_WAIT64: {
      pan_unpack(bytes, CEU_SYNC_WAIT64, I);

      fprintf(fp, "SYNC_WAIT64%s%s d%u, d%u\n", I.invert ? ".gt" : ".le",
              I.error_reject ? ".reject" : ".inherit", I.address, I.data);
      break;
   }

   case MALI_CEU_OPCODE_UMIN32: {
      pan_unpack(bytes, CEU_UMIN32, I);

      fprintf(fp, "UMIN32 r%u, r%u, r%u\n", I.destination, I.source_1,
              I.source_2);
      break;
   }

   case MALI_CEU_OPCODE_BRANCH: {
      pan_unpack(bytes, CEU_BRANCH, I);

      static const char *condition[] = {
         "le", "gt", "eq", "ne", "lt", "ge", "always",
      };
      fprintf(fp, "BRANCH.%s r%u, #%d\n", condition[I.condition], I.value,
              I.offset);

      break;
   }

   case MALI_CEU_OPCODE_FLUSH_CACHE2: {
      pan_unpack(bytes, CEU_FLUSH_CACHE2, I);
      static const char *mode[] = {
         "nop",
         "clean",
         "INVALID",
         "clean_invalidate",
      };

      fprintf(fp, "FLUSH_CACHE2.%s_l2.%s_lsc%s r%u, signal(%u), wait(",
              mode[I.l2_flush_mode], mode[I.lsc_flush_mode],
              I.other_invalidate ? ".invalidate_other" : "", I.latest_flush_id,
              I.scoreboard_entry);

      bool first = true;
      u_foreach_bit(i, I.scoreboard_mask) {
         fprintf(fp, "%s%u", first ? "" : ",", i);
         first = false;
      }
      fprintf(fp, ")\n");
      break;
   }

   case MALI_CEU_OPCODE_FINISH_TILING: {
      pan_unpack(bytes, CEU_FINISH_TILING, I);
      fprintf(fp, "FINISH_TILING\n");
      break;
   }

   case MALI_CEU_OPCODE_FINISH_FRAGMENT: {
      pan_unpack(bytes, CEU_FINISH_FRAGMENT, I);

      bool first = true;
      fprintf(fp, "FINISH_FRAGMENT.%s, d%u, d%u, signal(%u), wait(",
              I.increment_fragment_completed ? ".frag_end" : "",
              I.last_heap_chunk, I.first_heap_chunk, I.scoreboard_entry);

      u_foreach_bit(i, I.wait_mask) {
         fprintf(fp, "%s%u", first ? "" : ",", i);
         first = false;
      }
      fprintf(fp, ")\n");
      break;
   }

   case MALI_CEU_OPCODE_HEAP_OPERATION: {
      pan_unpack(bytes, CEU_HEAP_OPERATION, I);
      const char *counter_names[] = {"vt_start", "vt_end", NULL, "frag_end"};
      bool first = true;
      fprintf(fp, "HEAP_OPERATION.%s signal(%u), wait(",
              counter_names[I.operation], I.scoreboard_entry);

      u_foreach_bit(i, I.wait_mask) {
         fprintf(fp, "%s%u", first ? "" : ",", i);
         first = false;
      }

      fprintf(fp, ")\n");
      break;
   }

   case MALI_CEU_OPCODE_HEAP_SET: {
      pan_unpack(bytes, CEU_HEAP_SET, I);
      fprintf(fp, "HEAP_SET d%u\n", I.address);
      break;
   }

   default: {
      fprintf(fp, "INVALID_%u 0x%" PRIX64 "\n", base.opcode, base.data);
      break;
   }
   }
}

static bool
interpret_ceu_jump(struct queue_ctx *ctx, uint64_t reg_address,
                   uint32_t reg_length)
{
   uint32_t address_lo = ctx->regs[reg_address];
   uint32_t address_hi = ctx->regs[reg_address + 1];
   uint32_t length = ctx->regs[reg_length];

   if (length % 8) {
      fprintf(stderr, "CS call alignment error\n");
      return false;
   }

   /* Map the entire subqueue now */
   uint64_t address = ((uint64_t)address_hi << 32) | address_lo;
   uint64_t *cs = pandecode_fetch_gpu_mem(address, length);

   ctx->ip = cs;
   ctx->end = cs + (length / 8);

   /* Skip the usual IP update */
   return true;
}

/*
 * Interpret a single instruction of the CEU, updating the register file,
 * instruction pointer, and call stack. Memory access and GPU controls are
 * ignored for now.
 *
 * Returns true if execution should continue.
 */
static bool
interpret_ceu_instr(struct queue_ctx *ctx)
{
   /* Unpack the base so we get the opcode */
   uint8_t *bytes = (uint8_t *)ctx->ip;
   pan_unpack(bytes, CEU_BASE, base);

   assert(ctx->ip < ctx->end);

   switch (base.opcode) {
   case MALI_CEU_OPCODE_MOVE: {
      pan_unpack(bytes, CEU_MOVE, I);

      ctx->regs[I.destination + 0] = (uint32_t)I.immediate;
      ctx->regs[I.destination + 1] = (uint32_t)(I.immediate >> 32);
      break;
   }

   case MALI_CEU_OPCODE_MOVE32: {
      pan_unpack(bytes, CEU_MOVE32, I);

      ctx->regs[I.destination] = I.immediate;
      break;
   }

   case MALI_CEU_OPCODE_ADD_IMMEDIATE32: {
      pan_unpack(bytes, CEU_ADD_IMMEDIATE32, I);

      ctx->regs[I.destination] = ctx->regs[I.source] + I.immediate;
      break;
   }

   case MALI_CEU_OPCODE_ADD_IMMEDIATE64: {
      pan_unpack(bytes, CEU_ADD_IMMEDIATE64, I);

      int64_t value =
         (ctx->regs[I.source] | ((int64_t)ctx->regs[I.source + 1] << 32)) +
         I.immediate;

      ctx->regs[I.destination] = value;
      ctx->regs[I.destination + 1] = value >> 32;
      break;
   }

   case MALI_CEU_OPCODE_CALL: {
      pan_unpack(bytes, CEU_CALL, I);

      if (ctx->call_stack_depth == MAX_CALL_STACK_DEPTH) {
         fprintf(stderr, "CS call stack overflow\n");
         return false;
      }

      assert(ctx->call_stack_depth < MAX_CALL_STACK_DEPTH);

      ctx->ip++;

      /* Note: tail calls are not optimized in the hardware. */
      assert(ctx->ip <= ctx->end);

      unsigned depth = ctx->call_stack_depth++;

      ctx->call_stack[depth].lr = ctx->ip;
      ctx->call_stack[depth].end = ctx->end;

      return interpret_ceu_jump(ctx, I.address, I.length);
   }

   case MALI_CEU_OPCODE_JUMP: {
      pan_unpack(bytes, CEU_CALL, I);

      if (ctx->call_stack_depth == 0) {
         fprintf(stderr, "Cannot jump from the entrypoint\n");
         return false;
      }

      return interpret_ceu_jump(ctx, I.address, I.length);
   }

   default:
      break;
   }

   /* Update IP first to point to the next instruction, so call doesn't
    * require special handling (even for tail calls).
    */
   ctx->ip++;

   while (ctx->ip == ctx->end) {
      /* Graceful termination */
      if (ctx->call_stack_depth == 0)
         return false;

      /* Pop off the call stack */
      unsigned old_depth = --ctx->call_stack_depth;

      ctx->ip = ctx->call_stack[old_depth].lr;
      ctx->end = ctx->call_stack[old_depth].end;
   }

   return true;
}

void
GENX(pandecode_cs)(mali_ptr queue, uint32_t size, unsigned gpu_id,
                   uint32_t *regs)
{
   pandecode_dump_file_open();

   uint64_t *cs = pandecode_fetch_gpu_mem(queue, size);

   struct queue_ctx ctx = {
      /* Mali-G610 has 96 registers. Other devices not yet supported, we can
       * make this configurable later when we encounter new Malis.
       */
      .nr_regs = 96,
      .regs = regs,
      .ip = cs,
      .end = cs + (size / 8),
      .gpu_id = gpu_id,
   };

   if (size) {
      do {
         disassemble_ceu_instr(*(ctx.ip), 1 + ctx.call_stack_depth, true,
                               pandecode_dump_stream, &ctx);
      } while (interpret_ceu_instr(&ctx));
   }

   fflush(pandecode_dump_stream);
   pandecode_map_read_write();
}
#endif
