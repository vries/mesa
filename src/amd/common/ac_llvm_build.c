/*
 * Copyright 2014 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 */
/* based on pieces from si_pipe.c and radeon_llvm_emit.c */
#include "ac_llvm_build.h"

#include <llvm-c/Core.h>

#include "c11/threads.h"

#include <assert.h>
#include <stdio.h>

#include "ac_llvm_util.h"
#include "ac_exp_param.h"
#include "util/bitscan.h"
#include "util/macros.h"
#include "sid.h"

#include "shader_enums.h"

/* Initialize module-independent parts of the context.
 *
 * The caller is responsible for initializing ctx::module and ctx::builder.
 */
void
ac_llvm_context_init(struct ac_llvm_context *ctx, LLVMContextRef context)
{
	LLVMValueRef args[1];

	ctx->context = context;
	ctx->module = NULL;
	ctx->builder = NULL;

	ctx->voidt = LLVMVoidTypeInContext(ctx->context);
	ctx->i1 = LLVMInt1TypeInContext(ctx->context);
	ctx->i8 = LLVMInt8TypeInContext(ctx->context);
	ctx->i32 = LLVMIntTypeInContext(ctx->context, 32);
	ctx->f32 = LLVMFloatTypeInContext(ctx->context);
	ctx->v4i32 = LLVMVectorType(ctx->i32, 4);
	ctx->v4f32 = LLVMVectorType(ctx->f32, 4);
	ctx->v16i8 = LLVMVectorType(ctx->i8, 16);

	ctx->range_md_kind = LLVMGetMDKindIDInContext(ctx->context,
						     "range", 5);

	ctx->invariant_load_md_kind = LLVMGetMDKindIDInContext(ctx->context,
							       "invariant.load", 14);

	ctx->fpmath_md_kind = LLVMGetMDKindIDInContext(ctx->context, "fpmath", 6);

	args[0] = LLVMConstReal(ctx->f32, 2.5);
	ctx->fpmath_md_2p5_ulp = LLVMMDNodeInContext(ctx->context, args, 1);

	ctx->uniform_md_kind = LLVMGetMDKindIDInContext(ctx->context,
							"amdgpu.uniform", 14);

	ctx->empty_md = LLVMMDNodeInContext(ctx->context, NULL, 0);
}

LLVMValueRef
ac_build_intrinsic(struct ac_llvm_context *ctx, const char *name,
		   LLVMTypeRef return_type, LLVMValueRef *params,
		   unsigned param_count, unsigned attrib_mask)
{
	LLVMValueRef function, call;
	bool set_callsite_attrs = HAVE_LLVM >= 0x0400 &&
				  !(attrib_mask & AC_FUNC_ATTR_LEGACY);

	function = LLVMGetNamedFunction(ctx->module, name);
	if (!function) {
		LLVMTypeRef param_types[32], function_type;
		unsigned i;

		assert(param_count <= 32);

		for (i = 0; i < param_count; ++i) {
			assert(params[i]);
			param_types[i] = LLVMTypeOf(params[i]);
		}
		function_type =
		    LLVMFunctionType(return_type, param_types, param_count, 0);
		function = LLVMAddFunction(ctx->module, name, function_type);

		LLVMSetFunctionCallConv(function, LLVMCCallConv);
		LLVMSetLinkage(function, LLVMExternalLinkage);

		if (!set_callsite_attrs)
			ac_add_func_attributes(ctx->context, function, attrib_mask);
	}

	call = LLVMBuildCall(ctx->builder, function, params, param_count, "");
	if (set_callsite_attrs)
		ac_add_func_attributes(ctx->context, call, attrib_mask);
	return call;
}

static LLVMValueRef bitcast_to_float(struct ac_llvm_context *ctx,
				     LLVMValueRef value)
{
	LLVMTypeRef type = LLVMTypeOf(value);
	LLVMTypeRef new_type;

	if (LLVMGetTypeKind(type) == LLVMVectorTypeKind)
		new_type = LLVMVectorType(ctx->f32, LLVMGetVectorSize(type));
	else
		new_type = ctx->f32;

	return LLVMBuildBitCast(ctx->builder, value, new_type, "");
}

/**
 * Given the i32 or vNi32 \p type, generate the textual name (e.g. for use with
 * intrinsic names).
 */
void ac_build_type_name_for_intr(LLVMTypeRef type, char *buf, unsigned bufsize)
{
	LLVMTypeRef elem_type = type;

	assert(bufsize >= 8);

	if (LLVMGetTypeKind(type) == LLVMVectorTypeKind) {
		int ret = snprintf(buf, bufsize, "v%u",
					LLVMGetVectorSize(type));
		if (ret < 0) {
			char *type_name = LLVMPrintTypeToString(type);
			fprintf(stderr, "Error building type name for: %s\n",
				type_name);
			return;
		}
		elem_type = LLVMGetElementType(type);
		buf += ret;
		bufsize -= ret;
	}
	switch (LLVMGetTypeKind(elem_type)) {
	default: break;
	case LLVMIntegerTypeKind:
		snprintf(buf, bufsize, "i%d", LLVMGetIntTypeWidth(elem_type));
		break;
	case LLVMFloatTypeKind:
		snprintf(buf, bufsize, "f32");
		break;
	case LLVMDoubleTypeKind:
		snprintf(buf, bufsize, "f64");
		break;
	}
}

LLVMValueRef
ac_build_gather_values_extended(struct ac_llvm_context *ctx,
				LLVMValueRef *values,
				unsigned value_count,
				unsigned value_stride,
				bool load)
{
	LLVMBuilderRef builder = ctx->builder;
	LLVMValueRef vec = NULL;
	unsigned i;

	if (value_count == 1) {
		if (load)
			return LLVMBuildLoad(builder, values[0], "");
		return values[0];
	} else if (!value_count)
		unreachable("value_count is 0");

	for (i = 0; i < value_count; i++) {
		LLVMValueRef value = values[i * value_stride];
		if (load)
			value = LLVMBuildLoad(builder, value, "");

		if (!i)
			vec = LLVMGetUndef( LLVMVectorType(LLVMTypeOf(value), value_count));
		LLVMValueRef index = LLVMConstInt(ctx->i32, i, false);
		vec = LLVMBuildInsertElement(builder, vec, value, index, "");
	}
	return vec;
}

LLVMValueRef
ac_build_gather_values(struct ac_llvm_context *ctx,
		       LLVMValueRef *values,
		       unsigned value_count)
{
	return ac_build_gather_values_extended(ctx, values, value_count, 1, false);
}

LLVMValueRef
ac_build_fdiv(struct ac_llvm_context *ctx,
	      LLVMValueRef num,
	      LLVMValueRef den)
{
	LLVMValueRef ret = LLVMBuildFDiv(ctx->builder, num, den, "");

	if (!LLVMIsConstant(ret))
		LLVMSetMetadata(ret, ctx->fpmath_md_kind, ctx->fpmath_md_2p5_ulp);
	return ret;
}

