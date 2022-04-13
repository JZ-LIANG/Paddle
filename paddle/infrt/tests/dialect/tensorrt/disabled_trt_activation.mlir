module  {
  func @main_graph(%arg0: !infrt.dense_tensor<CPU, FP32, ANY>) -> !infrt.dense_tensor<CPU, FP32, ANY> {
    %0 = "phi_dt.create_context.gpu"() : () -> !phi.context<GPU>
    %1 = "phi_dt.memcpy.gpu"(%arg0, %0) {d2h = false} : (!infrt.dense_tensor<CPU, FP32, ANY>, !phi.context<GPU>) -> !infrt.dense_tensor<GPU, FP32, NCHW>
    %2 = "trt.create_engine"(%1) ( {
      %6 = "trt.Activation"(%1) {activation_type = 1 : si32, alpha = 0.000000e+00 : f32, beta = 0.000000e+00 : f32} : (!infrt.dense_tensor<GPU, FP32, NCHW>) -> !infrt.dense_tensor<GPU, FP32, NCHW>
      infrt.return %6 : !infrt.dense_tensor<GPU, FP32, NCHW>
    }) {run_once = true} : (!infrt.dense_tensor<GPU, FP32, NCHW>) -> !trt.engine
    %3 = "trt.compute"(%2, %0) : (!trt.engine, !phi.context<GPU>) -> !infrt.tensor_list
    %4 = "dt.tensor_list_get_tensor"(%3) {id = 0 : i32} : (!infrt.tensor_list) -> !infrt.dense_tensor<GPU, FP32, NCHW>
    %5 = "phi_dt.memcpy.gpu"(%4, %0) {d2h = true} : (!infrt.dense_tensor<GPU, FP32, NCHW>, !phi.context<GPU>) -> !infrt.dense_tensor<CPU, FP32, ANY>
    infrt.return %5 : !infrt.dense_tensor<CPU, FP32, ANY>
  }
  func @main() {
    %0 = "phi_dt.create_context.cpu"() : () -> !phi.context<CPU>
    %1 = "phi_dt.create_inited_dense_tensor.cpu.f32"(%0) {dims = [3, 6, 1, 1], layout = #infrt.layout<NCHW>, lod = [0], value = 1.500000e+00 : f32} : (!phi.context<CPU>) -> !infrt.dense_tensor<CPU, FP32, NCHW>
    %2 = infrt.call @main_graph(%1) : (!infrt.dense_tensor<CPU, FP32, NCHW>) -> !infrt.dense_tensor<CPU, FP32, NCHW>
    phi_dt.print_tensor(%2 : !infrt.dense_tensor<CPU, FP32, NCHW>)
    infrt.return
  }
}
