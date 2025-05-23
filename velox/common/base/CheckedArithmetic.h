/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <functional>
#include <string>
#include "folly/Likely.h"
#include "velox/common/base/Exceptions.h"

namespace facebook::velox {

template <typename T>
T checkedPlus(T a, T b, const char* typeName = "integer") {
  T result;
  bool overflow = __builtin_add_overflow(a, b, &result);
  if (UNLIKELY(overflow)) {
    VELOX_ARITHMETIC_ERROR("{} overflow: {} + {}", typeName, a, b);
  }
  return result;
}

template <typename T>
T checkedMinus(T a, T b, const char* typeName = "integer") {
  T result;
  bool overflow = __builtin_sub_overflow(a, b, &result);
  if (UNLIKELY(overflow)) {
    VELOX_ARITHMETIC_ERROR("{} overflow: {} - {}", typeName, a, b);
  }
  return result;
}

template <typename T>
T checkedMultiply(T a, T b, const char* typeName = "integer") {
  T result;
  bool overflow = __builtin_mul_overflow(a, b, &result);
  if (UNLIKELY(overflow)) {
    VELOX_ARITHMETIC_ERROR("{} overflow: {} * {}", typeName, a, b);
  }
  return result;
}

template <typename T>
T checkedDivide(T a, T b) {
  if (b == 0) {
    VELOX_ARITHMETIC_ERROR("division by zero");
  }

  // Type T can not represent abs(std::numeric_limits<T>::min()).
  if constexpr (std::is_integral_v<T>) {
    if (UNLIKELY(a == std::numeric_limits<T>::min() && b == -1)) {
      VELOX_ARITHMETIC_ERROR("integer overflow: {} / {}", a, b);
    }
  }
  return a / b;
}

template <typename T>
T checkedModulus(T a, T b) {
  if (UNLIKELY(b == 0)) {
    VELOX_ARITHMETIC_ERROR("Cannot divide by 0");
  }
  // std::numeric_limits<int64_t>::min() % -1 could crash the program since
  // abs(std::numeric_limits<int64_t>::min()) can not be represented in
  // int64_t.
  if (b == -1) {
    return 0;
  }
  return (a % b);
}

template <typename T>
T checkedNegate(T a) {
  if (UNLIKELY(a == std::numeric_limits<T>::min())) {
    VELOX_ARITHMETIC_ERROR("Cannot negate minimum value");
  }
  return std::negate<std::remove_cv_t<T>>()(a);
}

} // namespace facebook::velox