/* Coordinates for cube map selection. sc, tc, and ma are as in Table 8.27
 * of the OpenGL 4.5 (Compatibility Profile) specification, except ma is
 * already multiplied by two. id is the cube face number.
 */
struct cube_selection_coords {
	LLVMValueRef stc[2];
	LLVMValueRef ma;
	LLVMValueRef id;
};

static void
build_cube_intrinsic(struct ac_llvm_context *ctx,
		     LLVMValueRef in[3],
		     struct cube_selection_coords *out)
{
	LLVMTypeRef f32 = ctx->f32;

	out->stc[1] = ac_build_intrinsic(ctx, "llvm.amdgcn.cubetc",
					 f32, in, 3, AC_FUNC_ATTR_READNONE);
	out->stc[0] = ac_build_intrinsic(ctx, "llvm.amdgcn.cubesc",
					 f32, in, 3, AC_FUNC_ATTR_READNONE);
	out->ma = ac_build_intrinsic(ctx, "llvm.amdgcn.cubema",
				     f32, in, 3, AC_FUNC_ATTR_READNONE);
	out->id = ac_build_intrinsic(ctx, "llvm.amdgcn.cubeid",
				     f32, in, 3, AC_FUNC_ATTR_READNONE);
}

/**
 * Build a manual selection sequence for cube face sc/tc coordinates and
 * major axis vector (multiplied by 2 for consistency) for the given
 * vec3 \p coords, for the face implied by \p selcoords.
 *
 * For the major axis, we always adjust the sign to be in the direction of
 * selcoords.ma; i.e., a positive out_ma means that coords is pointed towards
 * the selcoords major axis.
 */
static void build_cube_select(LLVMBuilderRef builder,
			      const struct cube_selection_coords *selcoords,
			      const LLVMValueRef *coords,
			      LLVMValueRef *out_st,
			      LLVMValueRef *out_ma)
{
	LLVMTypeRef f32 = LLVMTypeOf(coords[0]);
	LLVMValueRef is_ma_positive;
	LLVMValueRef sgn_ma;
	LLVMValueRef is_ma_z, is_not_ma_z;
	LLVMValueRef is_ma_y;
	LLVMValueRef is_ma_x;
	LLVMValueRef sgn;
	LLVMValueRef tmp;

	is_ma_positive = LLVMBuildFCmp(builder, LLVMRealUGE,
		selcoords->ma, LLVMConstReal(f32, 0.0), "");
	sgn_ma = LLVMBuildSelect(builder, is_ma_positive,
		LLVMConstReal(f32, 1.0), LLVMConstReal(f32, -1.0), "");

	is_ma_z = LLVMBuildFCmp(builder, LLVMRealUGE, selcoords->id, LLVMConstReal(f32, 4.0), "");
	is_not_ma_z = LLVMBuildNot(builder, is_ma_z, "");
	is_ma_y = LLVMBuildAnd(builder, is_not_ma_z,
		LLVMBuildFCmp(builder, LLVMRealUGE, selcoords->id, LLVMConstReal(f32, 2.0), ""), "");
	is_ma_x = LLVMBuildAnd(builder, is_not_ma_z, LLVMBuildNot(builder, is_ma_y, ""), "");

	/* Select sc */
	tmp = LLVMBuildSelect(builder, is_ma_z, coords[2], coords[0], "");
	sgn = LLVMBuildSelect(builder, is_ma_y, LLVMConstReal(f32, 1.0),
		LLVMBuildSelect(builder, is_ma_x, sgn_ma,
			LLVMBuildFNeg(builder, sgn_ma, ""), ""), "");
	out_st[0] = LLVMBuildFMul(builder, tmp, sgn, "");

	/* Select tc */
	tmp = LLVMBuildSelect(builder, is_ma_y, coords[2], coords[1], "");
	sgn = LLVMBuildSelect(builder, is_ma_y, LLVMBuildFNeg(builder, sgn_ma, ""),
		LLVMConstReal(f32, -1.0), "");
	out_st[1] = LLVMBuildFMul(builder, tmp, sgn, "");

	/* Select ma */
	tmp = LLVMBuildSelect(builder, is_ma_z, coords[2],
		LLVMBuildSelect(builder, is_ma_y, coords[1], coords[0], ""), "");
	sgn = LLVMBuildSelect(builder, is_ma_positive,
		LLVMConstReal(f32, 2.0), LLVMConstReal(f32, -2.0), "");
	*out_ma = LLVMBuildFMul(builder, tmp, sgn, "");
}

void
ac_prepare_cube_coords(struct ac_llvm_context *ctx,
		       bool is_deriv, bool is_array,
		       LLVMValueRef *coords_arg,
		       LLVMValueRef *derivs_arg)
{

	LLVMBuilderRef builder = ctx->builder;
	struct cube_selection_coords selcoords;
	LLVMValueRef coords[3];
	LLVMValueRef invma;

	build_cube_intrinsic(ctx, coords_arg, &selcoords);

	invma = ac_build_intrinsic(ctx, "llvm.fabs.f32",
			ctx->f32, &selcoords.ma, 1, AC_FUNC_ATTR_READNONE);
	invma = ac_build_fdiv(ctx, LLVMConstReal(ctx->f32, 1.0), invma);

	for (int i = 0; i < 2; ++i)
		coords[i] = LLVMBuildFMul(builder, selcoords.stc[i], invma, "");

	coords[2] = selcoords.id;

	if (is_deriv && derivs_arg) {
		LLVMValueRef derivs[4];
		int axis;

		/* Convert cube derivatives to 2D derivatives. */
		for (axis = 0; axis < 2; axis++) {
			LLVMValueRef deriv_st[2];
			LLVMValueRef deriv_ma;

			/* Transform the derivative alongside the texture
			 * coordinate. Mathematically, the correct formula is
			 * as follows. Assume we're projecting onto the +Z face
			 * and denote by dx/dh the derivative of the (original)
			 * X texture coordinate with respect to horizontal
			 * window coordinates. The projection onto the +Z face
			 * plane is:
			 *
			 *   f(x,z) = x/z
			 *
			 * Then df/dh = df/dx * dx/dh + df/dz * dz/dh
			 *            = 1/z * dx/dh - x/z * 1/z * dz/dh.
			 *
			 * This motivatives the implementation below.
			 *
			 * Whether this actually gives the expected results for
			 * apps that might feed in derivatives obtained via
			 * finite differences is anyone's guess. The OpenGL spec
			 * seems awfully quiet about how textureGrad for cube
			 * maps should be handled.
			 */
			build_cube_select(builder, &selcoords, &derivs_arg[axis * 3],
					  deriv_st, &deriv_ma);

			deriv_ma = LLVMBuildFMul(builder, deriv_ma, invma, "");

			for (int i = 0; i < 2; ++i)
				derivs[axis * 2 + i] =
					LLVMBuildFSub(builder,
						LLVMBuildFMul(builder, deriv_st[i], invma, ""),
						LLVMBuildFMul(builder, deriv_ma, coords[i], ""), "");
		}

		memcpy(derivs_arg, derivs, sizeof(derivs));
	}

	/* Shift the texture coordinate. This must be applied after the
	 * derivative calculation.
	 */
	for (int i = 0; i < 2; ++i)
		coords[i] = LLVMBuildFAdd(builder, coords[i], LLVMConstReal(ctx->f32, 1.5), "");

	if (is_array) {
		/* for cube arrays coord.z = coord.w(array_index) * 8 + face */
		/* coords_arg.w component - array_index for cube arrays */
		LLVMValueRef tmp = LLVMBuildFMul(ctx->builder, coords_arg[3], LLVMConstReal(ctx->f32, 8.0), "");
		coords[2] = LLVMBuildFAdd(ctx->builder, tmp, coords[2], "");
	}

	memcpy(coords_arg, coords, sizeof(coords));
}


