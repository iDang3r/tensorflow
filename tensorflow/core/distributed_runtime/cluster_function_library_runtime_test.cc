/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#include "tensorflow/core/distributed_runtime/cluster_function_library_runtime.h"

#include "tensorflow/core/common_runtime/function_testlib.h"
#include "tensorflow/core/distributed_runtime/rpc/grpc_channel.h"
#include "tensorflow/core/distributed_runtime/rpc/grpc_testlib.h"
#include "tensorflow/core/distributed_runtime/rpc/grpc_worker_cache.h"
#include "tensorflow/core/framework/function_testlib.h"
#include "tensorflow/core/framework/tensor_testutil.h"
#include "tensorflow/core/lib/core/status_test_util.h"
#include "tensorflow/core/platform/test.h"
#include "tensorflow/core/util/equal_graph_def.h"

namespace tensorflow {

class ClusterFunctionLibraryRuntimeTest : public ::testing::Test {
 public:
  ClusterFunctionLibraryRuntimeTest() {
    SessionOptions options;
    TF_CHECK_OK(test::TestCluster::MakeTestCluster(options, 2, &cluster_));
    GrpcChannelSpec spec;
    TF_CHECK_OK(spec.AddHostPortsJob("localhost", cluster_->targets()));
    ChannelCreationFunction channel_func =
        ConvertToChannelCreationFunction(NewHostPortGrpcChannel);
    std::unique_ptr<WorkerCacheInterface> worker_cache(
        NewGrpcWorkerCache(NewGrpcChannelCache(spec, channel_func)));

    worker_session_.reset(new WorkerSession(
        "cluster_test_session", "/job:localhost/replica:0/task:0",
        std::move(worker_cache), std::unique_ptr<DeviceMgr>(),
        std::unique_ptr<GraphMgr>()));

    cluster_flr_.reset(
        new ClusterFunctionLibraryRuntime(worker_session_.get()));
  }

  Status ConstructFunctionGraphHelper(const OpDef& sig,
                                      test::function::Attrs attrs, GraphDef* g,
                                      std::vector<string>* send_keys,
                                      std::vector<string>* recv_keys) {
    return ClusterFunctionLibraryRuntime::ConstructFunctionGraph(
        sig, attrs, g, send_keys, recv_keys);
  }

  Status Instantiate(const string& function_name,
                     const FunctionLibraryDefinition& lib_def,
                     test::function::Attrs attrs,
                     FunctionLibraryRuntime::LocalHandle* local_handle) {
    return cluster_flr_->Instantiate(function_name, lib_def, attrs,
                                     local_handle);
  }

  Status InstantiateAndRun(const string& function_name,
                           const FunctionLibraryDefinition& lib_def,
                           test::function::Attrs attrs,
                           const std::vector<Tensor>& args,
                           std::vector<Tensor*> rets) {
    FunctionLibraryRuntime::LocalHandle handle;
    TF_RETURN_IF_ERROR(
        cluster_flr_->Instantiate(function_name, lib_def, attrs, &handle));

    Notification done;
    FunctionLibraryRuntime::Options opts;
    std::vector<Tensor> out;
    Status status;
    cluster_flr_->Run(opts, handle, args, &out,
                      [&status, &done](const Status& s) {
                        status = s;
                        done.Notify();
                      });
    done.WaitForNotification();
    if (!status.ok()) {
      return status;
    }
    CHECK_EQ(rets.size(), out.size());
    for (size_t i = 0; i < rets.size(); ++i) {
      *rets[i] = out[i];
    }

    return Status::OK();
  }

