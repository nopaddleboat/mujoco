// Copyright 2021 DeepMind Technologies Limited
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

#ifndef MUJOCO_SRC_ENGINE_ENGINE_CROSSPLATFORM_H_
#define MUJOCO_SRC_ENGINE_ENGINE_CROSSPLATFORM_H_

// IWYU pragma: begin_keep
#if !defined(__cplusplus)
  #include <stddef.h>
  #include <stdlib.h>
#else
  #include <cstddef>
  #include <cstdlib>
#endif
// IWYU pragma: end_keep

// Windows
#ifdef _WIN32
  #define strcasecmp _stricmp
  #define strncasecmp _strnicmp

  #define mjQUICKSORT(buf, elnum, elsz, func, context) \
      qsort_s(buf, elnum, elsz, func, context)
  #define quicksortfunc(name, context, el1, el2) \
      static int name(void* context, const void* el1, const void* el2)

// Unix-common
#else
  #include <strings.h>

  // Apple
  #ifdef __APPLE__
    #define mjQUICKSORT(buf, elnum, elsz, func, context) \
        qsort_r(buf, elnum, elsz, context, func)
    #define quicksortfunc(name, context, el1, el2) \
        static int name(void* context, const void* el1, const void* el2)

  // non-Apple
  #else
    #define mjQUICKSORT(buf, elnum, elsz, func, context) \
        qsort_r(buf, elnum, elsz, func, context)
    #define quicksortfunc(name, context, el1, el2) \
        static int name(const void* el1, const void* el2, void* context)
  #endif
#endif

#if defined(__cplusplus)
  #define mjFALLTHROUGH [[fallthrough]]
#elif defined(__clang__) || (defined(__GNUC__) && __GNUC__ >= 7)
  #define mjFALLTHROUGH __attribute__((fallthrough))
#else
  #define mjFALLTHROUGH ((void) 0)
#endif

#if defined(_MSC_VER) && !defined(__clang__) && !defined(__cplusplus)
  typedef long double max_align_t;
#endif

#endif  // MUJOCO_SRC_ENGINE_ENGINE_CROSSPLATFORM_H_
