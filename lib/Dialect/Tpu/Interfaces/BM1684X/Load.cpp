//===----------------------------------------------------------------------===//
//
// Copyright (C) 2022 Sophgo Technologies Inc.  All rights reserved.
//
// TPU-MLIR is licensed under the 2-Clause BSD License except for the
// third-party components.
//
//===----------------------------------------------------------------------===//

#include "tpu_mlir/Backend/BM168x/BM1684X.h"
#include "tpu_mlir/Dialect/Tpu/IR/TpuOps.h"
#include "tpu_mlir/Dialect/Tpu/Transforms/BM168x/DynCompileCommon.hpp"
#include "tpu_mlir/Support/MathUtils.h"
#include "tpu_mlir/Support/Module.h"
#include <algorithm>

using namespace tpu_mlir::backend;

void tpu::LoadOp::codegen_global_bm1684x() {
  llvm_unreachable("global not support");
}

int64_t tpu::LoadOp::getBufferSize_bm1684x(int64_t in_lmem_bytes,
                                           int64_t out_lmem_bytes,
                                           int64_t in_nslice, int64_t in_hslice,
                                           int64_t out_nslice,
                                           int64_t out_hslice,
                                           group_type_t group_type) {
  return 0;
}

void tpu::LoadOp::codegen_local_bm1684x(int64_t n_step, int64_t h_step,
                                        group_type_t group_type,
                                        local_sec_info_t &sec_info) {
  auto pid_node = (CMD_ID_NODE *)BM168x::instance()->gdma_node;
  auto gi = getGroupInfo(n_step, h_step);
  assert(false == gi.overstepped);

  int64_t N, C, D, H, W, real_hslice;
  int64_t gdma_format;
  module::getNCDHW(getOutput(), N, C, D, H, W, group_type);
  auto data_type = BM168x::getDataType(getOutput());

  real_hslice = gi.h_slice;
  if (data_type == DTYPE_UINT4 || data_type == DTYPE_INT4) {
    gdma_format = BM168x::GDMA_VALUE_FORMAT_INT8;
    data_type = DTYPE_INT8;
    if (gi.h_slice == H) {
      if (W * H & 0x1 == 1) {
        W = align_up(W * H, (int64_t)2) / 2;
        H = 1;
        real_hslice = 1;
      } else {
        if (W & 0x1 == 1)
          real_hslice >>= 1;
        else
          W >>= 1;
      }
    } else {
      real_hslice = gi.h_slice; // to do for int4
    }
  }
  gdma_format = BM168x::getGdmaFormat(data_type);
  auto fmt_bytes = BM168x::getFmtBytes(data_type);
  auto g_addr = module::getAddress(getInput());
  int64_t use_3ic = getUse_3icOptimize();
  if (use_3ic < 4 && use_3ic > 0) {
    auto g_stride = BM168x::getGlobalStride(N, C, H, W);
    if (getDoBcast() == true) {
      C = BM168x::NPU_NUM;
      g_stride.N = 0;
      g_stride.C = 0;
      g_stride.H = 0;
    }
    auto s_stride = BM168x::getLocalStride(gi.n_slice, C, real_hslice, W,
                                           fmt_bytes, gi.eu_align);
    int64_t g_offset =
        (gi.n_idx * g_stride.N + gi.h_idx * g_stride.H) * fmt_bytes;
    auto use_op = *getOutput().getUsers().begin();
    auto conv_op = dyn_cast<tpu::Conv2DOp>(use_op);
    auto kernel = module::getI64Array(conv_op.getKernelShape());
    int64_t to_ic =
        use_3ic == 1
            ? kernel->at(0)
            : (use_3ic == 2 ? kernel->at(1) : kernel->at(0) * kernel->at(1));
    for (int64_t i = 0; i < C; ++i) {
      BM168x::instance()->dl_tensor_broadcast_move_gen_cmd(
          g_addr + g_offset + i * W * H * fmt_bytes, 0, gi.out_addr, i * to_ic,
          gi.n_slice, real_hslice, W, to_ic, g_stride.N, g_stride.H, s_stride.N,
          s_stride.H, gdma_format, true, GDMA_VALUE_DIR_S2L, pid_node);
    }
  } else {
    int64_t c_num_local = ceiling_func(C, Arch::NPU_NUM);
    int64_t c_stride = gi.eu_align ?
                       align_up(real_hslice * W, Arch::eu_num(fmt_bytes)) :
                       real_hslice * W;
    int64_t channel_num = C;
    const int64_t csecs = ceiling_func(channel_num, (int64_t)GDMA_MAX_C);
    if (D <= gi.n_slice) {
      for (int64_t d = 0; d < D; d++) {
        int64_t channel_index = 0;
        while (channel_index < csecs) {
          int64_t real_cslice =
              std::min(channel_num - channel_index * (int64_t)GDMA_MAX_C,
                       (int64_t)GDMA_MAX_C);
          int64_t real_c_num_local =
              (channel_index * (int64_t)GDMA_MAX_C) / Arch::NPU_NUM;
          int64_t src_offset_c =
              channel_index * (int64_t)GDMA_MAX_C * H * W * fmt_bytes;
          int64_t dst_offset_c = real_c_num_local * c_stride * fmt_bytes;
          int64_t real_npu_idx =
              (channel_index * (int64_t)GDMA_MAX_C) % Arch::NPU_NUM;
          int64_t cur_local_offset =
              d * gi.n_slice * c_num_local * c_stride * fmt_bytes +
              dst_offset_c;
          int64_t cur_global_offset = gi.n_idx * C * D * H * W * fmt_bytes +
                                      d * H * W * fmt_bytes +
                                      gi.h_idx * W * fmt_bytes + src_offset_c;
          BM168x::instance()->dl_tensor_stride_move_gen_cmd(
              gi.out_addr + cur_local_offset, real_npu_idx,
              g_addr + cur_global_offset, gi.n_slice, real_cslice, real_hslice,
              W, C * D * H * W, D * H * W, W, 1, c_num_local * c_stride,
              c_stride, W, 1, gdma_format, GDMA_VALUE_DIR_S2L, 0, pid_node);
          channel_index++;
        }
      }      // depth loop
    } else { // HAVE DEPTH,3D [N,C,D,H,W]->[d,n_slice,c,h_slice,w]
      for (int64_t i = 0; i < gi.n_slice; i++) {
        int64_t cur_local_offset = i * c_num_local * c_stride * fmt_bytes;
        int64_t cur_global_offset = (gi.n_idx + i) * C * D * H * W * fmt_bytes +
                                    gi.h_idx * W * fmt_bytes;
        BM168x::instance()->dl_tensor_stride_move_gen_cmd(
            gi.out_addr + cur_local_offset, 0, g_addr + cur_global_offset, D, C,
            real_hslice, W, H * W, D * H * W, W, 1,
            gi.n_slice * c_num_local * c_stride, c_stride, W, 1, gdma_format,
            GDMA_VALUE_DIR_S2L, 0, pid_node);
      } // nslice loop
    }
  }
}

// dynamic codegen
int64_t tpu::LoadOp::dyn_codegen_local_bm1684x(void *buffer) {
  // no need to implement it
  return 0;
}

// ======================================
// Dynamic GlobalGenInterface
// ======================================
int64_t tpu::LoadOp::dyn_codegen_global_bm1684x(void *buffer) {
  // no need to implement it
  return 0;
}

int64_t tpu::LoadOp::get_fw_type_bm1684x() { return FW_LAYER_UNKNOWN; }
