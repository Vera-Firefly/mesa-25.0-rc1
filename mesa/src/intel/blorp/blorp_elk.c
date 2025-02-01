/*
 * Copyright 2012 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#include "blorp_priv.h"
#include "blorp_nir_builder.h"
#include "compiler/elk/elk_compiler.h"
#include "compiler/elk/elk_nir.h"
#include "compiler/intel_nir.h"
#include "dev/intel_debug.h"

static const nir_shader_compiler_options *
blorp_nir_options_elk(struct blorp_context *blorp,
                      gl_shader_stage stage)
{
   const struct elk_compiler *compiler = blorp->compiler->elk;
   return compiler->nir_options[stage];
}

static struct blorp_program
blorp_compile_fs_elk(struct blorp_context *blorp, void *mem_ctx,
                     struct nir_shader *nir,
                     bool multisample_fbo,
                     bool is_fast_clear,
                     bool use_repclear)
{
   const struct elk_compiler *compiler = blorp->compiler->elk;

   struct elk_wm_prog_data *wm_prog_data = rzalloc(mem_ctx, struct elk_wm_prog_data);
   wm_prog_data->base.nr_params = 0;
   wm_prog_data->base.param = NULL;

   struct elk_nir_compiler_opts opts = {};
   elk_preprocess_nir(compiler, nir, &opts);
   nir_remove_dead_variables(nir, nir_var_shader_in, NULL);
   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));

   struct elk_wm_prog_key wm_key;
   memset(&wm_key, 0, sizeof(wm_key));
   wm_key.multisample_fbo = multisample_fbo ? ELK_ALWAYS : ELK_NEVER;
   wm_key.nr_color_regions = 1;

   if (compiler->devinfo->ver < 6) {
      if (nir->info.fs.uses_discard)
         wm_key.iz_lookup |= ELK_WM_IZ_PS_KILL_ALPHATEST_BIT;

      wm_key.input_slots_valid = nir->info.inputs_read | VARYING_BIT_POS;
   }

   struct elk_compile_fs_params params = {
      .base = {
         .mem_ctx = mem_ctx,
         .nir = nir,
         .log_data = blorp->driver_ctx,
         .debug_flag = DEBUG_BLORP,
      },
      .key = &wm_key,
      .prog_data = wm_prog_data,

      .use_rep_send = use_repclear,
      .max_polygons = 1,
   };

   const unsigned *kernel = elk_compile_fs(compiler, &params);
   return (struct blorp_program){
      .kernel         = kernel,
      .kernel_size    = wm_prog_data->base.program_size,
      .prog_data      = wm_prog_data,
      .prog_data_size = sizeof(*wm_prog_data),
   };
}

static struct blorp_program
blorp_compile_vs_elk(struct blorp_context *blorp, void *mem_ctx,
                     struct nir_shader *nir)
{
   const struct elk_compiler *compiler = blorp->compiler->elk;

   struct elk_nir_compiler_opts opts = {};
   elk_preprocess_nir(compiler, nir, &opts);
   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));

   struct elk_vs_prog_data *vs_prog_data = rzalloc(mem_ctx, struct elk_vs_prog_data);
   vs_prog_data->inputs_read = nir->info.inputs_read;

   elk_compute_vue_map(compiler->devinfo,
                       &vs_prog_data->base.vue_map,
                       nir->info.outputs_written,
                       nir->info.separate_shader,
                       1);

   struct elk_vs_prog_key vs_key = { 0, };

   struct elk_compile_vs_params params = {
      .base = {
         .mem_ctx = mem_ctx,
         .nir = nir,
         .log_data = blorp->driver_ctx,
         .debug_flag = DEBUG_BLORP,
      },
      .key = &vs_key,
      .prog_data = vs_prog_data,
   };

   const unsigned *kernel = elk_compile_vs(compiler, &params);
   return (struct blorp_program) {
      .kernel         = kernel,
      .kernel_size    = vs_prog_data->base.base.program_size,
      .prog_data      = vs_prog_data,
      .prog_data_size = sizeof(*vs_prog_data),
   };
}

static bool
lower_base_workgroup_id(nir_builder *b, nir_intrinsic_instr *intrin,
                        UNUSED void *data)
{
   if (intrin->intrinsic != nir_intrinsic_load_base_workgroup_id)
      return false;

   b->cursor = nir_instr_remove(&intrin->instr);
   nir_def_rewrite_uses(&intrin->def, nir_imm_zero(b, 3, 32));
   return true;
}

static struct blorp_program
blorp_compile_cs_elk(struct blorp_context *blorp, void *mem_ctx,
                     struct nir_shader *nir)
{
   const struct elk_compiler *compiler = blorp->compiler->elk;

   struct elk_nir_compiler_opts opts = {};
   elk_preprocess_nir(compiler, nir, &opts);
   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));

   NIR_PASS_V(nir, nir_lower_io, nir_var_uniform, elk_type_size_scalar_bytes,
              (nir_lower_io_options)0);

   STATIC_ASSERT(offsetof(struct blorp_wm_inputs, subgroup_id) + 4 ==
                 sizeof(struct blorp_wm_inputs));
   nir->num_uniforms = offsetof(struct blorp_wm_inputs, subgroup_id);
   unsigned nr_params = nir->num_uniforms / 4;

   struct elk_cs_prog_data *cs_prog_data = rzalloc(mem_ctx, struct elk_cs_prog_data);
   cs_prog_data->base.nr_params = nr_params;
   cs_prog_data->base.param = rzalloc_array(NULL, uint32_t, nr_params);

   NIR_PASS_V(nir, elk_nir_lower_cs_intrinsics, compiler->devinfo,
              cs_prog_data);
   NIR_PASS_V(nir, nir_shader_intrinsics_pass, lower_base_workgroup_id,
              nir_metadata_control_flow, NULL);

   struct elk_cs_prog_key cs_key;
   memset(&cs_key, 0, sizeof(cs_key));

   struct elk_compile_cs_params params = {
      .base = {
         .mem_ctx = mem_ctx,
         .nir = nir,
         .log_data = blorp->driver_ctx,
         .debug_flag = DEBUG_BLORP,
      },
      .key = &cs_key,
      .prog_data = cs_prog_data,
   };

   const unsigned *kernel = elk_compile_cs(compiler, &params);

   ralloc_free(cs_prog_data->base.param);
   cs_prog_data->base.param = NULL;

   return (struct blorp_program) {
      .kernel         = kernel,
      .kernel_size    = cs_prog_data->base.program_size,
      .prog_data      = cs_prog_data,
      .prog_data_size = sizeof(*cs_prog_data),
   };
}

struct blorp_sf_key {
   struct blorp_base_key base;
   struct elk_sf_prog_key key;
};

static bool
blorp_ensure_sf_program_elk(struct blorp_batch *batch,
                            struct blorp_params *params)
{
   struct blorp_context *blorp = batch->blorp;
   const struct elk_compiler *compiler = blorp->compiler->elk;
   const struct elk_wm_prog_data *wm_prog_data = params->wm_prog_data;
   assert(params->wm_prog_data);

   /* Gfx6+ doesn't need a strips and fans program */
   if (compiler->devinfo->ver >= 6)
      return true;

   struct blorp_sf_key key = {
      .base = BLORP_BASE_KEY_INIT(BLORP_SHADER_TYPE_GFX4_SF),
   };

   /* Everything gets compacted in vertex setup, so we just need a
    * pass-through for the correct number of input varyings.
    */
   const uint64_t slots_valid = VARYING_BIT_POS |
      ((1ull << wm_prog_data->num_varying_inputs) - 1) << VARYING_SLOT_VAR0;

   key.key.attrs = slots_valid;
   key.key.primitive = ELK_SF_PRIM_TRIANGLES;
   key.key.contains_flat_varying = wm_prog_data->contains_flat_varying;

   STATIC_ASSERT(sizeof(key.key.interp_mode) ==
                 sizeof(wm_prog_data->interp_mode));
   memcpy(key.key.interp_mode, wm_prog_data->interp_mode,
          sizeof(key.key.interp_mode));

   if (blorp->lookup_shader(batch, &key, sizeof(key),
                            &params->sf_prog_kernel, &params->sf_prog_data))
      return true;

   void *mem_ctx = ralloc_context(NULL);

   const unsigned *program;
   unsigned program_size;

   struct intel_vue_map vue_map;
   elk_compute_vue_map(compiler->devinfo, &vue_map, slots_valid, false, 1);

   struct elk_sf_prog_data prog_data_tmp;
   program = elk_compile_sf(compiler, mem_ctx, &key.key,
                            &prog_data_tmp, &vue_map, &program_size);

   bool result =
      blorp->upload_shader(batch, MESA_SHADER_NONE,
                           &key, sizeof(key), program, program_size,
                           (void *)&prog_data_tmp, sizeof(prog_data_tmp),
                           &params->sf_prog_kernel, &params->sf_prog_data);

   ralloc_free(mem_ctx);

   return result;
}

