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

#include <windows.h>

#include "line_printer.h"

// TokenPool implementation for GNU make jobserver - Win32 implementation
// (https://www.gnu.org/software/make/manual/html_node/Windows-Jobserver.html)
struct GNUmakeTokenPool : public TokenPool {
  GNUmakeTokenPool();
  virtual ~GNUmakeTokenPool();

  virtual bool Acquire();
  virtual void Reserve();
  virtual void Release();
  virtual void Clear();

  bool Setup(bool ignore, bool verbose, double& max_load_average);

 private:
  int available_;
  int used_;

  HANDLE semaphore_;

  void Return();
};

// every instance owns an implicit token -> available_ == 1
GNUmakeTokenPool::GNUmakeTokenPool() : available_(1), used_(0),
                                       semaphore_(NULL) {
}

GNUmakeTokenPool::~GNUmakeTokenPool() {
  Clear();
  CloseHandle(semaphore_);
  semaphore_ = NULL;
}

bool GNUmakeTokenPool::Setup(bool ignore,
                             bool verbose,
                             double& max_load_average) {
  const char *value = getenv("MAKEFLAGS");
  if (value) {
    const char *jobserver = strstr(value, "--jobserver-auth=");
    if (jobserver) {
      LinePrinter printer;

      if (ignore) {
        printer.PrintOnNewLine("ninja: warning: -jN forced on command line; ignoring GNU make jobserver.\n");
      } else {
        char *auth = NULL;
        // matches "--jobserver-auth=gmake_semaphore_<INTEGER>..."
        if ((sscanf(jobserver, "%*[^=]=%m[a-z0-9_]", &auth) == 1) &&
            (semaphore_ = OpenSemaphore(SEMAPHORE_ALL_ACCESS,      /* Semaphore access setting */
                                        FALSE,                     /* Child processes DON'T inherit */
                                        auth                       /* Semaphore name */
                                        )) != NULL) {
          const char *l_arg = strstr(value, " -l");
          int load_limit = -1;

          free(auth);

          if (verbose) {
            printer.PrintOnNewLine("ninja: using GNU make jobserver.\n");
          }

          // translate GNU make -lN to ninja -lN
          if (l_arg &&
              (sscanf(l_arg + 3, "%d ", &load_limit) == 1) &&
              (load_limit > 0)) {
            max_load_average = load_limit;
          }

          return true;
        }
        free(auth);
      }
    }
  }

  return false;
}

bool GNUmakeTokenPool::Acquire() {
  if (available_ > 0)
    return true;

  if (WaitForSingleObject(semaphore_, 0) == WAIT_OBJECT_0) {
    // token acquired
    available_++;
    return true;
  } else {
    // no token available
    return false;
  }
}

void GNUmakeTokenPool::Reserve() {
  available_--;
  used_++;
}

void GNUmakeTokenPool::Return() {
  if (ReleaseSemaphore(semaphore_,
                       1,          /* increase count by one */
                       NULL))      /* not interested in previous count */
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

struct TokenPool *TokenPool::Get(bool ignore,
                                 bool verbose,
                                 double& max_load_average) {
  GNUmakeTokenPool *tokenpool = new GNUmakeTokenPool;
  if (tokenpool->Setup(ignore, verbose, max_load_average))
    return tokenpool;
  else
    delete tokenpool;
  return NULL;
}
