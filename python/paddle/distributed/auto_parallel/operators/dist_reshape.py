# Copyright (c) 2021 PaddlePaddle Authors. All Rights Reserved.
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

from .common import DistributedOperator
from .common import DistributedOperatorImpl
from .common import register_distributed_operator
from .common import register_distributed_operator_impl
from ..utils import is_dim_shard
from ..utils import is_dim_replicate
from ..utils import is_valid_list_index
from ..utils import compute_compatible_dim_mapping
from ..utils import compute_compatible_dims_mapping
from ..utils import compute_compatible_and_update_dim_mapping
from paddle.fluid import core, unique_name
from paddle.fluid.framework import in_dygraph_mode
from paddle.fluid.framework import Program, Parameter, Variable, program_guard
from paddle.fluid.data_feeder import check_variable_and_dtype, check_dtype


class DistributedReshape2(DistributedOperator):
    def __init__(self, name):
        super(DistributedReshape2, self).__init__()
        self._name = name


register_distributed_operator("reshape2", DistributedReshape2("reshape2"))


class DistributedReshapeImpl0(DistributedOperatorImpl):
    def __init__(self, name):
        super(DistributedReshapeImpl0, self).__init__()
        self._name = name
        self._forward_implemented = True
        self._backward_implemented = False

    def is_process_mesh_compatible(self, op_dist_attr):
        """ No restriction for now. """
        return True

    def is_input_compatible(self, op_dist_attr):
        op_desc = op_dist_attr.get_owner_op().desc
        x_name = op_desc.input('X')[0]
        out_name = op_desc.output('Out')[0]
        x_dims_mapping = op_dist_attr.get_input_dims_mapping(x_name)
        out_dims_mapping = op_dist_attr.get_output_dims_mapping(out_name)

        if len(x_dims_mapping) != len(out_dims_mapping) - 1:
            return False

        return True

    def is_output_compatible(self, op_dist_attr):
        op_desc = op_dist_attr.get_owner_op().desc
        x_name = op_desc.input('X')[0]
        out_name = op_desc.output('Out')[0]
        x_dims_mapping = op_dist_attr.get_input_dims_mapping(x_name)
        out_dims_mapping = op_dist_attr.get_output_dims_mapping(out_name)

        if len(x_dims_mapping) != len(out_dims_mapping) - 1:
            return False

        if is_dim_shard(out_dims_mapping[-1]):
            return False

        return True

    def update_dims_mapping(self, op_dist_attr):
        changed = False
        op_desc = op_dist_attr.get_owner_op().desc
        x_name = op_desc.input('X')[0]
        out_name = op_desc.output('Out')[0]
        x_shape_name = op_desc.output('XShape')[0]
        x_dims_mapping = op_dist_attr.get_input_dims_mapping(x_name)
        out_dims_mapping = op_dist_attr.get_output_dims_mapping(out_name)
        x_shape_dims_mapping = op_dist_attr.get_output_dims_mapping(
            x_shape_name)

        for i in range(len(x_dims_mapping)):
            dim_changed = compute_compatible_and_update_dim_mapping(
                [x_dims_mapping, out_dims_mapping], [i, i])
            if dim_changed:
                changed = True

        for i in range(len(x_dims_mapping)):
            x_shape_dims_mapping[i + 1] = x_dims_mapping[i]

        return changed

    @staticmethod
    def forward(ctx, *args, **kwargs):
        """
        kwargs: inputname_mapping & outputname_mapping
        """

        dst_block = ctx.get_dst_main_program().global_block()
        src_op = ctx.get_cur_src_op()
        rank_id = ctx.get_rank_id()
        op_dist_attr = ctx.get_cur_dist_attr()

        # check validation of inputs / outputs 
        for input_name in src_op.desc.input_names():
            assert input_name in kwargs, "Dist op of Reshape input [{}] is not given".format(
                input_name)
        for output_name in src_op.desc.output_names():
            assert output_name in kwargs, "Dist op of Reshape output [{}] is not given".format(
                output_name)
        assert len(
            kwargs['X']
        ) == 1, "Dist op of Reshape input X take 1 variable but got {}".format(
            kwargs['X'])
        assert len(
            kwargs['ShapeTensor']
        ) <= 1, "Dist op of Reshape input ShapeTensor take 0 or 1 variable but got {}".format(
            kwargs['ShapeTensor'])
        assert len(
            kwargs['Shape']
        ) <= 1, "Dist op of Reshape input Shape take 0 or 1 variable but got {}".format(
            kwargs['Shape'])
        assert len(
            kwargs['Out']
        ) == 1, "Dist op of Reshape input Out take 1 variable but got {}".format(
            kwargs['Out'])
        assert len(
            kwargs['XShape']
        ) == 1, "Dist op of Reshape input XShape take 1 variable but got {}".format(
            kwargs['XShape'])

        X_var = dst_block.var(kwargs['X'][0])
        Out_var = dst_block.var(kwargs['Out'][0])
        XShape_var = dst_block.var(kwargs['XShape'][0])
        shape_list = src_op.desc.attr("shape")
        ShapeTensor_var_list = []
        for name in kwargs['ShapeTensor']:
            ShapeTensor_var_list.append(name)
        Shape_var_list = []
        for name in kwargs['Shape']:
            Shape_var_list.append(name)

        # got dist attribute info
        dim_mapping = op_dist_attr.get_output_dims_mapping(Out_var.name)
        process_mesh_shape = op_dist_attr.get_process_mesh().topology

        # modify target shape
        for idx, axis in enumerate(dim_mapping):
            if axis >= 0:
                if len(shape_list) > idx:
                    shape_list[idx] = shape_list[idx] // process_mesh_shape[
                        axis]

        # create op
        new_op_desc = dst_block.desc.append_op()
        new_op_desc.copy_from(src_op.desc)
        new_op_desc.set_input('ShapeTensor', ShapeTensor_var_list)
        new_op_desc.set_input('Shape', Shape_var_list)
        new_op_desc.set_input('X', [X_var.name])
        new_op_desc.set_output('XShape', [XShape_var.name])
        new_op_desc.set_output('Out', [Out_var.name])
        new_op_desc._set_attr('shape', shape_list)

        dst_block._sync_with_cpp()


