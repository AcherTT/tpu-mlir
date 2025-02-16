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
#include "tpu_mlir/Dialect/Tpu/Transforms/BM168x/WeightReorder.h"
#include "tpu_mlir/Support/Module.h"
#include "tpu_mlir/Dialect/Tpu/Transforms/BM168x/DynCompileCommon.hpp"
using namespace tpu_mlir::backend;
using namespace tpu_mlir::bm1684x;

// =========================================
// GlobalGenInterface
// =========================================
void tpu::PoolMaskOp::codegen_global_bm1684x() {
  llvm_unreachable("This is for cv18xx.");
}

// ======================================
// Dynamic GlobalGenInterface
// ======================================
int64_t tpu::PoolMaskOp::dyn_codegen_global_bm1684x(void *buffer) {
  return 0;
}

int64_t tpu::PoolMaskOp::get_fw_type_bm1684x() {
  return FW_LAYER_UNKNOWN;
}
