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

#include "velox/dwio/dwrf/writer/StatisticsBuilderUtils.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <velox/common/memory/HashStringAllocator.h>
#include <velox/common/memory/Memory.h>
#include <cmath>

using namespace facebook::velox::dwio::common;
using namespace facebook::velox;
using namespace facebook::velox::dwrf;

template <typename T>
std::shared_ptr<FlatVector<T>> makeFlatVector(
    facebook::velox::memory::MemoryPool* pool,
    BufferPtr nulls,
    size_t nullCount,
    size_t length,
    BufferPtr values) {
  return std::make_shared<FlatVector<T>>(
      pool,
      CppToType<T>::create(),
      nulls,
      length,
      values,
      std::vector<BufferPtr>{});
}

template <typename T>
std::shared_ptr<FlatVector<T>> makeFlatVectorNoNulls(
    facebook::velox::memory::MemoryPool* pool,
    size_t length,
    BufferPtr values) {
  return makeFlatVector<T>(pool, BufferPtr(nullptr), 0, length, values);
}

class TestStatisticsBuilderUtils : public testing::Test {
 protected:
  static void SetUpTestCase() {
    memory::MemoryManager::testingSetInstance(memory::MemoryManager::Options{});
  }

  void SetUp() override {
    StatisticsBuilderOptions options{16};
  }

  const std::shared_ptr<memory::MemoryPool> pool_ =
      memory::memoryManager()->addLeafPool();
  std::unique_ptr<HashStringAllocator> allocator_ =
      std::make_unique<HashStringAllocator>(pool_.get());
  StatisticsBuilderOptions options{16, 100, true, allocator_.get()};
};

TEST_F(TestStatisticsBuilderUtils, addIntegerValues) {
  IntegerStatisticsBuilder builder{options};

  // add values non null
  size_t size = 10;

  auto values = AlignedBuffer::allocate<int32_t>(size, pool_.get());
  auto* valuesPtr = values->asMutable<int32_t>();
  for (size_t i = 0; i < size; ++i) {
    valuesPtr[i] = i + 1;
  }

  auto vec = makeFlatVectorNoNulls<int32_t>(pool_.get(), size, values);
  {
    StatisticsBuilderUtils::addValues<int32_t>(
        builder, vec, common::Ranges::of(0, size));
    auto stats = builder.build();
    auto intStats = dynamic_cast<IntegerColumnStatistics*>(stats.get());
    EXPECT_EQ(10, intStats->getNumberOfValues());
    EXPECT_FALSE(intStats->hasNull().value());
    EXPECT_EQ(10, intStats->getMaximum().value());
    EXPECT_EQ(1, intStats->getMinimum().value());
    EXPECT_EQ(55, intStats->getSum());
    EXPECT_EQ(10, intStats->numDistinct());
  }

  // add values with null
  auto nulls = allocateNulls(size, pool_.get());
  bits::setNull(nulls->asMutable<uint64_t>(), 3);

  vec = makeFlatVector<int32_t>(pool_.get(), nulls, 1, size, values);

  {
    StatisticsBuilderUtils::addValues<int32_t>(
        builder, vec, common::Ranges::of(0, size));
    auto stats = builder.build();
    auto intStats = dynamic_cast<IntegerColumnStatistics*>(stats.get());
    EXPECT_EQ(19, intStats->getNumberOfValues());
    EXPECT_TRUE(intStats->hasNull().value());
    EXPECT_EQ(10, intStats->getMaximum().value());
    EXPECT_EQ(1, intStats->getMinimum().value());
    EXPECT_EQ(106, intStats->getSum().value());
    EXPECT_EQ(10, intStats->numDistinct());
  }
}