LLVMValueRef
ac_build_fs_interp(struct ac_llvm_context *ctx,
		   LLVMValueRef llvm_chan,
		   LLVMValueRef attr_number,
		   LLVMValueRef params,
		   LLVMValueRef i,
		   LLVMValueRef j)
{
	LLVMValueRef args[5];
	LLVMValueRef p1;
	
	if (HAVE_LLVM < 0x0400) {
		LLVMValueRef ij[2];
		ij[0] = LLVMBuildBitCast(ctx->builder, i, ctx->i32, "");
		ij[1] = LLVMBuildBitCast(ctx->builder, j, ctx->i32, "");

		args[0] = llvm_chan;
		args[1] = attr_number;
		args[2] = params;
		args[3] = ac_build_gather_values(ctx, ij, 2);
		return ac_build_intrinsic(ctx, "llvm.SI.fs.interp",
					  ctx->f32, args, 4,
					  AC_FUNC_ATTR_READNONE);
	}

	args[0] = i;
	args[1] = llvm_chan;
	args[2] = attr_number;
	args[3] = params;

	p1 = ac_build_intrinsic(ctx, "llvm.amdgcn.interp.p1",
				ctx->f32, args, 4, AC_FUNC_ATTR_READNONE);

	args[0] = p1;
	args[1] = j;
	args[2] = llvm_chan;
	args[3] = attr_number;
	args[4] = params;

	return ac_build_intrinsic(ctx, "llvm.amdgcn.interp.p2",
				  ctx->f32, args, 5, AC_FUNC_ATTR_READNONE);
}

LLVMValueRef
ac_build_fs_interp_mov(struct ac_llvm_context *ctx,
		       LLVMValueRef parameter,
		       LLVMValueRef llvm_chan,
		       LLVMValueRef attr_number,
		       LLVMValueRef params)
{
	LLVMValueRef args[4];
	if (HAVE_LLVM < 0x0400) {
		args[0] = llvm_chan;
		args[1] = attr_number;
		args[2] = params;

		return ac_build_intrinsic(ctx,
					  "llvm.SI.fs.constant",
					  ctx->f32, args, 3,
					  AC_FUNC_ATTR_READNONE);
	}

	args[0] = parameter;
	args[1] = llvm_chan;
	args[2] = attr_number;
	args[3] = params;

	return ac_build_intrinsic(ctx, "llvm.amdgcn.interp.mov",
				  ctx->f32, args, 4, AC_FUNC_ATTR_READNONE);
}

LLVMValueRef
ac_build_gep0(struct ac_llvm_context *ctx,
	      LLVMValueRef base_ptr,
	      LLVMValueRef index)
{
	LLVMValueRef indices[2] = {
		LLVMConstInt(ctx->i32, 0, 0),
		index,
	};
	return LLVMBuildGEP(ctx->builder, base_ptr,
			    indices, 2, "");
}

void
ac_build_indexed_store(struct ac_llvm_context *ctx,
		       LLVMValueRef base_ptr, LLVMValueRef index,
		       LLVMValueRef value)
{
	LLVMBuildStore(ctx->builder, value,
		       ac_build_gep0(ctx, base_ptr, index));
}

/**
 * Build an LLVM bytecode indexed load using LLVMBuildGEP + LLVMBuildLoad.
 * It's equivalent to doing a load from &base_ptr[index].
 *
 * \param base_ptr  Where the array starts.
 * \param index     The element index into the array.
 * \param uniform   Whether the base_ptr and index can be assumed to be
 *                  dynamically uniform
 */
LLVMValueRef
ac_build_indexed_load(struct ac_llvm_context *ctx,
		      LLVMValueRef base_ptr, LLVMValueRef index,
		      bool uniform)
{
	LLVMValueRef pointer;

	pointer = ac_build_gep0(ctx, base_ptr, index);
	if (uniform)
		LLVMSetMetadata(pointer, ctx->uniform_md_kind, ctx->empty_md);
	return LLVMBuildLoad(ctx->builder, pointer, "");
}

/**
 * Do a load from &base_ptr[index], but also add a flag that it's loading
 * a constant from a dynamically uniform index.
 */
LLVMValueRef
ac_build_indexed_load_const(struct ac_llvm_context *ctx,
			    LLVMValueRef base_ptr, LLVMValueRef index)
{
	LLVMValueRef result = ac_build_indexed_load(ctx, base_ptr, index, true);
	LLVMSetMetadata(result, ctx->invariant_load_md_kind, ctx->empty_md);
	return result;
}

/* TBUFFER_STORE_FORMAT_{X,XY,XYZ,XYZW} <- the suffix is selected by num_channels=1..4.
 * The type of vdata must be one of i32 (num_channels=1), v2i32 (num_channels=2),
 * or v4i32 (num_channels=3,4).
 */
