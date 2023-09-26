#   Copyright (c) 2021 PaddlePaddle Authors. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License

import copy

import paddle
from paddle.static import Variable

from .dist_attribute import OperatorDistAttr
from .utils import (
    __no_shape_var_type__,
    convert_to_shard_spec,
    verify_shard_spec,
)


class DistributedOperator:
    def __init__(self, serial_op, dist_attr=None):
        self._serial_op = serial_op
        if dist_attr is not None and isinstance(dist_attr, OperatorDistAttr):
            # TODO: remove this deepcopy after we fix the issue
            self._dist_attr = copy.deepcopy(dist_attr)
            # self._dist_attr = dist_attr
            # TODO: Do we really need to write back to serial op？
            self._serial_op.dist_attr = dist_attr
        else:
            assert dist_attr is None, f"{dist_attr}"
            # Use the dist attr of serial_op to do the initialization
            self._dist_attr = self._serial_op.dist_attr
        self._serial_inputs = {}
        self._serial_outputs = {}

    @property
    def serial_op(self):
        return self._serial_op

    @property
    def dist_attr(self):
        return self._dist_attr

    @dist_attr.setter
    def dist_attr(self, dist_attr):
        self._dist_attr = dist_attr
        # TODO: Do we really need to write back to serial op？
        self._serial_op.dist_attr = dist_attr

    def get_serial_input(self, name):
        if self._serial_op.type == "create_py_reader":
            tensor = None
        elif self._serial_op.block._find_var_recursive(name) is not None:
            tensor = self._serial_op.block._var_recursive(name)
        else:
            tensor = None
        return tensor

    def get_serial_output(self, name):
        tensor = self._serial_op.block._var_recursive(name)
        return tensor

    def validate_dist_attr(self):
        if "read" in self.serial_op.type or "while" == self.serial_op.type:
            return True
        for name in self.serial_op.input_arg_names:
            input_dist_attr = self.dist_attr.get_input_dist_attr(name)
            dims_mapping = input_dist_attr.dims_mapping
            if self.get_serial_input(name).type in __no_shape_var_type__:
                shape = []
            else:
                shape = self.get_serial_input(name).shape
            if len(shape) != len(dims_mapping):
                return False
            for i in range(len(dims_mapping)):
                if dims_mapping[i] < -1 or dims_mapping[i] >= len(
                    self.dist_attr.process_mesh.shape
                ):
                    return False
            for i in range(len(self.dist_attr.process_mesh.shape)):
                if dims_mapping.count(i) > 1:
                    return False
            if self.dist_attr.process_mesh != input_dist_attr.process_mesh:
                return False

        for name in self.serial_op.output_arg_names:
            output_dist_attr = self.dist_attr.get_output_dist_attr(name)
            dims_mapping = output_dist_attr.dims_mapping
            if self.get_serial_output(name).type in __no_shape_var_type__:
                shape = []
            else:
                shape = self.get_serial_output(name).shape
            if len(shape) != len(dims_mapping):
                return False
            for i in range(len(dims_mapping)):
                if dims_mapping[i] < -1 or dims_mapping[i] >= len(
                    self.dist_attr.process_mesh.shape
                ):
                    return False
            for i in range(len(self.dist_attr.process_mesh.shape)):
                if dims_mapping.count(i) > 1:
                    return False
            if self.dist_attr.process_mesh != output_dist_attr.process_mesh:
                return False
        return True

    def __str__(self):
        str = "{{op type: {}, op id: {}, op original_id: {}".format(
            self.serial_op.desc.type(),
            self.serial_op.desc.id(),
            self.serial_op.desc.original_id(),
        )

        # str += ", {}".format(self.dist_attr)
        # return str

        if self.dist_attr.is_annotated("process_mesh"):
            annotated_str = "annotated"
        else:
            annotated_str = "non-annotated"
        str += (
            f", process_mesh ({annotated_str}): {self.dist_attr.process_mesh}"
        )

        for arg_name in self.serial_op.desc.input_arg_names():
            try:
                dims_mapping = self.dist_attr.get_input_dims_mapping(arg_name)
            except IndexError:
                raise IndexError(
                    "There is not input var '{}''s dist_attr in current op '{}'".format(
                        arg_name, self.serial_op.desc.type()
                    )
                )
            if self.dist_attr.is_annotated_input_dims_mapping(arg_name):
                annotated_str = "annotated"
            else:
                annotated_str = "non-annotated"
            if self.get_serial_input(arg_name) is not None:
                if self.get_serial_input(arg_name).is_parameter:
                    is_parameter_str = "parameter"
                else:
                    is_parameter_str = "non-parameter"
            else:
                is_parameter_str = "non-parameter"
            str += ", {}'s dims_mapping (input, {}, {}): {}".format(
                arg_name, annotated_str, is_parameter_str, dims_mapping
            )

        for arg_name in self.serial_op.desc.output_arg_names():
            try:
                dims_mapping = self.dist_attr.get_output_dims_mapping(arg_name)
            except IndexError:
                raise IndexError(
                    "There is not output var '{}''s dist_attr in current op '{}'".format(
                        arg_name, self.serial_op.desc.type()
                    )
                )
            if self.dist_attr.is_annotated_output_dims_mapping(arg_name):
                annotated_str = "annotated"
            else:
                annotated_str = "non-annotated"
            if self.get_serial_output(arg_name) is not None:
                if self.get_serial_output(arg_name).is_parameter:
                    is_parameter_str = "parameter"
                else:
                    is_parameter_str = "non-parameter"
            else:
                is_parameter_str = "non-parameter"
            str += ", {}'s dims_mapping (output, {}, {}): {}".format(
                arg_name, annotated_str, is_parameter_str, dims_mapping
            )

        str += ", dist_impl idx: {} , dist_impl type {} }}".format(
            self.dist_attr.impl_idx, self.dist_attr.impl_type
        )

        return str

    def __deepcopy__(self, memo):
        cls = self.__class__
        result = cls.__new__(cls)
        memo[id(self)] = result
        for k, v in self.__dict__.items():
            if (
                k == "_serial_op"
                or k == "_serial_inputs"
                or k == "_serial_outputs"
            ):
                setattr(result, k, v)
            else:
                setattr(result, k, copy.deepcopy(v, memo))
        return result


