#include "sophgo/Dialect/Tpu/IR/TpuOps.h"
#include "sophgo/Interfaces/InferenceInterface.h"
#include "sophgo/Support/Dnnl/Dnnl.h"
#include "sophgo/Support/Helper/Quant.h"
#include "sophgo/Support/Helper/Module.h"

using namespace sophgo;
using namespace sophgo::helper;
using namespace mlir;

LogicalResult tpu::MaxPoolOp::init(InferenceParameter &p) {
  auto pooling = new Pooling();
  int64_t n, c, ih, iw, oh, ow, kh, kw, sh, sw, pt, pb, pl, pr, pad_value;
  bool is_global, count_include_pad;
  auto dt = getDnnlType(input());
  parseParam(n, c, ih, iw, oh, ow, kh, kw, sh, sw, pt, pb, pl, pr, pad_value,
             is_global, count_include_pad);
  pooling->setup(p.inputs[0], p.outputs[0], n, c, ih, iw, oh, ow, kh, kw, sh,
                 sw, pt, pb, pl, pr, false, count_include_pad, pad_value, dt);
  p.handle = (void *)pooling;
  return success();
}

void tpu::MaxPoolOp::deinit(InferenceParameter &p) {
  if (p.handle != nullptr) {
    auto pooling = (Pooling *)p.handle;
    delete pooling;
    p.handle = nullptr;
  }
  return;
}

LogicalResult tpu::MaxPoolOp::inference(InferenceParameter &p) {
  if (p.handle == nullptr) {
    return failure();
  }
  auto pooling = (Pooling *)p.handle;
  pooling->run();
  if (do_relu()) {
    relu(p.outputs[0], p.outputs[0], Module::getNumElements(output()),
         Module::getStorageType(output()));
  }
  return success();
}