void
ac_build_buffer_store_dword(struct ac_llvm_context *ctx,
			    LLVMValueRef rsrc,
			    LLVMValueRef vdata,
			    unsigned num_channels,
			    LLVMValueRef voffset,
			    LLVMValueRef soffset,
			    unsigned inst_offset,
			    bool glc,
			    bool slc,
			    bool writeonly_memory,
			    bool has_add_tid)
{
	/* TODO: Fix stores with ADD_TID and remove the "has_add_tid" flag. */
	if (!has_add_tid) {
		/* Split 3 channel stores, becase LLVM doesn't support 3-channel
		 * intrinsics. */
		if (num_channels == 3) {
			LLVMValueRef v[3], v01;

			for (int i = 0; i < 3; i++) {
				v[i] = LLVMBuildExtractElement(ctx->builder, vdata,
						LLVMConstInt(ctx->i32, i, 0), "");
			}
			v01 = ac_build_gather_values(ctx, v, 2);

			ac_build_buffer_store_dword(ctx, rsrc, v01, 2, voffset,
						    soffset, inst_offset, glc, slc,
						    writeonly_memory, has_add_tid);
			ac_build_buffer_store_dword(ctx, rsrc, v[2], 1, voffset,
						    soffset, inst_offset + 8,
						    glc, slc,
						    writeonly_memory, has_add_tid);
			return;
		}

		unsigned func = CLAMP(num_channels, 1, 3) - 1;
		static const char *types[] = {"f32", "v2f32", "v4f32"};
		char name[256];
		LLVMValueRef offset = soffset;

		if (inst_offset)
			offset = LLVMBuildAdd(ctx->builder, offset,
					      LLVMConstInt(ctx->i32, inst_offset, 0), "");
		if (voffset)
			offset = LLVMBuildAdd(ctx->builder, offset, voffset, "");

		LLVMValueRef args[] = {
			bitcast_to_float(ctx, vdata),
			LLVMBuildBitCast(ctx->builder, rsrc, ctx->v4i32, ""),
			LLVMConstInt(ctx->i32, 0, 0),
			offset,
			LLVMConstInt(ctx->i1, glc, 0),
			LLVMConstInt(ctx->i1, slc, 0),
		};

		snprintf(name, sizeof(name), "llvm.amdgcn.buffer.store.%s",
			 types[func]);

		ac_build_intrinsic(ctx, name, ctx->voidt,
				   args, ARRAY_SIZE(args),
				   writeonly_memory ?
					   AC_FUNC_ATTR_INACCESSIBLE_MEM_ONLY :
					   AC_FUNC_ATTR_WRITEONLY);
		return;
	}

	static unsigned dfmt[] = {
		V_008F0C_BUF_DATA_FORMAT_32,
		V_008F0C_BUF_DATA_FORMAT_32_32,
		V_008F0C_BUF_DATA_FORMAT_32_32_32,
		V_008F0C_BUF_DATA_FORMAT_32_32_32_32
	};
	assert(num_channels >= 1 && num_channels <= 4);

	LLVMValueRef args[] = {
		rsrc,
		vdata,
		LLVMConstInt(ctx->i32, num_channels, 0),
		voffset ? voffset : LLVMGetUndef(ctx->i32),
		soffset,
		LLVMConstInt(ctx->i32, inst_offset, 0),
		LLVMConstInt(ctx->i32, dfmt[num_channels - 1], 0),
		LLVMConstInt(ctx->i32, V_008F0C_BUF_NUM_FORMAT_UINT, 0),
		LLVMConstInt(ctx->i32, voffset != NULL, 0),
		LLVMConstInt(ctx->i32, 0, 0), /* idxen */
		LLVMConstInt(ctx->i32, glc, 0),
		LLVMConstInt(ctx->i32, slc, 0),
		LLVMConstInt(ctx->i32, 0, 0), /* tfe*/
	};

	/* The instruction offset field has 12 bits */
	assert(voffset || inst_offset < (1 << 12));

	/* The intrinsic is overloaded, we need to add a type suffix for overloading to work. */
	unsigned func = CLAMP(num_channels, 1, 3) - 1;
	const char *types[] = {"i32", "v2i32", "v4i32"};
	char name[256];
	snprintf(name, sizeof(name), "llvm.SI.tbuffer.store.%s", types[func]);

	ac_build_intrinsic(ctx, name, ctx->voidt,
			   args, ARRAY_SIZE(args),
			   AC_FUNC_ATTR_LEGACY);
}

LLVMValueRef
ac_build_buffer_load(struct ac_llvm_context *ctx,
		     LLVMValueRef rsrc,
		     int num_channels,
		     LLVMValueRef vindex,
		     LLVMValueRef voffset,
		     LLVMValueRef soffset,
		     unsigned inst_offset,
		     unsigned glc,
		     unsigned slc,
		     bool can_speculate,
		     bool allow_smem)
{
	LLVMValueRef offset = LLVMConstInt(ctx->i32, inst_offset, 0);
	if (voffset)
		offset = LLVMBuildAdd(ctx->builder, offset, voffset, "");
	if (soffset)
		offset = LLVMBuildAdd(ctx->builder, offset, soffset, "");

	/* TODO: VI and later generations can use SMEM with GLC=1.*/
	if (allow_smem && !glc && !slc) {
		assert(vindex == NULL);

		LLVMValueRef result[4];

		for (int i = 0; i < num_channels; i++) {
			if (i) {
				offset = LLVMBuildAdd(ctx->builder, offset,
						      LLVMConstInt(ctx->i32, 4, 0), "");
			}
			LLVMValueRef args[2] = {rsrc, offset};
			result[i] = ac_build_intrinsic(ctx, "llvm.SI.load.const.v4i32",
						       ctx->f32, args, 2,
						       AC_FUNC_ATTR_READNONE |
						       AC_FUNC_ATTR_LEGACY);
		}
		if (num_channels == 1)
			return result[0];

		if (num_channels == 3)
			result[num_channels++] = LLVMGetUndef(ctx->f32);
		return ac_build_gather_values(ctx, result, num_channels);
	}

	unsigned func = CLAMP(num_channels, 1, 3) - 1;

	LLVMValueRef args[] = {
		LLVMBuildBitCast(ctx->builder, rsrc, ctx->v4i32, ""),
		vindex ? vindex : LLVMConstInt(ctx->i32, 0, 0),
		offset,
		LLVMConstInt(ctx->i1, glc, 0),
		LLVMConstInt(ctx->i1, slc, 0)
	};

	LLVMTypeRef types[] = {ctx->f32, LLVMVectorType(ctx->f32, 2),
			       ctx->v4f32};
	const char *type_names[] = {"f32", "v2f32", "v4f32"};
	char name[256];

	snprintf(name, sizeof(name), "llvm.amdgcn.buffer.load.%s",
		 type_names[func]);

	return ac_build_intrinsic(ctx, name, types[func], args,
				  ARRAY_SIZE(args),
				  /* READNONE means writes can't affect it, while
				   * READONLY means that writes can affect it. */
				  can_speculate && HAVE_LLVM >= 0x0400 ?
					  AC_FUNC_ATTR_READNONE :
					  AC_FUNC_ATTR_READONLY);
}

LLVMValueRef ac_build_buffer_load_format(struct ac_llvm_context *ctx,
					 LLVMValueRef rsrc,
					 LLVMValueRef vindex,
					 LLVMValueRef voffset,
					 bool can_speculate)
{
	LLVMValueRef args [] = {
		LLVMBuildBitCast(ctx->builder, rsrc, ctx->v4i32, ""),
		vindex,
		voffset,
		LLVMConstInt(ctx->i1, 0, 0), /* glc */
		LLVMConstInt(ctx->i1, 0, 0), /* slc */
	};

	return ac_build_intrinsic(ctx,
				  "llvm.amdgcn.buffer.load.format.v4f32",
				  ctx->v4f32, args, ARRAY_SIZE(args),
				  /* READNONE means writes can't affect it, while
				   * READONLY means that writes can affect it. */
				  can_speculate && HAVE_LLVM >= 0x0400 ?
					  AC_FUNC_ATTR_READNONE :
					  AC_FUNC_ATTR_READONLY);
}

/**
 * Set range metadata on an instruction.  This can only be used on load and
 * call instructions.  If you know an instruction can only produce the values
 * 0, 1, 2, you would do set_range_metadata(value, 0, 3);
 * \p lo is the minimum value inclusive.
 * \p hi is the maximum value exclusive.
 */
