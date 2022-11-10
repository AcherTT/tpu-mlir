//===----------------------------------------------------------------------===//
//
// Copyright (c) 2020-2030 by Sophgo Technologies Inc. All rights reserved.
//
// Licensed under the Apache License v2.0.
// See http://www.apache.org/licenses/LICENSE-2.0 for license information.
// SPDX-License-Identifier: Apache-2.0
//
//===----------------------------------------------------------------------===//

#include "tpu_mlir/Backend/CV18xx/CV18xx.h"
#include "tpu_mlir/Backend/CV18xx/CV18xx_global_api.h"
#include "tpu_mlir/Dialect/Tpu/IR/TpuOps.h"
#include "tpu_mlir/Support/Helper/Module.h"
#include "tpu_mlir/Support/Helper/Quant.h"
#include "tpu_mlir/Support/TPUCompressUtil.h"

using namespace mlir;
using namespace tpu_mlir;
using namespace tpu_mlir::helper;
using namespace tpu_mlir::backend;

// for bf16
static void transposeBiasFp32(std::vector<float> &bias_f32,
                              std::vector<uint32_t> &bias_u32) {
  // Split into high/low part
  std::vector<uint16_t> bias_fp32_high;
  std::vector<uint16_t> bias_fp32_low;
  float *biasFloatPtr = bias_f32.data();
  int size = bias_f32.size();
  for (int i = 0; i < size; ++i) {
    unsigned short *temp_short_ptr =
        reinterpret_cast<unsigned short *>(biasFloatPtr + i);
    bias_fp32_high.push_back(temp_short_ptr[1]);
    bias_fp32_low.push_back(temp_short_ptr[0]);
  }
  std::vector<uint16_t> bias_reshape_fp32;
  bias_reshape_fp32.insert(bias_reshape_fp32.end(), bias_fp32_high.begin(),
                           bias_fp32_high.end());
  bias_reshape_fp32.insert(bias_reshape_fp32.end(), bias_fp32_low.begin(),
                           bias_fp32_low.end());
  // then copy into uint32_t
  assert(bias_u32.size() == bias_f32.size());
  memcpy(bias_u32.data(), bias_reshape_fp32.data(), size * sizeof(uint32_t));
}

// for int8
static void transposeBiasInt32(std::vector<int32_t> &bias_int32,
                               std::vector<uint32_t> &bias_u32) {
  int8_t *ptr = reinterpret_cast<int8_t *>(bias_int32.data());
  std::vector<int8_t> b(ptr, ptr + bias_int32.size() * sizeof(int32_t));
  std::vector<int8_t> b_t(b.size());
  for (size_t i = 0; i < bias_int32.size(); i++) {
    for (size_t j = 0; j < 4; j++) {
      b_t[j * bias_int32.size() + i] = b[i * 4 + j];
    }
  }
  memcpy(bias_u32.data(), b_t.data(), b_t.size());
}

void tpu::MatMulOp::codegen_global_cv18xx(void *ctx, int64_t layer_id) {
  CviBackendContext *backend_ctx = (CviBackendContext *)ctx;
  OpBuilder builder(getContext());
  int64_t batch, M, K, N, right_zp;
  bool with_bias, relu;
  double relu_limit;
  parseParam(batch, M, K, N, with_bias, relu, relu_limit, right_zp);
  assert(batch == 1);
  // TODO get batch_high and batch_low, group_fc bias transpose
  int batch_high = 1;    // fixme
  int batch_low = batch; // fixme
  auto op = getOperation();
  gaddr_t ga_input = Module::getAddress(input());
  gaddr_t ga_filter = Module::getAddress(right());
  gaddr_t ga_output = Module::getAddress(output());
  gaddr_t ga_bias = GA_INVALID;
  bool is_fc = isa<top::WeightOp>(right().getDefiningOp());
  if (is_fc) {
    WeightCompresser weight_opt(this->getOperation(), true);
    if (Quant::isUniformQuantized(output())) {
      auto multiplier_v = Module::getI64Array(multipliers(), batch, 1);
      auto rshift_v = Module::getI64Array(rshifts(), batch, 0);
      std::vector<int32_t> multiplier_int32;
      std::vector<int32_t> rshift_int32;
      multiplier_int32.assign(multiplier_v->begin(), multiplier_v->end());
      rshift_int32.assign(rshift_v->begin(), rshift_v->end());
      if (with_bias) {
        // TODO  group_fc bias transpose
        ga_bias = Module::getAddress(bias());
        std::shared_ptr<std::vector<int32_t>> bias_data;
        auto biasOp = bias().getDefiningOp<top::WeightOp>();
        bias_data = biasOp.read<int32_t>();
        std::vector<uint32_t> bias_u32(bias_data->size());
        transposeBiasInt32(*bias_data, bias_u32);
        biasOp.update(bias_u32, bias_data->size());
      }
      cvi_backend_tg_fixed_fc_kernel(
          *backend_ctx, layer_id, ga_input, ga_filter, ga_bias, ga_output, M, K,
          N, with_bias, relu, rshift_int32, multiplier_int32,
          &weight_opt.old_data, &weight_opt.new_data, batch_high, batch_low,
          false, false, false);
    } else {
      // TODO batch_high, batch_low, lstride, ostride, do_quant_bf16
      if (with_bias) {
        // TODO  group_fc bias transpose
        ga_bias = Module::getAddress(bias());
        std::shared_ptr<std::vector<float_t>> bias_data;
        auto biasOp = bias().getDefiningOp<top::WeightOp>();
        bias_data = biasOp.read<float_t>();
        std::vector<uint32_t> bias_u32(bias_data->size());
        transposeBiasFp32(*bias_data, bias_u32);
        biasOp.update(bias_u32, bias_data->size());
        auto new_bias_type = RankedTensorType::get(
            Module::getShape(bias()), builder.getIntegerType(32),
            builder.getI64IntegerAttr(Module::getAddress(bias())));
        bias().setType(new_bias_type);
      }
      gaddr_t ga_scale = GA_INVALID;
      gaddr_t ga_zeropoint = GA_INVALID;
      bool do_quant_bf16 = false;
      cvi_backend_tg_bf16_fc_kernel(
          *backend_ctx, layer_id, ga_input, ga_filter, ga_bias, ga_output, M, K,
          N, with_bias, relu, &weight_opt.old_data, &weight_opt.new_data,
          batch_high, batch_low, false, false, false, do_quant_bf16, ga_scale,
          ga_zeropoint);
    }
  } else {
    if (Quant::isUniformQuantized(output())) {
      auto multiplier_v = Module::getI64Array(multipliers(), 1, 1);
      auto rshift_v = Module::getI64Array(rshifts(), 1, 0);
      std::vector<int32_t> multiplier_int32;
      std::vector<int32_t> rshift_int32;
      multiplier_int32.assign(multiplier_v->begin(), multiplier_v->end());
      rshift_int32.assign(rshift_v->begin(), rshift_v->end());

      cvi_backend_tg_fixed_fc_kernel(
          *backend_ctx, layer_id, ga_input, ga_filter, ga_bias, ga_output, M, K,
          N, with_bias, relu, rshift_int32, multiplier_int32, nullptr, nullptr,
          batch_high, batch_low, false, false, false);
    } else {
      // TODO batch_high, batch_low, lt, rt, ot
      cvi_backend_tg_bf16_fc_kernel(*backend_ctx, layer_id, ga_input, ga_filter,
                                    GA_INVALID, ga_output, M, K, N, false, relu,
                                    nullptr, nullptr, batch_high, batch_low,
                                    false, false, false);
    }
  }
}