class DistributedOperatorHelper:
    def __init__(
        self, serial_op, process_mesh, in_dims_mappings, out_dims_mappings
    ):
        self._serial_op = serial_op
        self._process_mesh = process_mesh
        self._in_dims_mappings = in_dims_mappings
        self._out_dims_mappings = out_dims_mappings

    def __call__(self, *args, **kwargs):
        tensor_to_dims_mapping = {}
        index = 0
        if self._in_dims_mappings:
            assert len(args) + len(kwargs) == len(
                self._in_dims_mappings
            ), "The length of dims_mapping {} does not matching the length output {}.".format(
                len(self._in_dims_mappings), len(args) + len(kwargs)
            )
        for arg in args:
            if isinstance(arg, Variable) and self._in_dims_mappings:
                tensor_to_dims_mapping[arg.name] = self._in_dims_mappings[index]
            index += 1
        for arg in kwargs.values() and self._in_dims_mappings:
            if isinstance(arg, Variable):
                tensor_to_dims_mapping[arg.name] = self._in_dims_mappings[index]
            index += 1

        default_prog = paddle.static.default_main_program()
        cur_block = default_prog.current_block()
        op_size = len(cur_block.ops)
        output = self._serial_op(*args, **kwargs)
        new_op_size = len(cur_block.ops)

        if isinstance(output, (tuple, list)):
            new_output = list(output)
        elif isinstance(output, Variable):
            new_output = [output]
        else:
            raise ValueError("Unrecognized output.")

        if self._out_dims_mappings:
            assert len(new_output) == len(
                self._out_dims_mappings
            ), "The length of dims_mapping {} does not matching the length output {}.".format(
                len(self._out_dims_mappings), len(new_output)
            )
        for i, item in enumerate(new_output):
            if isinstance(item, Variable) and self._out_dims_mappings:
                tensor_to_dims_mapping[item.name] = self._out_dims_mappings[i]

        from .dist_context import get_default_distributed_context

        default_dist_ctx = get_default_distributed_context()
        for idx in range(op_size, new_op_size):
            op = cur_block.ops[idx]
            dist_op = DistributedOperator(op)
            for name in dist_op.serial_op.input_arg_names:
                if name in tensor_to_dims_mapping.keys():
                    tensor = dist_op.get_serial_input(name)
                    tensor_dist_attr = dist_op.dist_attr.get_input_dist_attr(
                        name
                    )
                    dims_mapping = tensor_to_dims_mapping[name]
                    if tensor is None:
                        tensor_shape = []
                    else:
                        if tensor.type in __no_shape_var_type__:
                            tensor_shape = []
                        else:
                            tensor_shape = tensor.shape
                    if dims_mapping is not None:
                        dims_mapping = tensor_to_dims_mapping[name]
                        shard_spec = convert_to_shard_spec(
                            dims_mapping, self._process_mesh
                        )
                        assert verify_shard_spec(
                            shard_spec, tensor_shape, self._process_mesh
                        ), "For tensor {}, shard_spec {} is invalid with tensor_shape {} and process_mesh {}.".format(
                            name, shard_spec, tensor_shape, self._process_mesh
                        )
                        tensor_dist_attr.dims_mapping = dims_mapping
                        tensor_dist_attr.mark_annotated("dims_mapping")
            for name in dist_op.serial_op.output_arg_names:
                if name in tensor_to_dims_mapping.keys():
                    tensor = dist_op.get_serial_output(name)
                    tensor_dist_attr = dist_op.dist_attr.get_output_dist_attr(
                        name
                    )
                    dims_mapping = tensor_to_dims_mapping[name]
                    if tensor is None:
                        tensor_shape = []
                    else:
                        if tensor.type in __no_shape_var_type__:
                            tensor_shape = []
                        else:
                            tensor_shape = tensor.shape
                    if dims_mapping is not None:
                        dims_mapping = tensor_to_dims_mapping[name]
                        shard_spec = convert_to_shard_spec(
                            dims_mapping, self._process_mesh
                        )
                        assert verify_shard_spec(
                            shard_spec, tensor_shape, self._process_mesh
                        ), "For tensor {}, shard_spec {} is invalid with tensor_shape {} and process_mesh {}.".format(
                            name, shard_spec, tensor_shape, self._process_mesh
                        )
                        tensor_dist_attr.dims_mapping = dims_mapping
                        tensor_dist_attr.mark_annotated("dims_mapping")
            dist_op.dist_attr.process_mesh = self._process_mesh
            if self._process_mesh is not None:
                dist_op.dist_attr.mark_annotated("process_mesh")
            default_dist_ctx.add_dist_op_for_program(dist_op)
            default_dist_ctx.add_process_mesh(self._process_mesh)

        return output
