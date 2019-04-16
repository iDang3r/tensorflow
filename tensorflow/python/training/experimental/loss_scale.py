# Copyright 2019 The TensorFlow Authors. All Rights Reserved.
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
# limitations under the License.
# ==============================================================================
"""Contains LossScale classes."""
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import abc

import six

from tensorflow.python.distribute import distribution_strategy_context
from tensorflow.python.distribute import reduce_util
from tensorflow.python.eager import context
from tensorflow.python.framework import dtypes
from tensorflow.python.framework import ops
from tensorflow.python.ops import control_flow_ops
from tensorflow.python.ops import math_ops
from tensorflow.python.ops import variables
from tensorflow.python.training.tracking import base as trackable
from tensorflow.python.ops import variable_scope


# TODO(reedwm): Merge this with tf.keras.mixed_precision.experimental.LossScale
@six.add_metaclass(abc.ABCMeta)
class LossScale(trackable.Trackable):
  """Loss scale base class.

  Instances of this class represent a loss scale. Calling instances of this
  class returns the loss scale as a scalar float32 tensor, while method
  `update()` updates the loss scale depending on the values of the gradients.
  Optimizers use instances of this class to scale loss and gradients.

  Note: this LossScale class can only be used with a v1 optimizer wrapper,
  tf.train.experimental.MixedPrecisionLossScaleOptimizer. For a v2
  wrapper, tf.keras.mixed_precision.experimental.LossScaleOptimizer, a
  tf.keras.mixed_precision.experimental.LossScale should be used instead.
  """

  def __init__(self):
    """Initializes the loss scale class."""
    self._weights = {}

  @abc.abstractmethod
  def __call__(self):
    """Returns the current loss scale as a scalar `float32` tensor."""
    pass

  @abc.abstractmethod
  def update(self, grads):
    """Updates the value of the loss scale.

    The loss scale will be potentially updated, based on the value of `grads`.
    The tensor returned by calling this class is only updated when this function
    is evaluated.

    In eager mode, this directly updates the loss scale, so that calling
    `__call__` will return the newly updated loss scale. In graph mode,
    this returns an op that, when evaluated, updates the loss scale.

    This function also returns a `should_apply_gradients` bool. If False,
    gradients should not be applied to the variables that step, as nonfinite
    gradients were found, and the loss scale has been be updated to reduce the
    chance of finding nonfinite gradients in the next step. Some loss scale
    classes will always return True, as they cannot adjust themselves in
    response to nonfinite gradients.

    When a DistributionStrategy is used, this function may only be called in a
    cross-replica context.

    Args:
      grads: A list of unscaled gradients, each which is the gradient of the
        loss with respect to a weight. The gradients should have already been
        divided by the loss scale being before passed to this function.

    Returns:
      update_op: In eager mode, None. In graph mode, an op to update the loss
        scale.
      should_apply_gradients: Either a bool or a scalar boolean tensor. If
        False, the caller should skip applying `grads` to the variables this
        step.
    """
    pass

  def _add_weight(self, name, initial_value, dtype=None):
    """Adds a weight to this loss scale manager..

    Args:
      name: Variable name.
      initial_value: The variable's initial value.
      dtype: The type of the variable.

    Returns:
      A variable.

    Raises:
      RuntimeError: If a weight with `name` has already been added.
    """
    variable = variable_scope.variable(
        initial_value=initial_value,
        name=name,
        dtype=dtype,
        trainable=False,
        use_resource=True,
        synchronization=variables.VariableSynchronization.AUTO,
        # Set aggregation to NONE, as loss scaling variables should never be
        # aggregated.
        aggregation=variables.VariableAggregation.NONE)
    if context.executing_eagerly():
      graph_key = None
    else:
      graph = ops.get_default_graph()
      graph_key = graph._graph_key  # pylint: disable=protected-access

    key = (name, graph_key)
    if self._weights.get(key, None) is not None:
      raise RuntimeError('Duplicate variables detected. {}'.format(key))
    self._weights[key] = variable
    self._handle_deferred_dependencies(name=name, trackable=variable)
    return variable

  @property
  def _checkpoint_dependencies(self):
    """From Trackable. Gather graph-specific weights to save."""
    if context.executing_eagerly():
      graph_key = None
    else:
      graph = ops.get_default_graph()
      graph_key = graph._graph_key  # pylint: disable=protected-access
    weights = []
    for (name, g), v in sorted(self._weights.items(), key=lambda i: i[0][0]):
      if g == graph_key:
        weights.append(trackable.TrackableReference(name=name, ref=v))
    return super(LossScale, self)._checkpoint_dependencies + weights

  def _lookup_dependency(self, name):
    """From Trackable. Find a weight in the current graph."""
    unconditional = super(LossScale, self)._lookup_dependency(name)
    if unconditional is not None:
      return unconditional
    if context.executing_eagerly():
      graph_key = None
    else:
      graph = ops.get_default_graph()
      graph_key = graph._graph_key  # pylint: disable=protected-access
    return self._weights.get((name, graph_key), None)


class FixedLossScale(LossScale):
  """Loss scale class with a fixed value.

  The loss scale is not updated for the lifetime of the class.
  """

  def __init__(self, loss_scale_value):
    """Creates the fixed loss scale.

    Args:
      loss_scale_value: A Python float. Its ideal value varies depending on
        models to run. Choosing a too small loss_scale might affect model
        quality; a too big loss_scale might cause inf or nan. There is no single
        right loss_scale to apply. There is no harm choosing a relatively big
        number as long as no nan or inf is encountered in training.

    Raises:
      ValueError: If loss_scale is less than 1.
    """
    super(FixedLossScale, self).__init__()
    if not isinstance(loss_scale_value, six.integer_types + (float,)):
      raise ValueError('loss_scale must be a Python int or float.')
    if loss_scale_value < 1:
      raise ValueError('loss scale must be at least 1.')
    self._tensor_loss_scale = ops.convert_to_tensor(
        loss_scale_value, dtype=dtypes.float32)

  def __call__(self):
    return self._tensor_loss_scale

  def update(self, grads):
    del grads
    return control_flow_ops.no_op(), True


