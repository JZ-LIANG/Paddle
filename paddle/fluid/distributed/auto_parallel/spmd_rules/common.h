/* Copyright (c) 2023 PaddlePaddle Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#pragma once

#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "paddle/fluid/distributed/auto_parallel/spmd_rules/dist_tensor_spec.h"
#include "paddle/fluid/framework/attribute.h"
#include "paddle/fluid/framework/type_defs.h"
#include "paddle/fluid/platform/enforce.h"
#include "paddle/phi/core/distributed/auto_parallel/dist_attr.h"
#include "paddle/utils/flat_hash_map.h"

namespace paddle {
namespace distributed {
namespace auto_parallel {

using paddle::framework::Attribute;

class SPMDRuleBase {
 public:
  virtual ~SPMDRuleBase() {}

  // Merge the DistAttr of input tensors and infer the DistAttr of the output
  // tensors from the merged input information. The input are DistAttr and Shape
  // (wrapp as DistTensorSpec) of the input tensors (tensors follow the same
  // order defined in Op's Phi API) and Op Attribue of the current op. The ouput
  // are the Merged DistAttr of input tensors and the infered DistAttr of the
  // output tensors. The Merged DistAttr might be different from the original
  // Intput DistAttrs, which means that the corressponding input tensor need to
  // be reshard.
  virtual std::vector<TensorDistAttr> InferForward(
      const std::vector<DistTensorSpec>& input_specs,
      const paddle::framework::AttributeMap& attrs);

  // Merge the DistAttr of output tensors and infer the DistAttr of the input
  // tensors from the merged output information. The input are DistAttr and
  // Shape (wrapp as DistTensorSpec) of the input tensors and Op Attribue of the
  // current op. The ouput are the Merged DistAttr of output tensors and the
  // infered DistAttr of the input tensors. This function will be use in Static
  // Graph mode only, where we have the whole computation graph for sharding
  // propogation.
  virtual std::vector<TensorDistAttr> InferBackward(
      const std::vector<DistTensorSpec>& output_specs,
      const paddle::framework::AttributeMap& attrs);

  template <typename T>
  inline const T ExtractAttr(
      const std::string& name,
      const paddle::framework::AttributeMap& attrs) const {
    auto& attr = GetAttr(name, attrs);

    // In order to get bool attr properly
    framework::proto::AttrType attr_type =
        static_cast<framework::proto::AttrType>(attr.index() - 1);
    if (attr_type == framework::proto::AttrType::INT) {
      if (std::is_same<bool, T>::value) {
        return static_cast<bool>(PADDLE_GET_CONST(int, attr));
      }
    }

    return PADDLE_GET_CONST(T, attr);
  }

  const Attribute& GetAttr(const std::string& name,
                           const paddle::framework::AttributeMap& attrs) const {
    auto iter = attrs.find(name);
    PADDLE_ENFORCE_NE(iter,
                      attrs.end(),
                      paddle::platform::errors::NotFound(
                          "(%s) is not found in AttributeMap."));
    return iter->second;
  }
};

// Merge sharding specification (dims mapping) of given tensors.
// The same axes of different tensors will be merged.
std::unordered_map<std::string, int64_t> ShardingMergeForTensors(
    const std::vector<std::pair<const std::string, const std::vector<int64_t>>>&
        tensor_axes_to_dim_pairs);

// Merge the sharding specification (dims mapping) for one tensor Axis.
// Rule1: A repicated dimension could be merged by any sharded dimension.
// Rule2: A tensor axis could at most be sharded by one mesh dimension.
// (TODO trigger heuristics cost model and reshard to handle axis sharded by
// multiple dimension case.)
int64_t ShardingMergeForAxis(const std::string& axis,
                             const int64_t& mesh_dim1,
                             const int64_t& mesh_dim2);

TensorDistAttr CopyTensorDistAttrForOutput(const TensorDistAttr& src_dist_attr);

// Resolute the partial mesh dimension of a output tensor, giving the
// merged sharding specifcation of input tensors and the axis names of output
// tensor. Input are
std::vector<int64_t> ResoluteOutputPartialDimension(
    const std::unordered_map<std::string, int64_t>& axis_to_dim_map,
    const std::string& tensor_axes);

// Generate the axis notation of tensor for the einsum notation of a broadcast
// operation(alignment star from the rightmost axis). tenosr_ndim: the size of
// the tensor. broadcast_ndim: the maxium size of tensors in this broadcast
// operation. alphabet: the characters used to represent the axes of tensor.
// length of alphabet should >= broadcast_ndim.
std::string GetBroadcastAxes(const int64_t& tenosr_ndim,
                             const int64_t& broadcast_ndim,
                             const std::string& alphabet);

// The static map that stores and initializes all the registered SPMD rules.
class SPMDRuleMap {
 public:
  ~SPMDRuleMap() = default;

  // A singleton
  static SPMDRuleMap& Instance();

  // Returns the spmd rule for the given op_type
  SPMDRuleBase* Get(const std::string& op_type) const;

  // Returns the spmd by name or nullptr if not registered
  SPMDRuleBase* GetNullable(const std::string& op_type) const;

  // Register a spmd for an op_type.
  int Insert(const std::string& op_type, std::unique_ptr<SPMDRuleBase> rule);

  bool Has(const std::string& op_type) const {
    return map_.find(op_type) != map_.end();
  }

 private:
  SPMDRuleMap() = default;
  paddle::flat_hash_map<std::string, std::unique_ptr<SPMDRuleBase>> map_;
  DISABLE_COPY_AND_ASSIGN(SPMDRuleMap);
};

#define REGISTER_SPMD_RULE(op_type, rule_class, ...)                        \
  UNUSED static int __spmd_rule_holder_##op_type =                          \
      ::paddle::distributed::auto_parallel::SPMDRuleMap::Instance().Insert( \
          #op_type, std::make_unique<rule_class>(__VA_ARGS__))

}  // namespace auto_parallel
}  // namespace distributed
}  // namespace paddle
