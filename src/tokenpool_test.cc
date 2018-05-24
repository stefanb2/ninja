// Copyright 2018 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "tokenpool.h"

#include "test.h"

namespace {

const double kLoadAverageDefault = -1.23456789;

struct TokenPoolTest : public testing::Test {
  double load_avg_;
  TokenPool *tokens_;

  virtual void SetUp() {
    load_avg_ = kLoadAverageDefault;
    tokens_ = NULL;
  }

  void CreatePool(const char *format, bool ignore_jobserver) {
    tokens_ = TokenPool::Get(ignore_jobserver, false, load_avg_);
  }

  virtual void TearDown() {
    if (tokens_)
      delete tokens_;
  }
};

} // anonymous namespace

// verifies none implementation
TEST_F(TokenPoolTest, NoTokenPool) {
  CreatePool(NULL, false);

  EXPECT_EQ(NULL, tokens_);
  EXPECT_EQ(kLoadAverageDefault, load_avg_);
}