static void set_range_metadata(struct ac_llvm_context *ctx,
			       LLVMValueRef value, unsigned lo, unsigned hi)
{
	LLVMValueRef range_md, md_args[2];
	LLVMTypeRef type = LLVMTypeOf(value);
	LLVMContextRef context = LLVMGetTypeContext(type);

	md_args[0] = LLVMConstInt(type, lo, false);
	md_args[1] = LLVMConstInt(type, hi, false);
	range_md = LLVMMDNodeInContext(context, md_args, 2);
	LLVMSetMetadata(value, ctx->range_md_kind, range_md);
}

LLVMValueRef
ac_get_thread_id(struct ac_llvm_context *ctx)
{
	LLVMValueRef tid;

	LLVMValueRef tid_args[2];
	tid_args[0] = LLVMConstInt(ctx->i32, 0xffffffff, false);
	tid_args[1] = LLVMConstInt(ctx->i32, 0, false);
	tid_args[1] = ac_build_intrinsic(ctx,
					 "llvm.amdgcn.mbcnt.lo", ctx->i32,
					 tid_args, 2, AC_FUNC_ATTR_READNONE);

	tid = ac_build_intrinsic(ctx, "llvm.amdgcn.mbcnt.hi",
				 ctx->i32, tid_args,
				 2, AC_FUNC_ATTR_READNONE);
	set_range_metadata(ctx, tid, 0, 64);
	return tid;
}

/*
 * SI implements derivatives using the local data store (LDS)
 * All writes to the LDS happen in all executing threads at
 * the same time. TID is the Thread ID for the current
 * thread and is a value between 0 and 63, representing
 * the thread's position in the wavefront.
 *
 * For the pixel shader threads are grouped into quads of four pixels.
 * The TIDs of the pixels of a quad are:
 *
 *  +------+------+
 *  |4n + 0|4n + 1|
 *  +------+------+
 *  |4n + 2|4n + 3|
 *  +------+------+
 *
 * So, masking the TID with 0xfffffffc yields the TID of the top left pixel
 * of the quad, masking with 0xfffffffd yields the TID of the top pixel of
 * the current pixel's column, and masking with 0xfffffffe yields the TID
 * of the left pixel of the current pixel's row.
 *
 * Adding 1 yields the TID of the pixel to the right of the left pixel, and
 * adding 2 yields the TID of the pixel below the top pixel.
 */
LLVMValueRef
ac_build_ddxy(struct ac_llvm_context *ctx,
	      bool has_ds_bpermute,
	      uint32_t mask,
	      int idx,
	      LLVMValueRef lds,
	      LLVMValueRef val)
{
	LLVMValueRef thread_id, tl, trbl, tl_tid, trbl_tid, args[2];
	LLVMValueRef result;

	thread_id = ac_get_thread_id(ctx);

	tl_tid = LLVMBuildAnd(ctx->builder, thread_id,
			      LLVMConstInt(ctx->i32, mask, false), "");

	trbl_tid = LLVMBuildAdd(ctx->builder, tl_tid,
				LLVMConstInt(ctx->i32, idx, false), "");

	if (has_ds_bpermute) {
		args[0] = LLVMBuildMul(ctx->builder, tl_tid,
				       LLVMConstInt(ctx->i32, 4, false), "");
		args[1] = val;
		tl = ac_build_intrinsic(ctx,
					"llvm.amdgcn.ds.bpermute", ctx->i32,
					args, 2,
					AC_FUNC_ATTR_READNONE |
					AC_FUNC_ATTR_CONVERGENT);

		args[0] = LLVMBuildMul(ctx->builder, trbl_tid,
				       LLVMConstInt(ctx->i32, 4, false), "");
		trbl = ac_build_intrinsic(ctx,
					  "llvm.amdgcn.ds.bpermute", ctx->i32,
					  args, 2,
					  AC_FUNC_ATTR_READNONE |
					  AC_FUNC_ATTR_CONVERGENT);
	} else {
		LLVMValueRef store_ptr, load_ptr0, load_ptr1;

		store_ptr = ac_build_gep0(ctx, lds, thread_id);
		load_ptr0 = ac_build_gep0(ctx, lds, tl_tid);
		load_ptr1 = ac_build_gep0(ctx, lds, trbl_tid);

		LLVMBuildStore(ctx->builder, val, store_ptr);
		tl = LLVMBuildLoad(ctx->builder, load_ptr0, "");
		trbl = LLVMBuildLoad(ctx->builder, load_ptr1, "");
	}

	tl = LLVMBuildBitCast(ctx->builder, tl, ctx->f32, "");
	trbl = LLVMBuildBitCast(ctx->builder, trbl, ctx->f32, "");
	result = LLVMBuildFSub(ctx->builder, trbl, tl, "");
	return result;
}

void
ac_build_sendmsg(struct ac_llvm_context *ctx,
		 uint32_t msg,
		 LLVMValueRef wave_id)
{
	LLVMValueRef args[2];
	const char *intr_name = (HAVE_LLVM < 0x0400) ? "llvm.SI.sendmsg" : "llvm.amdgcn.s.sendmsg";
	args[0] = LLVMConstInt(ctx->i32, msg, false);
	args[1] = wave_id;
	ac_build_intrinsic(ctx, intr_name, ctx->voidt, args, 2, 0);
}

LLVMValueRef
ac_build_imsb(struct ac_llvm_context *ctx,
	      LLVMValueRef arg,
	      LLVMTypeRef dst_type)
{
	const char *intr_name = (HAVE_LLVM < 0x0400) ? "llvm.AMDGPU.flbit.i32" :
						       "llvm.amdgcn.sffbh.i32";
	LLVMValueRef msb = ac_build_intrinsic(ctx, intr_name,
					      dst_type, &arg, 1,
					      AC_FUNC_ATTR_READNONE);

	/* The HW returns the last bit index from MSB, but NIR/TGSI wants
	 * the index from LSB. Invert it by doing "31 - msb". */
	msb = LLVMBuildSub(ctx->builder, LLVMConstInt(ctx->i32, 31, false),
			   msb, "");

	LLVMValueRef all_ones = LLVMConstInt(ctx->i32, -1, true);
	LLVMValueRef cond = LLVMBuildOr(ctx->builder,
					LLVMBuildICmp(ctx->builder, LLVMIntEQ,
						      arg, LLVMConstInt(ctx->i32, 0, 0), ""),
					LLVMBuildICmp(ctx->builder, LLVMIntEQ,
						      arg, all_ones, ""), "");

	return LLVMBuildSelect(ctx->builder, cond, all_ones, msb, "");
}