 protected:
  std::unique_ptr<test::TestCluster> cluster_;
  std::unique_ptr<WorkerSession> worker_session_;
  std::unique_ptr<ClusterFunctionLibraryRuntime> cluster_flr_;
};

TEST_F(ClusterFunctionLibraryRuntimeTest, ConstructFunctionGraph) {
  GraphDef actual;
  std::vector<string> send_keys, recv_keys;
  TF_CHECK_OK(ConstructFunctionGraphHelper(
      test::function::XTimesTwo().signature(),
      {{"T", DT_FLOAT}, {"_target", "/job:a/replica:0/task:0/cpu:0"}}, &actual,
      &send_keys, &recv_keys));

  GraphDef expected;
  protobuf::TextFormat::ParseFromString(R"(
node {
  name: "_recv_x_0"
  op: "_Recv"
  device: "/job:a/replica:0/task:0/device:CPU:0"
  attr {
    key: "client_terminated"
    value {
      b: true
    }
  }
  attr {
    key: "recv_device"
    value {
      s: "/job:a/replica:0/task:0/device:CPU:0"
    }
  }
  attr {
    key: "send_device"
    value {
      s: "/job:a/replica:0/task:0/device:CPU:0"
    }
  }
  attr {
    key: "send_device_incarnation"
    value {
      i: 1
    }
  }
  attr {
    key: "tensor_name"
    value {
      s: "x"
    }
  }
  attr {
    key: "tensor_type"
    value {
      type: DT_FLOAT
    }
  }
}
node {
  name: "XTimesTwo"
  op: "XTimesTwo"
  input: "_recv_x_0"
  device: "/job:a/replica:0/task:0/device:CPU:0"
  attr {
    key: "T"
    value {
      type: DT_FLOAT
    }
  }
  attr {
    key: "_target"
    value {
      s: "/job:a/replica:0/task:0/device:CPU:0"
    }
  }
}
node {
  name: "_send_y_0"
  op: "_Send"
  input: "XTimesTwo"
  device: "/job:a/replica:0/task:0/device:CPU:0"
  attr {
    key: "T"
    value {
      type: DT_FLOAT
    }
  }
  attr {
    key: "client_terminated"
    value {
      b: true
    }
  }
  attr {
    key: "recv_device"
    value {
      s: "/job:a/replica:0/task:0/device:CPU:0"
    }
  }
  attr {
    key: "send_device"
    value {
      s: "/job:a/replica:0/task:0/device:CPU:0"
    }
  }
  attr {
    key: "send_device_incarnation"
    value {
      i: 1
    }
  }
  attr {
    key: "tensor_name"
    value {
      s: "y"
    }
  }
})",
                                        &expected);
  TF_EXPECT_GRAPH_EQ(expected, actual);
}

TEST_F(ClusterFunctionLibraryRuntimeTest, InstantiateAndRun) {
  FunctionDefLibrary proto;
  *(proto.add_function()) = test::function::XTimesTwoInt32();
  FunctionLibraryDefinition lib_def(OpRegistry::Global(), proto);

  Tensor y;
  auto x = test::AsTensor<int32>({1, 2, 3, 4});
  TF_EXPECT_OK(InstantiateAndRun(
      "XTimesTwoInt32", lib_def,
      {{"_target", "/job:localhost/replica:0/task:1/cpu:0"}}, {x}, {&y}));
  test::ExpectTensorEqual<int32>(y, test::AsTensor<int32>({2, 4, 6, 8}));
}

TEST_F(ClusterFunctionLibraryRuntimeTest, InstantiateAndRunAttrSubstitution) {
  FunctionDefLibrary proto;
  *(proto.add_function()) = test::function::XTimesTwo();
  FunctionLibraryDefinition lib_def(OpRegistry::Global(), proto);

  Tensor y;
  auto x = test::AsTensor<float>({1, 2, 3, 4});
  TF_EXPECT_OK(InstantiateAndRun(
      "XTimesTwo", lib_def,
      {{"T", DT_FLOAT}, {"_target", "/job:localhost/replica:0/task:1/cpu:0"}},
      {x}, {&y}));
  test::ExpectTensorEqual<float>(y, test::AsTensor<float>({2, 4, 6, 8}));
}

}  // namespace tensorflow
