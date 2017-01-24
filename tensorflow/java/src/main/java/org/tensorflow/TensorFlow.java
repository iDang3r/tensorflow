/* Copyright 2016 The TensorFlow Authors. All Rights Reserved.

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

package org.tensorflow;

/** Static utility methods describing the TensorFlow runtime. */
public final class TensorFlow {
  /** Returns the version of the underlying TensorFlow runtime. */
  public static native String version();

  private TensorFlow() {}

  /** Load the TensorFlow runtime C library. */
  static void init() {
    try {
      System.loadLibrary("tensorflow_jni");
    } catch (UnsatisfiedLinkError e) {
      // The native code might have been statically linked (through a custom launcher) or be part of
      // an application-level library. For example, tensorflow/examples/android and
      // tensorflow/contrib/android include the required native code in differently named libraries.
      // To allow for such cases, the UnsatisfiedLinkError does not bubble up here.
      try {
        version();
      } catch (UnsatisfiedLinkError e2) {
        System.err.println(
            "TensorFlow Java API methods will throw an UnsatisfiedLinkError unless native code shared libraries are loaded");
      }
    }
  }

  static {
    init();
  }
}