#pragma pack(push, 1)
struct layer_offset_vs_key {
   struct blorp_base_key base;
   unsigned num_inputs;
};
#pragma pack(pop)

/* In the case of doing attachment clears, we are using a surface state that
 * is handed to us so we can't set (and don't even know) the base array layer.
 * In order to do a layered clear in this scenario, we need some way of adding
 * the base array layer to the instance id.  Unfortunately, our hardware has
 * no real concept of "base instance", so we have to do it manually in a
 * vertex shader.
 */
static bool
blorp_params_get_layer_offset_vs_elk(struct blorp_batch *batch,
                                     struct blorp_params *params)
{
   struct blorp_context *blorp = batch->blorp;
   struct layer_offset_vs_key blorp_key = {
      .base = BLORP_BASE_KEY_INIT(BLORP_SHADER_TYPE_LAYER_OFFSET_VS),
   };

   struct elk_wm_prog_data *wm_prog_data = params->wm_prog_data;
   if (wm_prog_data)
      blorp_key.num_inputs = wm_prog_data->num_varying_inputs;

   if (blorp->lookup_shader(batch, &blorp_key, sizeof(blorp_key),
                            &params->vs_prog_kernel, &params->vs_prog_data))
      return true;

   void *mem_ctx = ralloc_context(NULL);