TEST_F(TestStatisticsBuilderUtils, addDoubleValues) {
  DoubleStatisticsBuilder builder{options};

  // add values non null
  size_t size = 10;

  auto values = AlignedBuffer::allocate<float>(size, pool_.get());
  auto* valuesPtr = values->asMutable<float>();
  for (size_t i = 0; i < size; ++i) {
    valuesPtr[i] = i + 1;
  }

  {
    auto vec = makeFlatVectorNoNulls<float>(pool_.get(), size, values);
    StatisticsBuilderUtils::addValues<float>(
        builder, vec, common::Ranges::of(0, size));
    auto stats = builder.build();
    auto doubleStats = dynamic_cast<DoubleColumnStatistics*>(stats.get());
    EXPECT_EQ(10, doubleStats->getNumberOfValues());
    EXPECT_FALSE(doubleStats->hasNull().value());
    EXPECT_EQ(10, doubleStats->getMaximum().value());
    EXPECT_EQ(1, doubleStats->getMinimum().value());
    EXPECT_EQ(55, doubleStats->getSum());
    EXPECT_EQ(10, doubleStats->numDistinct().value());
  }

  // add values with null
  auto nulls = allocateNulls(size, pool_.get());
  bits::setNull(nulls->asMutable<uint64_t>(), 3);

  {
    auto vec = makeFlatVector<float>(pool_.get(), nulls, 1, size, values);

    StatisticsBuilderUtils::addValues<float>(
        builder, vec, common::Ranges::of(0, size));
    auto stats = builder.build();
    auto doubleStats = dynamic_cast<DoubleColumnStatistics*>(stats.get());
    EXPECT_EQ(19, doubleStats->getNumberOfValues());
    EXPECT_TRUE(doubleStats->hasNull().value());
    EXPECT_EQ(10, doubleStats->getMaximum().value());
    EXPECT_EQ(1, doubleStats->getMinimum().value());
    EXPECT_EQ(106, doubleStats->getSum());
    EXPECT_EQ(10, doubleStats->numDistinct().value());
  }
}

TEST_F(TestStatisticsBuilderUtils, addStringValues) {
  StringStatisticsBuilder builder{options};

  // add values non null
  size_t size = 10;

  auto values = AlignedBuffer::allocate<StringView>(10, pool_.get());
  auto* valuesPtr = values->asMutable<StringView>();
  char c = 'a';
  for (size_t i = 0; i < size; ++i, ++c) {
    valuesPtr[i] = StringView(&c, 1);
  }

  {
    auto vec = makeFlatVectorNoNulls<StringView>(pool_.get(), size, values);
    StatisticsBuilderUtils::addValues(
        builder, vec, common::Ranges::of(0, size));
    auto stats = builder.build();
    auto strStats = dynamic_cast<StringColumnStatistics*>(stats.get());
    EXPECT_EQ(10, strStats->getNumberOfValues());
    EXPECT_FALSE(strStats->hasNull().value());
    EXPECT_EQ("j", strStats->getMaximum().value());
    EXPECT_EQ("a", strStats->getMinimum().value());
    EXPECT_EQ(10, strStats->getTotalLength());
    EXPECT_EQ(10, strStats->numDistinct());
  }

  // add values with null
  auto nulls = allocateNulls(size, pool_.get());
  bits::setNull(nulls->asMutable<uint64_t>(), 3);

  {
    auto vec = makeFlatVector<StringView>(pool_.get(), nulls, 1, size, values);
    StatisticsBuilderUtils::addValues(
        builder, vec, common::Ranges::of(0, size));
    auto stats = builder.build();
    auto strStats = dynamic_cast<StringColumnStatistics*>(stats.get());
    EXPECT_EQ(19, strStats->getNumberOfValues());
    EXPECT_TRUE(strStats->hasNull().value());
    EXPECT_EQ("j", strStats->getMaximum().value());
    EXPECT_EQ("a", strStats->getMinimum().value());
    EXPECT_EQ(19, strStats->getTotalLength().value());
    EXPECT_EQ(10, strStats->numDistinct());
  }
}