def _is_all_finite(grads):
  """Returns a scalar boolean tensor indicating if all gradients are finite."""
  is_finite_per_grad = [
      math_ops.reduce_all(math_ops.is_finite(g)) for g in grads if g is not None
  ]
  return math_ops.reduce_all(is_finite_per_grad)


def _op_in_graph_mode(tensor):
  """Returns the tensor's op in graph mode, or the tensor in eager mode.

  This is useful because sometimes an op is needed in graph mode instead of a
  tensor. In eager mode, there are no ops.

  Args:
    tensor: A tensor.

  Returns:
    The tensor's op in graph mode. The tensor in eager mode.
  """
  if context.executing_eagerly():
    return tensor
  return tensor.op


def _assign_if_finite(var, value):
  """Assigns a value to a variable if the value is finite."""
  return control_flow_ops.cond(
      math_ops.is_finite(value), lambda: _op_in_graph_mode(var.assign(value)),
      control_flow_ops.no_op)


class DynamicLossScale(LossScale):
  """Loss scale class that dynamically adjusts the loss scale.

  Dynamic loss scaling works by adjusting the loss scale as training progresses.
  The goal is to keep the loss scale as high as possible without overflowing the
  gradients. As long as the gradients do not overflow, raising the loss scale
  never hurts.

  The algorithm starts by setting the loss scale to an initial value. Every N
  steps that the gradients are finite, the loss scale is increased by some
  factor. However, if a NaN or Inf gradient is found, the gradients for that
  step are not applied, and the loss scale is decreased by the factor. This
  process tends to keep the loss scale as high as possible without gradients
  overflowing.
  """

  def __init__(self,
               initial_loss_scale=2**15,
               increment_period=2000,
               multiplier=2.):
    """Constructor of exponential-update loss scale class.

    Args:
      initial_loss_scale: A Python float.  The loss scale to use at the
        beginning. It's better to start this at a very high number, because a
        loss scale that is too high gets lowered far more quickly than a loss
        scale that is to low gets raised. The default is 2 ** 15, which is
        approximately half the maximum float16 value.
      increment_period: Increases loss scale every `increment_period`
        consecutive steps that finite gradients are encountered. If a nonfinite
        gradient is encountered, the count is reset back to zero.
      multiplier: The multiplier to use when increasing or decreasing the loss
        scale.
    """
    super(DynamicLossScale, self).__init__()
    self._initial_loss_scale = float(initial_loss_scale)
    self._increment_period = int(increment_period)
    self._multiplier = float(multiplier)

    self._current_loss_scale = self._add_weight(
        name='loss_scale',
        dtype=dtypes.float32,
        initial_value=self._initial_loss_scale)
    # The number of consecutive steps with finite gradients since the last
    # nonfinite gradient or change in loss scale.
    self._num_good_steps = self._add_weight(
        name='good_steps', dtype=dtypes.int64, initial_value=0)

  @property
  def initial_loss_scale(self):
    return self._initial_loss_scale

  @property
  def increment_period(self):
    return self._increment_period

  @property
  def multiplier(self):
    return self._multiplier

  def __call__(self):
    return self._current_loss_scale

  def update(self, grads):
    """Updates loss scale based on if gradients are finite in current step."""
    if distribution_strategy_context.has_strategy():
      distribution = distribution_strategy_context.get_cross_replica_context()

      def get_is_finite(grads):
        is_finite = _is_all_finite(grads)
        # We cast to float, because we cannot reduce booleans with
        # DistributionStrategy.
        return math_ops.cast(is_finite, dtypes.float32)

      is_finite_float = distribution.extended.call_for_each_replica(
          get_is_finite, args=(grads,))
      reduced_is_finite_float = distribution.reduce(reduce_util.ReduceOp.SUM,
                                                    is_finite_float, axis=None)
      is_finite = math_ops.equal(reduced_is_finite_float,
                                 distribution.num_replicas_in_sync)
    else:
      is_finite = _is_all_finite(grads)

    def update_if_finite_grads():
      """Update assuming the gradients are finite."""

      def incr_loss_scale():
        new_loss_scale = self._current_loss_scale * self._multiplier
        return control_flow_ops.group(
            _assign_if_finite(self._current_loss_scale, new_loss_scale),
            self._num_good_steps.assign(0))

      return control_flow_ops.cond(
          self._num_good_steps + 1 >= self._increment_period,
          incr_loss_scale, lambda: _op_in_graph_mode(
              self._num_good_steps.assign_add(1)))

    def update_if_not_finite_grads():
      """Update assuming the gradients are nonfinite."""

      new_loss_scale = math_ops.maximum(
          self._current_loss_scale / self._multiplier, 1)
      return control_flow_ops.group(
          self._num_good_steps.assign(0),
          self._current_loss_scale.assign(new_loss_scale))

    update_op = control_flow_ops.cond(is_finite, update_if_finite_grads,
                                      update_if_not_finite_grads)
    should_apply_gradients = is_finite
    return update_op, should_apply_gradients


def get(identifier):
  """Get a loss scale object."""
  if isinstance(identifier, six.integer_types + (float,)):
    return FixedLossScale(identifier)
  if identifier == 'dynamic':
    return DynamicLossScale()
  if isinstance(identifier, LossScale):
    return identifier
  elif identifier is None:
    return None
  else:
    raise ValueError('Could not interpret loss scale identifier: %s' %
                     identifier)