class DistributedReshapeImpl1(DistributedOperatorImpl):
    def __init__(self, name):
        super(DistributedReshapeImpl1, self).__init__()
        self._name = name
        self._forward_implemented = True
        self._backward_implemented = False

    def is_process_mesh_compatible(self, op_dist_attr):
        """ No restriction for now. """
        return True

    def is_input_compatible(self, op_dist_attr):
        op_desc = op_dist_attr.get_owner_op().desc
        x_name = op_desc.input('X')[0]
        out_name = op_desc.output('Out')[0]
        x_dims_mapping = op_dist_attr.get_input_dims_mapping(x_name)
        out_dims_mapping = op_dist_attr.get_output_dims_mapping(out_name)

        if len(x_dims_mapping) != len(out_dims_mapping) + 1:
            return False

        if is_dim_shard(x_dims_mapping[-1]):
            return False

        return True

    def is_output_compatible(self, op_dist_attr):
        op_desc = op_dist_attr.get_owner_op().desc
        x_name = op_desc.input('X')[0]
        out_name = op_desc.output('Out')[0]
        x_dims_mapping = op_dist_attr.get_input_dims_mapping(x_name)
        out_dims_mapping = op_dist_attr.get_output_dims_mapping(out_name)

        if len(x_dims_mapping) != len(out_dims_mapping) + 1:
            return False

        return True

    def update_dims_mapping(self, op_dist_attr):
        changed = False
        op_desc = op_dist_attr.get_owner_op().desc
        x_name = op_desc.input('X')[0]
        out_name = op_desc.output('Out')[0]
        x_shape_name = op_desc.output('XShape')[0]
        x_dims_mapping = op_dist_attr.get_input_dims_mapping(x_name)
        out_dims_mapping = op_dist_attr.get_output_dims_mapping(out_name)
        x_shape_dims_mapping = op_dist_attr.get_output_dims_mapping(
            x_shape_name)

        for i in range(len(out_dims_mapping)):
            dim_changed = compute_compatible_and_update_dim_mapping(
                [x_dims_mapping, out_dims_mapping], [i, i])
            if dim_changed:
                changed = True

        for i in range(len(x_dims_mapping)):
            x_shape_dims_mapping[i + 1] = x_dims_mapping[i]

        return changed

    @staticmethod
    def forward(ctx, *args, **kwargs):
        """
        kwargs: inputname_mapping & outputname_mapping
        """
        dst_block = ctx.get_dst_main_program().global_block()
        src_op = ctx.get_cur_src_op()
        rank_id = ctx.get_rank_id()
        op_dist_attr = ctx.get_cur_dist_attr()

        # check validation of inputs / outputs 
        for input_name in src_op.desc.input_names():
            assert input_name in kwargs, "Dist op of Reshape input [{}] is not given".format(
                input_name)
        for output_name in src_op.desc.output_names():
            assert output_name in kwargs, "Dist op of Reshape output [{}] is not given".format(
                output_name)
        assert len(
            kwargs['X']
        ) == 1, "Dist op of Reshape input X take 1 variable but got {}".format(
            kwargs['X'])
        assert len(
            kwargs['ShapeTensor']
        ) <= 1, "Dist op of Reshape input ShapeTensor take 0 or 1 variable but got {}".format(
            kwargs['ShapeTensor'])
        assert len(
            kwargs['Shape']
        ) <= 1, "Dist op of Reshape input Shape take 0 or 1 variable but got {}".format(
            kwargs['Shape'])
        assert len(
            kwargs['Out']
        ) == 1, "Dist op of Reshape output Out take 1 variable but got {}".format(
            kwargs['Out'])
        assert len(
            kwargs['XShape']
        ) == 1, "Dist op of Reshape output XShape take 1 variable but got {}".format(
            kwargs['XShape'])

        X_var = dst_block.var(kwargs['X'][0])
        Out_var = dst_block.var(kwargs['Out'][0])
        XShape_var = dst_block.var(kwargs['XShape'][0])
        shape_list = src_op.desc.attr("shape")
        ShapeTensor_var_list = []
        for name in kwargs['ShapeTensor']:
            ShapeTensor_var_list.append(name)
        Shape_var_list = []
        for name in kwargs['Shape']:
            Shape_var_list.append(name)

        # got dist attribute info
        dim_mapping = op_dist_attr.get_output_dims_mapping(Out_var.name)
        process_mesh_shape = op_dist_attr.get_process_mesh().topology

        # modify target shape
        for idx, axis in enumerate(dim_mapping):
            if axis >= 0:
                if len(shape_list) > idx:
                    shape_list[idx] = shape_list[idx] // process_mesh_shape[
                        axis]

        # create op
        new_op_desc = dst_block.desc.append_op()
        new_op_desc.copy_from(src_op.desc)
        new_op_desc.set_input('ShapeTensor', ShapeTensor_var_list)
        new_op_desc.set_input('Shape', Shape_var_list)
        new_op_desc.set_input('X', [X_var.name])
        new_op_desc.set_output('XShape', [XShape_var.name])
        new_op_desc.set_output('Out', [Out_var.name])
        new_op_desc._set_attr('shape', shape_list)

        dst_block._sync_with_cpp()


register_distributed_operator_impl("reshape2",
                                   DistributedReshapeImpl0("add_one_dim_back"))
register_distributed_operator_impl(
    "reshape2", DistributedReshapeImpl1("remove_one_dim_back"))