TEST_F(TestStatisticsBuilderUtils, addBooleanValues) {
  BooleanStatisticsBuilder builder{options};

  // add values non null
  size_t size = 10;

  auto values = AlignedBuffer::allocate<bool>(size, pool_.get());
  auto valuesPtr = values->asMutableRange<bool>();
  for (int32_t i = 0; i < size; ++i) {
    valuesPtr[i] = true;
  }
  valuesPtr[6] = false;

  {
    auto vec = makeFlatVectorNoNulls<bool>(pool_.get(), size, values);

    StatisticsBuilderUtils::addValues(
        builder, vec, common::Ranges::of(0, size));
    auto stats = builder.build();
    auto boolStats = dynamic_cast<BooleanColumnStatistics*>(stats.get());
    EXPECT_EQ(9, boolStats->getTrueCount().value());
    EXPECT_EQ(10, boolStats->getNumberOfValues());
    EXPECT_FALSE(boolStats->hasNull().value());
  }

  // add values with null
  auto nulls = allocateNulls(size, pool_.get());
  bits::setNull(nulls->asMutable<uint64_t>(), 3);

  {
    auto vec = makeFlatVector<bool>(pool_.get(), nulls, 1, size, values);

    StatisticsBuilderUtils::addValues(
        builder, vec, common::Ranges::of(0, size));
    auto stats = builder.build();
    auto boolStats = dynamic_cast<BooleanColumnStatistics*>(stats.get());
    EXPECT_EQ(17, boolStats->getTrueCount().value());
    EXPECT_EQ(19, boolStats->getNumberOfValues());
    EXPECT_TRUE(boolStats->hasNull().value());
  }
}

TEST_F(TestStatisticsBuilderUtils, addValues) {
  StatisticsBuilder builder{options};

  // add values non null
  size_t size = 10;

  auto values = AlignedBuffer::allocate<bool>(size, pool_.get());

  {
    auto vec = makeFlatVectorNoNulls<bool>(pool_.get(), size, values);

    StatisticsBuilderUtils::addValues(
        builder, vec, common::Ranges::of(0, size));
    auto stats = builder.build();
    EXPECT_EQ(10, stats->getNumberOfValues());
    EXPECT_FALSE(stats->hasNull().value());
  }

  // add values with null
  auto nulls = allocateNulls(size, pool_.get());
  bits::setNull(nulls->asMutable<uint64_t>(), 3);

  {
    auto vec = makeFlatVector<bool>(pool_.get(), nulls, 1, size, values);

    StatisticsBuilderUtils::addValues(
        builder, vec, common::Ranges::of(0, size));
    auto stats = builder.build();
    EXPECT_EQ(19, stats->getNumberOfValues());
    EXPECT_TRUE(stats->hasNull().value());
  }
}

TEST_F(TestStatisticsBuilderUtils, addBinaryValues) {
  BinaryStatisticsBuilder builder{options};

  // add values non null
  size_t size = 10;

  auto values = AlignedBuffer::allocate<StringView>(size, pool_.get());
  auto* valuesPtr = values->asMutable<StringView>();

  std::array<char, 10> data;
  std::memset(data.data(), 'a', 10);
  for (size_t i = 0; i < size; ++i) {
    valuesPtr[i] = StringView(data.data(), i + 1);
  }

  {
    auto vec = makeFlatVectorNoNulls<StringView>(pool_.get(), size, values);

    StatisticsBuilderUtils::addValues(
        builder, vec, common::Ranges::of(0, size));
    auto stats = builder.build();
    auto binStats = dynamic_cast<BinaryColumnStatistics*>(stats.get());
    EXPECT_EQ(10, binStats->getNumberOfValues());
    EXPECT_FALSE(binStats->hasNull().value());
    EXPECT_EQ(55, binStats->getTotalLength().value());
  }

  // add values with null
  auto nulls = allocateNulls(size, pool_.get());
  bits::setNull(nulls->asMutable<uint64_t>(), 3);

  {
    auto vec = makeFlatVector<StringView>(pool_.get(), nulls, 1, size, values);

    StatisticsBuilderUtils::addValues(
        builder, vec, common::Ranges::of(0, size));
    auto stats = builder.build();
    auto binStats = dynamic_cast<BinaryColumnStatistics*>(stats.get());
    EXPECT_EQ(19, binStats->getNumberOfValues());
    EXPECT_TRUE(binStats->hasNull().value());
    EXPECT_EQ(106, binStats->getTotalLength().value());
  }
}