LLVMValueRef
ac_build_umsb(struct ac_llvm_context *ctx,
	      LLVMValueRef arg,
	      LLVMTypeRef dst_type)
{
	LLVMValueRef args[2] = {
		arg,
		LLVMConstInt(ctx->i1, 1, 0),
	};
	LLVMValueRef msb = ac_build_intrinsic(ctx, "llvm.ctlz.i32",
					      dst_type, args, ARRAY_SIZE(args),
					      AC_FUNC_ATTR_READNONE);

	/* The HW returns the last bit index from MSB, but TGSI/NIR wants
	 * the index from LSB. Invert it by doing "31 - msb". */
	msb = LLVMBuildSub(ctx->builder, LLVMConstInt(ctx->i32, 31, false),
			   msb, "");

	/* check for zero */
	return LLVMBuildSelect(ctx->builder,
			       LLVMBuildICmp(ctx->builder, LLVMIntEQ, arg,
					     LLVMConstInt(ctx->i32, 0, 0), ""),
			       LLVMConstInt(ctx->i32, -1, true), msb, "");
}

LLVMValueRef ac_build_clamp(struct ac_llvm_context *ctx, LLVMValueRef value)
{
	if (HAVE_LLVM >= 0x0500) {
		LLVMValueRef max[2] = {
			value,
			LLVMConstReal(ctx->f32, 0),
		};
		LLVMValueRef min[2] = {
			LLVMConstReal(ctx->f32, 1),
		};

		min[1] = ac_build_intrinsic(ctx, "llvm.maxnum.f32",
					    ctx->f32, max, 2,
					    AC_FUNC_ATTR_READNONE);
		return ac_build_intrinsic(ctx, "llvm.minnum.f32",
					  ctx->f32, min, 2,
					  AC_FUNC_ATTR_READNONE);
	}

	LLVMValueRef args[3] = {
		value,
		LLVMConstReal(ctx->f32, 0),
		LLVMConstReal(ctx->f32, 1),
	};

	return ac_build_intrinsic(ctx, "llvm.AMDGPU.clamp.", ctx->f32, args, 3,
				  AC_FUNC_ATTR_READNONE |
				  AC_FUNC_ATTR_LEGACY);
}

void ac_build_export(struct ac_llvm_context *ctx, struct ac_export_args *a)
{
	LLVMValueRef args[9];

	if (HAVE_LLVM >= 0x0500) {
		args[0] = LLVMConstInt(ctx->i32, a->target, 0);
		args[1] = LLVMConstInt(ctx->i32, a->enabled_channels, 0);

		if (a->compr) {
			LLVMTypeRef i16 = LLVMInt16TypeInContext(ctx->context);
			LLVMTypeRef v2i16 = LLVMVectorType(i16, 2);

			args[2] = LLVMBuildBitCast(ctx->builder, a->out[0],
						   v2i16, "");
			args[3] = LLVMBuildBitCast(ctx->builder, a->out[1],
						   v2i16, "");
			args[4] = LLVMConstInt(ctx->i1, a->done, 0);
			args[5] = LLVMConstInt(ctx->i1, a->valid_mask, 0);

			ac_build_intrinsic(ctx, "llvm.amdgcn.exp.compr.v2i16",
					   ctx->voidt, args, 6, 0);
		} else {
			args[2] = a->out[0];
			args[3] = a->out[1];
			args[4] = a->out[2];
			args[5] = a->out[3];
			args[6] = LLVMConstInt(ctx->i1, a->done, 0);
			args[7] = LLVMConstInt(ctx->i1, a->valid_mask, 0);

			ac_build_intrinsic(ctx, "llvm.amdgcn.exp.f32",
					   ctx->voidt, args, 8, 0);
		}
		return;
	}

	args[0] = LLVMConstInt(ctx->i32, a->enabled_channels, 0);
	args[1] = LLVMConstInt(ctx->i32, a->valid_mask, 0);
	args[2] = LLVMConstInt(ctx->i32, a->done, 0);
	args[3] = LLVMConstInt(ctx->i32, a->target, 0);
	args[4] = LLVMConstInt(ctx->i32, a->compr, 0);
	memcpy(args + 5, a->out, sizeof(a->out[0]) * 4);

	ac_build_intrinsic(ctx, "llvm.SI.export", ctx->voidt, args, 9,
			   AC_FUNC_ATTR_LEGACY);
}

LLVMValueRef ac_build_image_opcode(struct ac_llvm_context *ctx,
				   struct ac_image_args *a)
{
	LLVMTypeRef dst_type;
	LLVMValueRef args[11];
	unsigned num_args = 0;
	const char *name;
	char intr_name[128], type[64];

	if (HAVE_LLVM >= 0x0400) {
		bool sample = a->opcode == ac_image_sample ||
			      a->opcode == ac_image_gather4 ||
			      a->opcode == ac_image_get_lod;

		if (sample)
			args[num_args++] = bitcast_to_float(ctx, a->addr);
		else
			args[num_args++] = a->addr;

		args[num_args++] = a->resource;
		if (sample)
			args[num_args++] = a->sampler;
		args[num_args++] = LLVMConstInt(ctx->i32, a->dmask, 0);
		if (sample)
			args[num_args++] = LLVMConstInt(ctx->i1, a->unorm, 0);
		args[num_args++] = LLVMConstInt(ctx->i1, 0, 0); /* glc */
		args[num_args++] = LLVMConstInt(ctx->i1, 0, 0); /* slc */
		args[num_args++] = LLVMConstInt(ctx->i1, 0, 0); /* lwe */
		args[num_args++] = LLVMConstInt(ctx->i1, a->da, 0);

		switch (a->opcode) {
		case ac_image_sample:
			name = "llvm.amdgcn.image.sample";
			break;
		case ac_image_gather4:
			name = "llvm.amdgcn.image.gather4";
			break;
		case ac_image_load:
			name = "llvm.amdgcn.image.load";
			break;
		case ac_image_load_mip:
			name = "llvm.amdgcn.image.load.mip";
			break;
		case ac_image_get_lod:
			name = "llvm.amdgcn.image.getlod";
			break;
		case ac_image_get_resinfo:
			name = "llvm.amdgcn.image.getresinfo";
			break;
		default:
			unreachable("invalid image opcode");
		}

		ac_build_type_name_for_intr(LLVMTypeOf(args[0]), type,
					    sizeof(type));

		snprintf(intr_name, sizeof(intr_name), "%s%s%s%s.v4f32.%s.v8i32",
			name,
			a->compare ? ".c" : "",
			a->bias ? ".b" :
			a->lod ? ".l" :
			a->deriv ? ".d" :
			a->level_zero ? ".lz" : "",
			a->offset ? ".o" : "",
			type);

		LLVMValueRef result =
			ac_build_intrinsic(ctx, intr_name,
					   ctx->v4f32, args, num_args,
					   AC_FUNC_ATTR_READNONE);
		if (!sample) {
			result = LLVMBuildBitCast(ctx->builder, result,
						  ctx->v4i32, "");
		}
		return result;
	}

	args[num_args++] = a->addr;
	args[num_args++] = a->resource;

	if (a->opcode == ac_image_load ||
	    a->opcode == ac_image_load_mip ||
	    a->opcode == ac_image_get_resinfo) {
		dst_type = ctx->v4i32;
	} else {
		dst_type = ctx->v4f32;
		args[num_args++] = a->sampler;
	}

