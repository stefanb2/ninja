// Copyright 2016 Google Inc. All Rights Reserved.
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

struct TokenPool {
  TokenPool();
  ~TokenPool();

  bool Setup();
  bool Acquire();
  void Reserve();
  void Release();
  void Clear();

 private:
  bool acquired_;
  int used_;

#ifdef _WIN32
  // @TODO
#else
  int rfd_;
  int wfd_;

  bool CheckFd(int fd);
#endif

  void Return();
};