   nir_builder b;
   blorp_nir_init_shader(&b, blorp, mem_ctx, MESA_SHADER_VERTEX,
                         blorp_shader_type_to_name(blorp_key.base.shader_type));

   const struct glsl_type *uvec4_type = glsl_vector_type(GLSL_TYPE_UINT, 4);

   /* First we deal with the header which has instance and base instance */
   nir_variable *a_header = nir_variable_create(b.shader, nir_var_shader_in,
                                                uvec4_type, "header");
   a_header->data.location = VERT_ATTRIB_GENERIC0;

   nir_variable *v_layer = nir_variable_create(b.shader, nir_var_shader_out,
                                               glsl_int_type(), "layer_id");
   v_layer->data.location = VARYING_SLOT_LAYER;

   /* Compute the layer id */
   nir_def *header = nir_load_var(&b, a_header);
   nir_def *base_layer = nir_channel(&b, header, 0);
   nir_def *instance = nir_channel(&b, header, 1);
   nir_store_var(&b, v_layer, nir_iadd(&b, instance, base_layer), 0x1);

   /* Then we copy the vertex from the next slot to VARYING_SLOT_POS */
   nir_variable *a_vertex = nir_variable_create(b.shader, nir_var_shader_in,
                                                glsl_vec4_type(), "a_vertex");
   a_vertex->data.location = VERT_ATTRIB_GENERIC1;

   nir_variable *v_pos = nir_variable_create(b.shader, nir_var_shader_out,
                                             glsl_vec4_type(), "v_pos");
   v_pos->data.location = VARYING_SLOT_POS;

   nir_copy_var(&b, v_pos, a_vertex);

   /* Then we copy everything else */
   for (unsigned i = 0; i < blorp_key.num_inputs; i++) {
      nir_variable *a_in = nir_variable_create(b.shader, nir_var_shader_in,
                                               uvec4_type, "input");
      a_in->data.location = VERT_ATTRIB_GENERIC2 + i;

      nir_variable *v_out = nir_variable_create(b.shader, nir_var_shader_out,
                                                uvec4_type, "output");
      v_out->data.location = VARYING_SLOT_VAR0 + i;

      nir_copy_var(&b, v_out, a_in);
   }

   const struct blorp_program p =
      blorp_compile_vs(blorp, mem_ctx, b.shader);

   bool result =
      blorp->upload_shader(batch, MESA_SHADER_VERTEX,
                           &blorp_key, sizeof(blorp_key),
                           p.kernel, p.kernel_size,
                           p.prog_data, p.prog_data_size,
                           &params->vs_prog_kernel, &params->vs_prog_data);

   ralloc_free(mem_ctx);
   return result;
}

void
blorp_init_elk(struct blorp_context *blorp, void *driver_ctx,
               struct isl_device *isl_dev, const struct elk_compiler *elk,
               const struct blorp_config *config)
{
   blorp_init(blorp, driver_ctx, isl_dev, config);
   assert(elk);

   blorp->compiler->elk = elk;
   blorp->compiler->nir_options = blorp_nir_options_elk;
   blorp->compiler->compile_fs = blorp_compile_fs_elk;
   blorp->compiler->compile_vs = blorp_compile_vs_elk;
   blorp->compiler->compile_cs = blorp_compile_cs_elk;
   blorp->compiler->ensure_sf_program = blorp_ensure_sf_program_elk;
   blorp->compiler->params_get_layer_offset_vs =
      blorp_params_get_layer_offset_vs_elk;
}
