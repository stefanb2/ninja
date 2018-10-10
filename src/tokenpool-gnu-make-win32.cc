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

#include "util.h"

// TokenPool implementation for GNU make jobserver - Win32 implementation
// (https://www.gnu.org/software/make/manual/html_node/Windows-Jobserver.html)
struct GNUmakeTokenPoolWin32 : public GNUmakeTokenPool {
  GNUmakeTokenPoolWin32();
  virtual ~GNUmakeTokenPoolWin32();

  virtual bool IOCPWithToken(HANDLE ioport, PULONG_PTR key);

  virtual bool ParseAuth(const char *jobserver);
  virtual bool AcquireToken();
  virtual bool ReturnToken();

 private:
  HANDLE startup_;
  HANDLE semaphore_;
  HANDLE ioport_;

  DWORD SemaphoreThread();
  static DWORD WINAPI SemaphoreThreadWrapper(LPVOID param);
  static void NoopAPCFunc(ULONG_PTR param);
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

DWORD GNUmakeTokenPoolWin32::SemaphoreThread() {
  // indicate to parent thread that child thread has started
  if (!ReleaseSemaphore(startup_, 1, NULL))
    Win32Fatal("ReleaseSemaphore/startup");

  // alertable wait forever on token semaphore
  if (WaitForSingleObjectEx(semaphore_, INFINITE, TRUE) == WAIT_OBJECT_0) {
    // release token again for AcquireToken()
    if (!ReleaseSemaphore(semaphore_, 1, NULL))
      Win32Fatal("ReleaseSemaphore/token");

    // indicate to parent thread on ioport that a token might be available
    if (!PostQueuedCompletionStatus(ioport_, 0, (ULONG_PTR) this, NULL))
      Win32Fatal("PostQueuedCompletionStatus");
  }

  return 0;
}

DWORD WINAPI GNUmakeTokenPoolWin32::SemaphoreThreadWrapper(LPVOID param) {
  GNUmakeTokenPoolWin32 *This = (GNUmakeTokenPoolWin32 *) param;
  return This->SemaphoreThread();
}

void GNUmakeTokenPoolWin32::NoopAPCFunc(ULONG_PTR param) {
}

bool GNUmakeTokenPoolWin32::IOCPWithToken(HANDLE ioport, PULONG_PTR key) {
  // subprocess-win32.cc uses I/O completion port (IOCP) which can't be
  // used as a waitable object. Therefore we can't use WaitMultipleObjects()
  // to wait on the IOCP and the token semaphore at the same time.
  HANDLE thread;
  DWORD bytes_read;
  OVERLAPPED* overlapped;

  // create thread that waits on token semaphore
  ioport_  = ioport;
  startup_ = CreateSemaphore(NULL, 0, 1, NULL);
  if (startup_ == NULL)
    Win32Fatal("CreateSemaphore");
  if ((thread = CreateThread(NULL, 0, &SemaphoreThreadWrapper, this, 0, NULL))
      == NULL)
    Win32Fatal("CreateThread");

  // wait for child thread to release startup semaphore
  if (WaitForSingleObject(startup_, INFINITE) != WAIT_OBJECT_0)
    Win32Fatal("WaitForSingleObject/startup");
  CloseHandle(startup_);
  startup_ = NULL;

  // now child thread waits on token semaphore and we wait on IOCP...
  if (!GetQueuedCompletionStatus(ioport, &bytes_read, key,
                                 &overlapped, INFINITE)) {
    if (GetLastError() != ERROR_BROKEN_PIPE)
      Win32Fatal("GetQueuedCompletionStatus");
  }

  // alert child thread and wait for it to exit
  QueueUserAPC(&NoopAPCFunc, thread, (ULONG_PTR)NULL);
  if (WaitForSingleObject(thread, INFINITE) != WAIT_OBJECT_0)
    Win32Fatal("WaitForSingleObject/exit");
  CloseHandle(thread);

  return *key == (ULONG_PTR) this;
}

struct TokenPool *TokenPool::Get() {
  return new GNUmakeTokenPoolWin32;
}
