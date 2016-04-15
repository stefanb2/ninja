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

#include "tokenpool.h"

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// TokenPool implementation for GNU make jobserver
struct GNUmakeTokenPool : public TokenPool {
  GNUmakeTokenPool();
  virtual ~GNUmakeTokenPool();

  virtual bool Acquire();
  virtual void Reserve();
  virtual void Release();
  virtual void Clear();

  bool Setup();

 private:
  int available_;
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

// every instance owns an implicit token -> available_ == 1
GNUmakeTokenPool::GNUmakeTokenPool() : available_(1), used_(0),
                                       rfd_(-1), wfd_(-1) {
}

GNUmakeTokenPool::~GNUmakeTokenPool() {
  Clear();
}

bool GNUmakeTokenPool::CheckFd(int fd) {
  if (fd < 0)
    return false;
  int ret = fcntl(fd, F_GETFD);
  if (ret < 0)
    return false;
  return true;
}

bool GNUmakeTokenPool::Setup() {
  const char *value = getenv("MAKEFLAGS");
  if (value) {
    const char *jobserver = strstr(value, "--jobserver-fds=");
    if (jobserver) {
      int rfd = -1;
      int wfd = -1;
      if ((sscanf(jobserver, "--jobserver-fds=%d,%d", &rfd, &wfd) == 2) &&
          CheckFd(rfd) &&
          CheckFd(wfd)) {
        fprintf(stderr, "FOUND JOBSERVER %d %d\n", rfd, wfd);
        rfd_ = rfd;
        wfd_ = wfd;
        return true;
      }
    }
  }

  return false;
}

bool GNUmakeTokenPool::Acquire() {
  if (available_ > 0)
    return true;

#ifdef USE_PPOLL
  pollfd pollfds[] = {{rfd_, POLLIN, 0}};
  int ret = poll(pollfds, 1, 0);
#else
  fd_set set;
  struct timeval timeout = { 0, 0 };
  FD_ZERO(&set);
  FD_SET(rfd_, &set);
  int ret = select(rfd_ + 1, &set, NULL, NULL, &timeout);
#endif
  if (ret > 0) {
    char buf;
    int ret = read(rfd_, &buf, 1);
    if (ret > 0) {
      available_++;
      return true;
    }
  }
  return false;
}

void GNUmakeTokenPool::Reserve() {
  available_--;
  used_++;
}

void GNUmakeTokenPool::Return() {
  const char buf = '+';
  if (write(wfd_, &buf, 1) > 0)
    available_--;
}

void GNUmakeTokenPool::Release() {
  available_++;
  used_--;
  if (available_ > 1)
    Return();
}

void GNUmakeTokenPool::Clear() {
  while (used_ > 0)
    Release();
  while (available_ > 1)
    Return();
}

struct TokenPool *TokenPool::Get(void) {
  GNUmakeTokenPool *tokenpool = new GNUmakeTokenPool;
  if (tokenpool->Setup())
    return tokenpool;
  else
    delete tokenpool;
  return NULL;
}
