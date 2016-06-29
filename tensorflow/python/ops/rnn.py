# Copyright 2015 The TensorFlow Authors. All Rights Reserved.
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

"""RNN helpers for TensorFlow models."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

from tensorflow.python.framework import dtypes
from tensorflow.python.framework import ops
from tensorflow.python.framework import tensor_shape
from tensorflow.python.ops import array_ops
from tensorflow.python.ops import control_flow_ops
from tensorflow.python.ops import logging_ops
from tensorflow.python.ops import math_ops
from tensorflow.python.ops import rnn_cell
from tensorflow.python.ops import tensor_array_ops
from tensorflow.python.ops import variable_scope as vs
from tensorflow.python.util import nest


# pylint: disable=protected-access
_state_size_with_prefix = rnn_cell._state_size_with_prefix
# pylint: enable=protected-access


def rnn(cell, inputs, initial_state=None, dtype=None,
        sequence_length=None, scope=None):
  """Creates a recurrent neural network specified by RNNCell `cell`.

  The simplest form of RNN network generated is:
    state = cell.zero_state(...)
    outputs = []
    for input_ in inputs:
      output, state = cell(input_, state)
      outputs.append(output)
    return (outputs, state)

  However, a few other options are available:

  An initial state can be provided.
  If the sequence_length vector is provided, dynamic calculation is performed.
  This method of calculation does not compute the RNN steps past the maximum
  sequence length of the minibatch (thus saving computational time),
  and properly propagates the state at an example's sequence length
  to the final state output.

  The dynamic calculation performed is, at time t for batch row b,
    (output, state)(b, t) =
      (t >= sequence_length(b))
        ? (zeros(cell.output_size), states(b, sequence_length(b) - 1))
        : cell(input(b, t), state(b, t - 1))

  Args:
    cell: An instance of RNNCell.
    inputs: A length T list of inputs, each a tensor of shape
      [batch_size, input_size], or a nested tuple of such elements.
    initial_state: (optional) An initial state for the RNN.
      If `cell.state_size` is an integer, this must be
      a tensor of appropriate type and shape `[batch_size x cell.state_size]`.
      If `cell.state_size` is a tuple, this should be a tuple of
      tensors having shapes `[batch_size, s] for s in cell.state_size`.
    dtype: (optional) The data type for the initial state.  Required if
      initial_state is not provided.
    sequence_length: Specifies the length of each sequence in inputs.
      An int32 or int64 vector (tensor) size `[batch_size]`, values in `[0, T)`.
    scope: VariableScope for the created subgraph; defaults to "RNN".

  Returns:
    A pair (outputs, state) where:
      - outputs is a length T list of outputs (one for each input), or a nested
        tuple of such elements.
      - state is the final state

  Raises:
    TypeError: If `cell` is not an instance of RNNCell.
    ValueError: If `inputs` is `None` or an empty list, or if the input depth
      (column size) cannot be inferred from inputs via shape inference.
  """

  if not isinstance(cell, rnn_cell.RNNCell):
    raise TypeError("cell must be an instance of RNNCell")
  if not nest.is_sequence(inputs):
    raise TypeError("inputs must be a sequence")
  if not inputs:
    raise ValueError("inputs must not be empty")

  outputs = []
  # Create a new scope in which the caching device is either
  # determined by the parent scope, or is set to place the cached
  # Variable using the same placement as for the rest of the RNN.
  with vs.variable_scope(scope or "RNN") as varscope:
    if varscope.caching_device is None:
      varscope.set_caching_device(lambda op: op.device)

    # Obtain the first sequence of the input
    first_input = inputs
    while nest.is_sequence(first_input):
      first_input = first_input[0]

    # Temporarily avoid EmbeddingWrapper and seq2seq badness
    # TODO(lukaszkaiser): remove EmbeddingWrapper
    if first_input.get_shape().ndims != 1:

      input_shape = first_input.get_shape().with_rank_at_least(2)
      fixed_batch_size = input_shape[0]

      flat_inputs = (nest.flatten(inputs)
                     if nest.is_sequence(inputs) else (inputs,))
      for flat_input in flat_inputs:
        input_shape = flat_input.get_shape().with_rank_at_least(2)
        batch_size, input_size = input_shape[0], input_shape[1:]
        fixed_batch_size.merge_with(batch_size)
        for i, size in enumerate(input_size):
          if size.value is None:
            raise ValueError(
                "Input size (dimension %d of inputs) must be accessible via "
                "shape inference, but saw value None." % i)
    else:
      fixed_batch_size = first_input.get_shape().with_rank_at_least(1)[0]

    if fixed_batch_size.value:
      batch_size = fixed_batch_size.value
    else:
      batch_size = array_ops.shape(first_input)[0]
    if initial_state is not None:
      state = initial_state
    else:
      if not dtype:
        raise ValueError("If no initial_state is provided, "
                         "dtype must be specified")
      state = cell.zero_state(batch_size, dtype)

    if sequence_length is not None:  # Prepare variables
      def _create_zero_output(output_size):
        # convert int to TensorShape if necessary
        size = _state_size_with_prefix(output_size, prefix=[batch_size])
        output = array_ops.zeros(array_ops.pack(size), first_input.dtype)
        shape = _state_size_with_prefix(
            output_size, prefix=[fixed_batch_size.value])
        output.set_shape(tensor_shape.TensorShape(shape))
        return output

      output_size = cell.output_size
      output_is_tuple = nest.is_sequence(output_size)
      flat_output_size = (
          nest.flatten(output_size) if output_is_tuple else (output_size,))
      flat_zero_output = tuple(_create_zero_output(size)
                               for size in flat_output_size)
      if output_is_tuple:
        zero_output = nest.pack_sequence_as(structure=output_size,
                                            flat_sequence=flat_zero_output)
      else:
        zero_output = flat_zero_output[0]

      sequence_length = math_ops.to_int32(sequence_length)
      min_sequence_length = math_ops.reduce_min(sequence_length)
      max_sequence_length = math_ops.reduce_max(sequence_length)

    for time, input_ in enumerate(inputs):
      if time > 0: varscope.reuse_variables()
      # pylint: disable=cell-var-from-loop
      call_cell = lambda: cell(input_, state)
      # pylint: enable=cell-var-from-loop
      if sequence_length is not None:
        (output, state) = _rnn_step(
            time=time,
            sequence_length=sequence_length,
            min_sequence_length=min_sequence_length,
            max_sequence_length=max_sequence_length,
            zero_output=zero_output,
            state=state,
            call_cell=call_cell,
            state_size=cell.state_size)
      else:
        (output, state) = call_cell()

      outputs.append(output)

    return (outputs, state)


def state_saving_rnn(cell, inputs, state_saver, state_name,
                     sequence_length=None, scope=None):
  """RNN that accepts a state saver for time-truncated RNN calculation.

  Args:
    cell: An instance of `RNNCell`.
    inputs: A length T list of inputs, each a tensor of shape
      `[batch_size, input_size]`.
    state_saver: A state saver object with methods `state` and `save_state`.
    state_name: Python string or tuple of strings.  The name to use with the
      state_saver. If the cell returns tuples of states (i.e.,
      `cell.state_size` is a tuple) then `state_name` should be a tuple of
      strings having the same length as `cell.state_size`.  Otherwise it should
      be a single string.
    sequence_length: (optional) An int32/int64 vector size [batch_size].
      See the documentation for rnn() for more details about sequence_length.
    scope: VariableScope for the created subgraph; defaults to "RNN".

  Returns:
    A pair (outputs, state) where:
      outputs is a length T list of outputs (one for each input)
      states is the final state

  Raises:
    TypeError: If `cell` is not an instance of RNNCell.
    ValueError: If `inputs` is `None` or an empty list, or if the arity and
     type of `state_name` does not match that of `cell.state_size`.
  """
  state_size = cell.state_size
  state_is_tuple = nest.is_sequence(state_size)
  state_name_tuple = nest.is_sequence(state_name)

  if state_is_tuple != state_name_tuple:
    raise ValueError(
        "state_name should be the same type as cell.state_size.  "
        "state_name: %s, cell.state_size: %s"
        % (str(state_name), str(state_size)))

  if state_is_tuple:
    state_name_flat = nest.flatten(state_name)
    state_size_flat = nest.flatten(state_size)

    if len(state_name_flat) != len(state_size_flat):
      raise ValueError("#elems(state_name) != #elems(state_size): %d vs. %d"
                       % (len(state_name_flat), len(state_size_flat)))

    initial_state = nest.pack_sequence_as(
        structure=state_size,
        flat_sequence=[state_saver.state(s) for s in state_name_flat])
  else:
    initial_state = state_saver.state(state_name)

  (outputs, state) = rnn(cell, inputs, initial_state=initial_state,
                         sequence_length=sequence_length, scope=scope)

  if state_is_tuple:
    flat_state = nest.flatten(state)
    state_name = nest.flatten(state_name)
    save_state = tuple(state_saver.save_state(name, substate)
                       for name, substate in zip(state_name, flat_state))
  else:
    save_state = [state_saver.save_state(state_name, state)]

  with ops.control_dependencies(save_state):
    last_output = outputs[-1]
    output_is_tuple = nest.is_sequence(last_output)
    flat_last_output = (
        nest.flatten(last_output) if output_is_tuple else (last_output,))
    flat_last_output = tuple(
        array_ops.identity(output) for output in flat_last_output)
    if output_is_tuple:
      outputs[-1] = nest.pack_sequence_as(structure=last_output,
                                          flat_sequence=flat_last_output)
    else:
      outputs[-1] = flat_last_output[0]

  return (outputs, state)


# pylint: disable=unused-argument
def _rnn_step(
    time, sequence_length, min_sequence_length, max_sequence_length,
    zero_output, state, call_cell, state_size, skip_conditionals=False):
  """Calculate one step of a dynamic RNN minibatch.

  Returns an (output, state) pair conditioned on the sequence_lengths.
  When skip_conditionals=False, the pseudocode is something like:

  if t >= max_sequence_length:
    return (zero_output, state)
  if t < min_sequence_length:
    return call_cell()

  # Selectively output zeros or output, old state or new state depending
  # on if we've finished calculating each row.
  new_output, new_state = call_cell()
  final_output = np.vstack([
    zero_output if time >= sequence_lengths[r] else new_output_r
    for r, new_output_r in enumerate(new_output)
  ])
  final_state = np.vstack([
    state[r] if time >= sequence_lengths[r] else new_state_r
    for r, new_state_r in enumerate(new_state)
  ])
  return (final_output, final_state)

  Args:
    time: Python int, the current time step
    sequence_length: int32 `Tensor` vector of size [batch_size]
    min_sequence_length: int32 `Tensor` scalar, min of sequence_length
    max_sequence_length: int32 `Tensor` scalar, max of sequence_length
    zero_output: `Tensor` vector of shape [output_size]
    state: Either a single `Tensor` matrix of shape `[batch_size, state_size]`,
      or a list/tuple of such tensors.
    call_cell: lambda returning tuple of (new_output, new_state) where
      new_output is a `Tensor` matrix of shape `[batch_size, output_size]`.
      new_state is a `Tensor` matrix of shape `[batch_size, state_size]`.
    state_size: The `cell.state_size` associated with the state.
    skip_conditionals: Python bool, whether to skip using the conditional
      calculations.  This is useful for `dynamic_rnn`, where the input tensor
      matches `max_sequence_length`, and using conditionals just slows
      everything down.

  Returns:
    A tuple of (`final_output`, `final_state`) as given by the pseudocode above:
      final_output is a `Tensor` matrix of shape [batch_size, output_size]
      final_state is either a single `Tensor` matrix, or a tuple of such
        matrices (matching length and shapes of input `state`).

  Raises:
    ValueError: If the cell returns a state tuple whose length does not match
      that returned by `state_size`.
  """

  # Convert state to a list for ease of use
  state_is_tuple = nest.is_sequence(state)
  flat_state = nest.flatten(state) if state_is_tuple else (state,)
  output_is_tuple = nest.is_sequence(zero_output)
  flat_zero_output = (
      nest.flatten(zero_output) if output_is_tuple else (zero_output,))

  def _copy_one_through(output, new_output):
    copy_cond = (time >= sequence_length)
    return math_ops.select(copy_cond, output, new_output)

  def _copy_some_through(flat_new_output, flat_new_state):
    # Use broadcasting select to determine which values should get
    # the previous state & zero output, and which values should get
    # a calculated state & output.
    flat_new_output = tuple(
        _copy_one_through(zero_output, new_output)
        for zero_output, new_output in zip(flat_zero_output, flat_new_output))
    flat_new_state = tuple(
        _copy_one_through(state, new_state)
        for state, new_state in zip(flat_state, flat_new_state))
    return flat_new_output + flat_new_state

  def _maybe_copy_some_through():
    """Run RNN step.  Pass through either no or some past state."""
    new_output, new_state = call_cell()

    nest.assert_same_structure(state, new_state)

    flat_new_state = nest.flatten(new_state) if state_is_tuple else (new_state,)
    flat_new_output = (
        nest.flatten(new_output) if output_is_tuple else (new_output,))
    return control_flow_ops.cond(
        # if t < min_seq_len: calculate and return everything
        time < min_sequence_length, lambda: flat_new_output + flat_new_state,
        # else copy some of it through
        lambda: _copy_some_through(flat_new_output, flat_new_state))

  # TODO(ebrevdo): skipping these conditionals may cause a slowdown,
  # but benefits from removing cond() and its gradient.  We should
  # profile with and without this switch here.
  if skip_conditionals:
    # Instead of using conditionals, perform the selective copy at all time
    # steps.  This is faster when max_seq_len is equal to the number of unrolls
    # (which is typical for dynamic_rnn).
    new_output, new_state = call_cell()
    nest.assert_same_structure(state, new_state)
    new_state = (
        tuple(nest.flatten(new_state)) if state_is_tuple else (new_state,))
    new_output = (
        tuple(nest.flatten(new_output)) if output_is_tuple else (new_output,))
    final_output_and_state = _copy_some_through(new_output, new_state)
  else:
    empty_update = lambda: flat_zero_output + flat_state
    final_output_and_state = control_flow_ops.cond(
        # if t >= max_seq_len: copy all state through, output zeros
        time >= max_sequence_length, empty_update,
        # otherwise calculation is required: copy some or all of it through
        _maybe_copy_some_through)

  if len(final_output_and_state) != len(flat_zero_output) + len(flat_state):
    raise ValueError("Internal error: state and output were not concatenated "
                     "correctly.")
  final_output = final_output_and_state[:len(flat_zero_output)]
  final_state = final_output_and_state[len(flat_zero_output):]

  for output, flat_output in zip(final_output, flat_zero_output):
    output.set_shape(flat_output.get_shape())
  for substate, flat_substate in zip(final_state, flat_state):
    substate.set_shape(flat_substate.get_shape())

  final_output = (
      nest.pack_sequence_as(structure=zero_output, flat_sequence=final_output)
      if output_is_tuple else final_output[0])
  final_state = (
      nest.pack_sequence_as(structure=state, flat_sequence=final_state)
      if state_is_tuple else final_state[0])

  return final_output, final_state


def _reverse_seq(input_seq, lengths):
  """Reverse a list of Tensors up to specified lengths.

  Args:
    input_seq: Sequence of seq_len tensors of dimension (batch_size, n_features)
               or nested tuples of tensors.
    lengths:   A tensor of dimension batch_size, containing lengths for each
               sequence in the batch. If "None" is specified, simply reverses
               the list.

  Returns:
    time-reversed sequence
  """
  if lengths is None:
    return list(reversed(input_seq))

  input_is_tuple = nest.is_sequence(input_seq[0])
  flat_input_seq = tuple(nest.flatten(input_) if input_is_tuple else (input_,)
                         for input_ in input_seq)

  flat_results = [[] for _ in range(len(input_seq))]
  for sequence in zip(*flat_input_seq):
    input_shape = tensor_shape.unknown_shape(
        ndims=sequence[0].get_shape().ndims)
    for input_ in sequence:
      input_shape.merge_with(input_.get_shape())
      input_.set_shape(input_shape)

    # Join into (time, batch_size, depth)
    s_joined = array_ops.pack(sequence)

    # TODO(schuster, ebrevdo): Remove cast when reverse_sequence takes int32
    if lengths is not None:
      lengths = math_ops.to_int64(lengths)

    # Reverse along dimension 0
    s_reversed = array_ops.reverse_sequence(s_joined, lengths, 0, 1)
    # Split again into list
    result = array_ops.unpack(s_reversed)
    for r, flat_result in zip(result, flat_results):
      r.set_shape(input_shape)
      flat_result.append(r)

  results = [nest.pack_sequence_as(structure=input_, flat_sequence=flat_result)
             if input_is_tuple else flat_result[0]
             for input_, flat_result in zip(input_seq, flat_results)]
  return results


def bidirectional_rnn(cell_fw, cell_bw, inputs,
                      initial_state_fw=None, initial_state_bw=None,
                      dtype=None, sequence_length=None, scope=None):
  """Creates a bidirectional recurrent neural network.

  Similar to the unidirectional case above (rnn) but takes input and builds
  independent forward and backward RNNs with the final forward and backward
  outputs depth-concatenated, such that the output will have the format
  [time][batch][cell_fw.output_size + cell_bw.output_size]. The input_size of
  forward and backward cell must match. The initial state for both directions
  is zero by default (but can be set optionally) and no intermediate states are
  ever returned -- the network is fully unrolled for the given (passed in)
  length(s) of the sequence(s) or completely unrolled if length(s) is not given.

  Args:
    cell_fw: An instance of RNNCell, to be used for forward direction.
    cell_bw: An instance of RNNCell, to be used for backward direction.
    inputs: A length T list of inputs, each a tensor of shape
      [batch_size, input_size], or a nested tuple of such elements.
    initial_state_fw: (optional) An initial state for the forward RNN.
      This must be a tensor of appropriate type and shape
      `[batch_size x cell_fw.state_size]`.
      If `cell_fw.state_size` is a tuple, this should be a tuple of
      tensors having shapes `[batch_size, s] for s in cell_fw.state_size`.
    initial_state_bw: (optional) Same as for `initial_state_fw`, but using
      the corresponding properties of `cell_bw`.
    dtype: (optional) The data type for the initial state.  Required if
      either of the initial states are not provided.
    sequence_length: (optional) An int32/int64 vector, size `[batch_size]`,
      containing the actual lengths for each of the sequences.
    scope: VariableScope for the created subgraph; defaults to "BiRNN"

  Returns:
    A tuple (outputs, output_state_fw, output_state_bw) where:
      outputs is a length `T` list of outputs (one for each input), which
        are depth-concatenated forward and backward outputs.
      output_state_fw is the final state of the forward rnn.
      output_state_bw is the final state of the backward rnn.

  Raises:
    TypeError: If `cell_fw` or `cell_bw` is not an instance of `RNNCell`.
    ValueError: If inputs is None or an empty list.
  """

  if not isinstance(cell_fw, rnn_cell.RNNCell):
    raise TypeError("cell_fw must be an instance of RNNCell")
  if not isinstance(cell_bw, rnn_cell.RNNCell):
    raise TypeError("cell_bw must be an instance of RNNCell")
  if not nest.is_sequence(inputs):
    raise TypeError("inputs must be a sequence")
  if not inputs:
    raise ValueError("inputs must not be empty")

  name = scope or "BiRNN"
  # Forward direction
  with vs.variable_scope(name + "_FW") as fw_scope:
    output_fw, output_state_fw = rnn(cell_fw, inputs, initial_state_fw, dtype,
                                     sequence_length, scope=fw_scope)

  # Backward direction
  with vs.variable_scope(name + "_BW") as bw_scope:
    reversed_inputs = _reverse_seq(inputs, sequence_length)
    tmp, output_state_bw = rnn(cell_bw, reversed_inputs, initial_state_bw,
                               dtype, sequence_length, scope=bw_scope)
  output_bw = _reverse_seq(tmp, sequence_length)
  # Concat each of the forward/backward outputs
  flat_output_fw = nest.flatten(output_fw)
  flat_output_bw = nest.flatten(output_bw)

  flat_outputs = tuple(array_ops.concat(1, [fw, bw])
                       for fw, bw in zip(flat_output_fw, flat_output_bw))

  outputs = nest.pack_sequence_as(structure=output_fw,
                                  flat_sequence=flat_outputs)

  return (outputs, output_state_fw, output_state_bw)


def dynamic_rnn(cell, inputs, sequence_length=None, initial_state=None,
                dtype=None, parallel_iterations=None, swap_memory=False,
                time_major=False, scope=None):
  """Creates a recurrent neural network specified by RNNCell `cell`.

  This function is functionally identical to the function `rnn` above, but
  performs fully dynamic unrolling of `inputs`.

  Unlike `rnn`, the input `inputs` is not a Python list of `Tensors`.  Instead,
  it is a single `Tensor` where the maximum time is either the first or second
  dimension (see the parameter `time_major`).  The corresponding output is
  a single `Tensor` having the same number of time steps and batch size.

  The parameter `sequence_length` is required and dynamic calculation is
  automatically performed.

  Args:
    cell: An instance of RNNCell.
    inputs: The RNN inputs.
      If time_major == False (default), this must be a tensor of shape:
        `[batch_size, max_time, input_size]`, or a nested tuple of such
        elements.
      If time_major == True, this must be a tensor of shape:
        `[max_time, batch_size, input_size]`, or a nested tuple of such
        elements.
    sequence_length: (optional) An int32/int64 vector sized `[batch_size]`.
    initial_state: (optional) An initial state for the RNN.
      If `cell.state_size` is an integer, this must be
      a tensor of appropriate type and shape `[batch_size x cell.state_size]`.
      If `cell.state_size` is a tuple, this should be a tuple of
      tensors having shapes `[batch_size, s] for s in cell.state_size`.
    dtype: (optional) The data type for the initial state.  Required if
      initial_state is not provided.
    parallel_iterations: (Default: 32).  The number of iterations to run in
      parallel.  Those operations which do not have any temporal dependency
      and can be run in parallel, will be.  This parameter trades off
      time for space.  Values >> 1 use more memory but take less time,
      while smaller values use less memory but computations take longer.
    swap_memory: Transparently swap the tensors produced in forward inference
      but needed for back prop from GPU to CPU.  This allows training RNNs
      which would typically not fit on a single GPU, with very minimal (or no)
      performance penalty.
    time_major: The shape format of the `inputs` and `outputs` Tensors.
      If true, these `Tensors` must be shaped `[max_time, batch_size, depth]`.
      If false, these `Tensors` must be shaped `[batch_size, max_time, depth]`.
      Using `time_major = True` is a bit more efficient because it avoids
      transposes at the beginning and end of the RNN calculation.  However,
      most TensorFlow data is batch-major, so by default this function
      accepts input and emits output in batch-major form.
    scope: VariableScope for the created subgraph; defaults to "RNN".

  Returns:
    A pair (outputs, state) where:
      outputs: The RNN output `Tensor`.
        If time_major == False (default), this will be a `Tensor` shaped:
          `[batch_size, max_time, cell.output_size]`.
        If time_major == True, this will be a `Tensor` shaped:
          `[max_time, batch_size, cell.output_size]`.
      state: The final state.  If `cell.state_size` is a `Tensor`, this
        will be shaped `[batch_size, cell.state_size]`.  If it is a tuple,
        this be a tuple with shapes `[batch_size, s] for s in cell.state_size`.

  Raises:
    TypeError: If `cell` is not an instance of RNNCell.
    ValueError: If inputs is None or an empty list.
  """

  if not isinstance(cell, rnn_cell.RNNCell):
    raise TypeError("cell must be an instance of RNNCell")

  # By default, time_major==False and inputs are batch-major: shaped
  #   [batch, time, depth]
  # For internal calculations, we transpose to [time, batch, depth]
  input_is_tuple = nest.is_sequence(inputs)
  flat_input = nest.flatten(inputs) if input_is_tuple else (inputs,)

  if not time_major:
    # (B,T,D) => (T,B,D)
    flat_input = tuple(array_ops.transpose(input_, [1, 0, 2])
                       for input_ in flat_input)

  parallel_iterations = parallel_iterations or 32
  if sequence_length is not None:
    sequence_length = math_ops.to_int32(sequence_length)
    sequence_length = array_ops.identity(  # Just to find it in the graph.
        sequence_length, name="sequence_length")

  # Create a new scope in which the caching device is either
  # determined by the parent scope, or is set to place the cached
  # Variable using the same placement as for the rest of the RNN.
  with vs.variable_scope(scope or "RNN") as varscope:
    if varscope.caching_device is None:
      varscope.set_caching_device(lambda op: op.device)
    input_shape = tuple(array_ops.shape(input_) for input_ in flat_input)
    batch_size = input_shape[0][1]

    for input_ in input_shape:
      if input_[1].get_shape() != batch_size.get_shape():
        raise ValueError("All inputs should have the same batch size")

    if initial_state is not None:
      state = initial_state
    else:
      if not dtype:
        raise ValueError("If no initial_state is provided, dtype must be.")
      state = cell.zero_state(batch_size, dtype)

    def _assert_has_shape(x, shape):
      x_shape = array_ops.shape(x)
      packed_shape = array_ops.pack(shape)
      return logging_ops.Assert(
          math_ops.reduce_all(math_ops.equal(x_shape, packed_shape)),
          ["Expected shape for Tensor %s is " % x.name,
           packed_shape, " but saw shape: ", x_shape])

    if sequence_length is not None:
      # Perform some shape validation
      with ops.control_dependencies(
          [_assert_has_shape(sequence_length, [batch_size])]):
        sequence_length = array_ops.identity(
            sequence_length, name="CheckSeqLen")

    inputs = (nest.pack_sequence_as(structure=inputs, flat_sequence=flat_input)
              if input_is_tuple else flat_input[0])

    (outputs, final_state) = _dynamic_rnn_loop(
        cell, inputs, state, parallel_iterations=parallel_iterations,
        swap_memory=swap_memory, sequence_length=sequence_length)

    # Outputs of _dynamic_rnn_loop are always shaped [time, batch, depth].
    # If we are performing batch-major calculations, transpose output back
    # to shape [batch, time, depth]
    if not time_major:
      # (T,B,D) => (B,T,D)
      flat_output = (
          nest.flatten(outputs) if nest.is_sequence(outputs) else (outputs,))
      flat_output = tuple(array_ops.transpose(output, [1, 0, 2])
                          for output in flat_output)
      if nest.is_sequence(outputs):
        outputs = nest.pack_sequence_as(structure=outputs,
                                        flat_sequence=flat_output)
      else:
        outputs = flat_output[0]

    return (outputs, final_state)


def _dynamic_rnn_loop(
    cell, inputs, initial_state, parallel_iterations, swap_memory,
    sequence_length=None):
  """Internal implementation of Dynamic RNN.

  Args:
    cell: An instance of RNNCell.
    inputs: A `Tensor` of shape [time, batch_size, input_size], or a nested
      tuple of such elements.
    initial_state: A `Tensor` of shape `[batch_size, state_size]`, or if
      `cell.state_size` is a tuple, then this should be a tuple of
      tensors having shapes `[batch_size, s] for s in cell.state_size`.
    parallel_iterations: Positive Python int.
    swap_memory: A Python boolean
    sequence_length: (optional) An `int32` `Tensor` of shape [batch_size].

  Returns:
    Tuple `(final_outputs, final_state)`.
    final_outputs:
      A `Tensor` of shape `[time, batch_size, cell.output_size]`.
    final_state:
      A `Tensor` matrix, or tuple of such matrices, matching in length
      and shapes to `initial_state`.

  Raises:
    ValueError: If the input depth cannot be inferred via shape inference
      from the inputs.
  """
  state = initial_state
  assert isinstance(parallel_iterations, int), "parallel_iterations must be int"

  state_size = cell.state_size
  input_is_tuple = nest.is_sequence(inputs)
  output_is_tuple = nest.is_sequence(cell.output_size)

  flat_input = nest.flatten(inputs) if input_is_tuple else (inputs,)
  flat_output_size = (nest.flatten(cell.output_size)
                      if output_is_tuple else (cell.output_size,))

  # Construct an initial output
  input_shape = array_ops.shape(flat_input[0])
  time_steps = input_shape[0]
  batch_size = input_shape[1]

  inputs_got_shape = tuple(input_.get_shape().with_rank_at_least(3)
                           for input_ in flat_input)

  const_time_steps, const_batch_size = inputs_got_shape[0].as_list()[:2]

  for shape in inputs_got_shape:
    if not shape[2:].is_fully_defined():
      raise ValueError(
          "Input size (depth of inputs) must be accessible via shape inference,"
          " but saw value None.")
    got_time_steps = shape[0]
    got_batch_size = shape[1]
    if const_time_steps != got_time_steps:
      raise ValueError(
          "Time steps is not the same for all the elements in the input in a "
          "batch.")
    if const_batch_size != got_batch_size:
      raise ValueError(
          "Batch_size is not the same for all the elements in the input.")

  # Prepare dynamic conditional copying of state & output
  def _create_zero_arrays(size):
    size = _state_size_with_prefix(size, prefix=[batch_size])
    return array_ops.zeros(array_ops.pack(size), flat_input[0].dtype)

  flat_zero_output = tuple(_create_zero_arrays(output)
                           for output in flat_output_size)
  if output_is_tuple:
    zero_output = nest.pack_sequence_as(structure=cell.output_size,
                                        flat_sequence=flat_zero_output)
  else:
    zero_output = flat_zero_output[0]

  if sequence_length is not None:
    min_sequence_length = math_ops.reduce_min(sequence_length)
    max_sequence_length = math_ops.reduce_max(sequence_length)

  time = array_ops.constant(0, dtype=dtypes.int32, name="time")

  with ops.op_scope([], "dynamic_rnn") as scope:
    base_name = scope

  def _create_ta(name):
    return tensor_array_ops.TensorArray(
        dtype=flat_input[0].dtype, size=time_steps,
        tensor_array_name=base_name + name)

  output_ta = tuple(_create_ta("output_%d" % i)
                    for i in range(len(flat_output_size)))
  input_ta = tuple(_create_ta("input_%d" % i) for i in range(len(flat_input)))

  input_ta = tuple(ta.unpack(input_)
                   for ta, input_ in zip(input_ta, flat_input))

  def _time_step(time, output_ta_t, state):
    """Take a time step of the dynamic RNN.

    Args:
      time: int32 scalar Tensor.
      output_ta_t: List of `TensorArray`s that represent the output.
      state: nested tuple of vector tensors that represent the state.

    Returns:
      The tuple (time + 1, output_ta_t with updated flow, new_state).
    """

    input_t = tuple(ta.read(time) for ta in input_ta)
    # Restore some shape information
    for input_, shape in zip(input_t, inputs_got_shape):
      input_.set_shape(shape[1:])

    input_t = (nest.pack_sequence_as(structure=inputs, flat_sequence=input_t)
               if input_is_tuple else input_t[0])
    call_cell = lambda: cell(input_t, state)

    if sequence_length is not None:
      (output, new_state) = _rnn_step(
          time=time,
          sequence_length=sequence_length,
          min_sequence_length=min_sequence_length,
          max_sequence_length=max_sequence_length,
          zero_output=zero_output,
          state=state,
          call_cell=call_cell,
          state_size=state_size,
          skip_conditionals=True)
    else:
      (output, new_state) = call_cell()

    # Pack state if using state tuples
    output = nest.flatten(output) if output_is_tuple else (output,)

    output_ta_t = tuple(ta.write(time, out)
                        for ta, out in zip(output_ta_t, output))

    return (time + 1, output_ta_t, new_state)

  _, output_final_ta, final_state = control_flow_ops.while_loop(
      cond=lambda time, *_: time < time_steps,
      body=_time_step,
      loop_vars=(time, output_ta, state),
      parallel_iterations=parallel_iterations,
      swap_memory=swap_memory)

  # Unpack final output if not using output tuples.
  final_outputs = tuple(ta.pack() for ta in output_final_ta)

  # Restore some shape information
  for output, output_size in zip(final_outputs, flat_output_size):
    shape = _state_size_with_prefix(
        output_size, prefix=[const_time_steps, const_batch_size])
    output.set_shape(shape)

  final_outputs = (
      nest.pack_sequence_as(structure=cell.output_size,
                            flat_sequence=final_outputs)
      if output_is_tuple else final_outputs[0])

  return (final_outputs, final_state)