	args[num_args++] = LLVMConstInt(ctx->i32, a->dmask, 0);
	args[num_args++] = LLVMConstInt(ctx->i32, a->unorm, 0);
	args[num_args++] = LLVMConstInt(ctx->i32, 0, 0); /* r128 */
	args[num_args++] = LLVMConstInt(ctx->i32, a->da, 0);
	args[num_args++] = LLVMConstInt(ctx->i32, 0, 0); /* glc */
	args[num_args++] = LLVMConstInt(ctx->i32, 0, 0); /* slc */
	args[num_args++] = LLVMConstInt(ctx->i32, 0, 0); /* tfe */
	args[num_args++] = LLVMConstInt(ctx->i32, 0, 0); /* lwe */

	switch (a->opcode) {
	case ac_image_sample:
		name = "llvm.SI.image.sample";
		break;
	case ac_image_gather4:
		name = "llvm.SI.gather4";
		break;
	case ac_image_load:
		name = "llvm.SI.image.load";
		break;
	case ac_image_load_mip:
		name = "llvm.SI.image.load.mip";
		break;
	case ac_image_get_lod:
		name = "llvm.SI.getlod";
		break;
	case ac_image_get_resinfo:
		name = "llvm.SI.getresinfo";
		break;
	}

	ac_build_type_name_for_intr(LLVMTypeOf(a->addr), type, sizeof(type));
	snprintf(intr_name, sizeof(intr_name), "%s%s%s%s.%s",
		name,
		a->compare ? ".c" : "",
		a->bias ? ".b" :
		a->lod ? ".l" :
		a->deriv ? ".d" :
		a->level_zero ? ".lz" : "",
		a->offset ? ".o" : "",
		type);

	return ac_build_intrinsic(ctx, intr_name,
				  dst_type, args, num_args,
				  AC_FUNC_ATTR_READNONE |
				  AC_FUNC_ATTR_LEGACY);
}

LLVMValueRef ac_build_cvt_pkrtz_f16(struct ac_llvm_context *ctx,
				    LLVMValueRef args[2])
{
	if (HAVE_LLVM >= 0x0500) {
		LLVMTypeRef v2f16 =
			LLVMVectorType(LLVMHalfTypeInContext(ctx->context), 2);
		LLVMValueRef res =
			ac_build_intrinsic(ctx, "llvm.amdgcn.cvt.pkrtz",
					   v2f16, args, 2,
					   AC_FUNC_ATTR_READNONE);
		return LLVMBuildBitCast(ctx->builder, res, ctx->i32, "");
	}

	return ac_build_intrinsic(ctx, "llvm.SI.packf16", ctx->i32, args, 2,
				  AC_FUNC_ATTR_READNONE |
				  AC_FUNC_ATTR_LEGACY);
}

/**
 * KILL, AKA discard in GLSL.
 *
 * \param value  kill if value < 0.0 or value == NULL.
 */
void ac_build_kill(struct ac_llvm_context *ctx, LLVMValueRef value)
{
	if (value) {
		ac_build_intrinsic(ctx, "llvm.AMDGPU.kill", ctx->voidt,
				   &value, 1, AC_FUNC_ATTR_LEGACY);
	} else {
		ac_build_intrinsic(ctx, "llvm.AMDGPU.kilp", ctx->voidt,
				   NULL, 0, AC_FUNC_ATTR_LEGACY);
	}
}

LLVMValueRef ac_build_bfe(struct ac_llvm_context *ctx, LLVMValueRef input,
			  LLVMValueRef offset, LLVMValueRef width,
			  bool is_signed)
{
	LLVMValueRef args[] = {
		input,
		offset,
		width,
	};

	if (HAVE_LLVM >= 0x0500) {
		return ac_build_intrinsic(ctx,
					  is_signed ? "llvm.amdgcn.sbfe.i32" :
						      "llvm.amdgcn.ubfe.i32",
					  ctx->i32, args, 3,
					  AC_FUNC_ATTR_READNONE);
	}

	return ac_build_intrinsic(ctx,
				  is_signed ? "llvm.AMDGPU.bfe.i32" :
					      "llvm.AMDGPU.bfe.u32",
				  ctx->i32, args, 3,
				  AC_FUNC_ATTR_READNONE |
				  AC_FUNC_ATTR_LEGACY);
}

void ac_get_image_intr_name(const char *base_name,
			    LLVMTypeRef data_type,
			    LLVMTypeRef coords_type,
			    LLVMTypeRef rsrc_type,
			    char *out_name, unsigned out_len)
{
        char coords_type_name[8];

        ac_build_type_name_for_intr(coords_type, coords_type_name,
                            sizeof(coords_type_name));

        if (HAVE_LLVM <= 0x0309) {
                snprintf(out_name, out_len, "%s.%s", base_name, coords_type_name);
        } else {
                char data_type_name[8];
                char rsrc_type_name[8];

                ac_build_type_name_for_intr(data_type, data_type_name,
                                        sizeof(data_type_name));
                ac_build_type_name_for_intr(rsrc_type, rsrc_type_name,
                                        sizeof(rsrc_type_name));
                snprintf(out_name, out_len, "%s.%s.%s.%s", base_name,
                         data_type_name, coords_type_name, rsrc_type_name);
        }
}

#define AC_EXP_TARGET (HAVE_LLVM >= 0x0500 ? 0 : 3)
#define AC_EXP_OUT0 (HAVE_LLVM >= 0x0500 ? 2 : 5)

enum ac_ir_type {
	AC_IR_UNDEF,
	AC_IR_CONST,
	AC_IR_VALUE,
};

struct ac_vs_exp_chan
{
	LLVMValueRef value;
	float const_float;
	enum ac_ir_type type;
};

struct ac_vs_exp_inst {
	unsigned offset;
	LLVMValueRef inst;
	struct ac_vs_exp_chan chan[4];
};

struct ac_vs_exports {
	unsigned num;
	struct ac_vs_exp_inst exp[VARYING_SLOT_MAX];
};

/* Return true if the PARAM export has been eliminated. */
static bool ac_eliminate_const_output(uint8_t *vs_output_param_offset,
				      uint32_t num_outputs,
				      struct ac_vs_exp_inst *exp)
{
	unsigned i, default_val; /* SPI_PS_INPUT_CNTL_i.DEFAULT_VAL */
	bool is_zero[4] = {}, is_one[4] = {};

	for (i = 0; i < 4; i++) {
		/* It's a constant expression. Undef outputs are eliminated too. */
		if (exp->chan[i].type == AC_IR_UNDEF) {
			is_zero[i] = true;
			is_one[i] = true;
		} else if (exp->chan[i].type == AC_IR_CONST) {
			if (exp->chan[i].const_float == 0)
				is_zero[i] = true;
			else if (exp->chan[i].const_float == 1)
				is_one[i] = true;
			else
				return false; /* other constant */
		} else
			return false;
	}

