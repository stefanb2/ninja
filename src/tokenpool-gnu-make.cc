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

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// TokenPool implementation for GNU make jobserver
// (http://make.mad-scientist.net/papers/jobserver-implementation/)
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

  struct sigaction old_act_;
  bool restore_;

  static bool interrupted_;
  static void SetInterruptedFlag(int signum);

  bool CheckFd(int fd);
  bool SetAlarmHandler();
#endif

  void Return();
};

// every instance owns an implicit token -> available_ == 1
GNUmakeTokenPool::GNUmakeTokenPool() : available_(1), used_(0),
                                       rfd_(-1), wfd_(-1), restore_(false) {
}

GNUmakeTokenPool::~GNUmakeTokenPool() {
  Clear();
  if (restore_)
    sigaction(SIGALRM, &old_act_, NULL);
}

bool GNUmakeTokenPool::CheckFd(int fd) {
  if (fd < 0)
    return false;
  int ret = fcntl(fd, F_GETFD);
  if (ret < 0)
    return false;
  return true;
}

bool GNUmakeTokenPool::interrupted_;

void GNUmakeTokenPool::SetInterruptedFlag(int signum) {
  interrupted_ = true;
}

bool GNUmakeTokenPool::SetAlarmHandler() {
  struct sigaction act;
  memset(&act, 0, sizeof(act));
  act.sa_handler = SetInterruptedFlag;
  if (sigaction(SIGALRM, &act, NULL) < 0) {
    perror("sigaction:");
    return(false);
  } else {
    restore_ = true;
    return(true);
  }
}

bool GNUmakeTokenPool::Setup() {
  const char *value = getenv("MAKEFLAGS");
  if (value) {
    // GNU make <= 4.1
    const char *jobserver = strstr(value, "--jobserver-fds=");
    // GNU make => 4.2
    if (!jobserver)
      jobserver = strstr(value, "--jobserver-auth=");
    if (jobserver) {
      int rfd = -1;
      int wfd = -1;
      if ((sscanf(jobserver, "%*[^=]=%d,%d", &rfd, &wfd) == 2) &&
          CheckFd(rfd) &&
          CheckFd(wfd) &&
          SetAlarmHandler()) {
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

    interrupted_ = false;
    alarm(1);
    int ret = read(rfd_, &buf, 1);
    alarm(0);

    if (ret > 0) {
      available_++;
      return true;
    }

    if (interrupted_)
      perror("blocked on token");
  }
  return false;
}

void GNUmakeTokenPool::Reserve() {
  available_--;
  used_++;
}

void GNUmakeTokenPool::Return() {
  const char buf = '+';
  while (1) {
    int ret = write(wfd_, &buf, 1);
    if (ret > 0)
      available_--;
    if ((ret != -1) || (errno != EINTR))
      return;
    // write got interrupted - retry
  }
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
