# Copyright 2021 The BladeDISC Authors. All rights reserved.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
# http://www.apache.org/licenses/LICENSE-2.0
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# !/bin/bash
set -o pipefail
set -e
# bazel cache
export CXXFLAGS=${CXXFLAGS:-"-Wno-deprecated-dewlarations"}
export CFLAGS=${CFLAGS:-"-Wno-deprecated-declarations"}
export CUDA_HOME=${CUDA_HOME:-/usr/local/cuda/}
export CUDACXX=${CUDACXX:-"${CUDA_HOME}/bin/nvcc"}
export PATH=${CUDA_HOME}/bin/:$PATH
export LD_LIBRARY_PATH=${CUDA_HOME}/lib64:$LD_LIBRARY_PATH
export LIBRARY_PATH=${CUDA_HOME}/lib64:$LIBRARY_PATH

# Build TorchBlade with DEBUG
# export DEBUG=1

# To save time, set USE_BLADE_DISC_PRE_BUILD=ON if you has already built blade_disc
# export USE_BLADE_DISC_PRE_BUILD=ON

# To save time, set USE_MLIR_DHLO_PRE_BUILD=ON if you has already built llvm-project
# If the llvm-project has been built, it will be installed in ${PROJECT_SOURCE_DIR}/MLIR_DHLO_LLVM_DIR.
# export USE_MLIR_DHLO_PRE_BUILD=ON

export TORCH_BLADE_BUILD_MLIR_SUPPORT=ON
export TORCH_BLADE_BUILD_WITH_CUDA_SUPPORT=ON
function ci_build() {
    echo "DO TORCH_BLADE CI_BUILD"
    # set TORCH_BLADE_CI_BUILD_TORCH_VERSION default to 1.7.1+cu110
    TORCH_BLADE_CI_BUILD_TORCH_VERSION=${TORCH_BLADE_CI_BUILD_TORCH_VERSION:-1.7.1+cu110}
    requirements=requirements-dev-${TORCH_BLADE_CI_BUILD_TORCH_VERSION}.txt
    python3 -m pip install --upgrade pip
    python3 -m pip install cmake ninja virtualenv -r ${requirements}
    rm -rf build && python3 setup.py develop;
    # The following are UNIT TESTS
    export TORCH_BLADE_DEBUG_LOG=ON
    /bin/bash cpp_test.sh -V 2>&1 | tee -a build/cpp_test.out;
    python3 -m unittest discover tests/ -v 2>&1 | tee -a build/py_test.out;
}

# Build
ci_build
