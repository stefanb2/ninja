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

#include "tokenpool-gnu-make.h"

// always include first to make sure other headers do the correct thing...
#include <windows.h>

#include <stdio.h>

// TokenPool implementation for GNU make jobserver - Win32 implementation
// (https://www.gnu.org/software/make/manual/html_node/Windows-Jobserver.html)
struct GNUmakeTokenPoolWin32 : public GNUmakeTokenPool {
  GNUmakeTokenPoolWin32();
  virtual ~GNUmakeTokenPoolWin32();

  virtual bool ParseAuth(const char *jobserver);
  virtual bool AcquireToken();
  virtual bool ReturnToken();

 private:
  HANDLE semaphore_;
};

GNUmakeTokenPoolWin32::GNUmakeTokenPoolWin32() : semaphore_(NULL) {
}

GNUmakeTokenPoolWin32::~GNUmakeTokenPoolWin32() {
  Clear();
  CloseHandle(semaphore_);
  semaphore_ = NULL;
}

bool GNUmakeTokenPoolWin32::ParseAuth(const char *jobserver) {
  char *auth = NULL;
  // matches "--jobserver-auth=gmake_semaphore_<INTEGER>..."
  if ((sscanf(jobserver, "%*[^=]=%m[a-z0-9_]", &auth) == 1) &&
      ((semaphore_ = OpenSemaphore(SEMAPHORE_ALL_ACCESS, /* Semaphore access setting */
                                   FALSE,                /* Child processes DON'T inherit */
                                   auth                  /* Semaphore name */
                                   )) != NULL)) {
    free(auth);
    return true;
  }

  free(auth);
  return false;
}

bool GNUmakeTokenPoolWin32::AcquireToken() {
  return WaitForSingleObject(semaphore_, 0) == WAIT_OBJECT_0;
}

bool GNUmakeTokenPoolWin32::ReturnToken() {
  return ReleaseSemaphore(semaphore_,
                          1,          /* increase count by one */
                          NULL);      /* not interested in previous count */
}

struct TokenPool *TokenPool::Get(bool ignore,
                                 bool verbose,
                                 double& max_load_average) {
  GNUmakeTokenPool *tokenpool = new GNUmakeTokenPoolWin32;
  if (tokenpool->Setup(ignore, verbose, max_load_average))
    return tokenpool;
  else
    delete tokenpool;
  return NULL;
}