	/* Only certain combinations of 0 and 1 can be eliminated. */
	if (is_zero[0] && is_zero[1] && is_zero[2])
		default_val = is_zero[3] ? 0 : 1;
	else if (is_one[0] && is_one[1] && is_one[2])
		default_val = is_zero[3] ? 2 : 3;
	else
		return false;

	/* The PARAM export can be represented as DEFAULT_VAL. Kill it. */
	LLVMInstructionEraseFromParent(exp->inst);

	/* Change OFFSET to DEFAULT_VAL. */
	for (i = 0; i < num_outputs; i++) {
		if (vs_output_param_offset[i] == exp->offset) {
			vs_output_param_offset[i] =
				AC_EXP_PARAM_DEFAULT_VAL_0000 + default_val;
			break;
		}
	}
	return true;
}

static bool ac_eliminate_duplicated_output(uint8_t *vs_output_param_offset,
					   uint32_t num_outputs,
					   struct ac_vs_exports *processed,
				           struct ac_vs_exp_inst *exp)
{
	unsigned p, copy_back_channels = 0;

	/* See if the output is already in the list of processed outputs.
	 * The LLVMValueRef comparison relies on SSA.
	 */
	for (p = 0; p < processed->num; p++) {
		bool different = false;

		for (unsigned j = 0; j < 4; j++) {
			struct ac_vs_exp_chan *c1 = &processed->exp[p].chan[j];
			struct ac_vs_exp_chan *c2 = &exp->chan[j];

			/* Treat undef as a match. */
			if (c2->type == AC_IR_UNDEF)
				continue;

			/* If c1 is undef but c2 isn't, we can copy c2 to c1
			 * and consider the instruction duplicated.
			 */
			if (c1->type == AC_IR_UNDEF) {
				copy_back_channels |= 1 << j;
				continue;
			}

			/* Test whether the channels are not equal. */
			if (c1->type != c2->type ||
			    (c1->type == AC_IR_CONST &&
			     c1->const_float != c2->const_float) ||
			    (c1->type == AC_IR_VALUE &&
			     c1->value != c2->value)) {
				different = true;
				break;
			}
		}
		if (!different)
			break;

		copy_back_channels = 0;
	}
	if (p == processed->num)
		return false;

	/* If a match was found, but the matching export has undef where the new
	 * one has a normal value, copy the normal value to the undef channel.
	 */
	struct ac_vs_exp_inst *match = &processed->exp[p];

	while (copy_back_channels) {
		unsigned chan = u_bit_scan(&copy_back_channels);

		assert(match->chan[chan].type == AC_IR_UNDEF);
		LLVMSetOperand(match->inst, AC_EXP_OUT0 + chan,
			       exp->chan[chan].value);
		match->chan[chan] = exp->chan[chan];
	}

	/* The PARAM export is duplicated. Kill it. */
	LLVMInstructionEraseFromParent(exp->inst);

	/* Change OFFSET to the matching export. */
	for (unsigned i = 0; i < num_outputs; i++) {
		if (vs_output_param_offset[i] == exp->offset) {
			vs_output_param_offset[i] = match->offset;
			break;
		}
	}
	return true;
}

void ac_optimize_vs_outputs(struct ac_llvm_context *ctx,
			    LLVMValueRef main_fn,
			    uint8_t *vs_output_param_offset,
			    uint32_t num_outputs,
			    uint8_t *num_param_exports)
{
	LLVMBasicBlockRef bb;
	bool removed_any = false;
	struct ac_vs_exports exports;

	exports.num = 0;

	/* Process all LLVM instructions. */
	bb = LLVMGetFirstBasicBlock(main_fn);
	while (bb) {
		LLVMValueRef inst = LLVMGetFirstInstruction(bb);

		while (inst) {
			LLVMValueRef cur = inst;
			inst = LLVMGetNextInstruction(inst);
			struct ac_vs_exp_inst exp;

			if (LLVMGetInstructionOpcode(cur) != LLVMCall)
				continue;

			LLVMValueRef callee = ac_llvm_get_called_value(cur);

			if (!ac_llvm_is_function(callee))
				continue;

			const char *name = LLVMGetValueName(callee);
			unsigned num_args = LLVMCountParams(callee);

			/* Check if this is an export instruction. */
			if ((num_args != 9 && num_args != 8) ||
			    (strcmp(name, "llvm.SI.export") &&
			     strcmp(name, "llvm.amdgcn.exp.f32")))
				continue;

			LLVMValueRef arg = LLVMGetOperand(cur, AC_EXP_TARGET);
			unsigned target = LLVMConstIntGetZExtValue(arg);

			if (target < V_008DFC_SQ_EXP_PARAM)
				continue;

			target -= V_008DFC_SQ_EXP_PARAM;

			/* Parse the instruction. */
			memset(&exp, 0, sizeof(exp));
			exp.offset = target;
			exp.inst = cur;

			for (unsigned i = 0; i < 4; i++) {
				LLVMValueRef v = LLVMGetOperand(cur, AC_EXP_OUT0 + i);

				exp.chan[i].value = v;

				if (LLVMIsUndef(v)) {
					exp.chan[i].type = AC_IR_UNDEF;
				} else if (LLVMIsAConstantFP(v)) {
					LLVMBool loses_info;
					exp.chan[i].type = AC_IR_CONST;
					exp.chan[i].const_float =
						LLVMConstRealGetDouble(v, &loses_info);
				} else {
					exp.chan[i].type = AC_IR_VALUE;
				}
			}

			/* Eliminate constant and duplicated PARAM exports. */
			if (ac_eliminate_const_output(vs_output_param_offset,
						      num_outputs, &exp) ||
			    ac_eliminate_duplicated_output(vs_output_param_offset,
							   num_outputs, &exports,
							   &exp)) {
				removed_any = true;
			} else {
				exports.exp[exports.num++] = exp;
			}
		}
		bb = LLVMGetNextBasicBlock(bb);
	}

	/* Remove holes in export memory due to removed PARAM exports.
	 * This is done by renumbering all PARAM exports.
	 */
	if (removed_any) {
		uint8_t old_offset[VARYING_SLOT_MAX];
		unsigned out, i;

		/* Make a copy of the offsets. We need the old version while
		 * we are modifying some of them. */
		memcpy(old_offset, vs_output_param_offset,
		       sizeof(old_offset));

		for (i = 0; i < exports.num; i++) {
			unsigned offset = exports.exp[i].offset;

			/* Update vs_output_param_offset. Multiple outputs can
			 * have the same offset.
			 */
			for (out = 0; out < num_outputs; out++) {
				if (old_offset[out] == offset)
					vs_output_param_offset[out] = i;
			}

			/* Change the PARAM offset in the instruction. */
			LLVMSetOperand(exports.exp[i].inst, AC_EXP_TARGET,
				       LLVMConstInt(ctx->i32,
						    V_008DFC_SQ_EXP_PARAM + i, 0));
		}
		*num_param_exports = exports.num;
	}
}