/*
 * Copyright 2016 WebAssembly Community Group participants
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

#ifndef WABT_TYPE_VECTOR_H_
#define WABT_TYPE_VECTOR_H_

#include "common.h"
#include "vector.h"

WABT_DEFINE_VECTOR(type, WabtType)

WABT_EXTERN_C_BEGIN

static WABT_INLINE WabtBool
wabt_type_vectors_are_equal(const WabtTypeVector* types1,
                            const WabtTypeVector* types2) {
  if (types1->size != types2->size)
    return WABT_FALSE;
  size_t i;
  for (i = 0; i < types1->size; ++i) {
    if (types1->data[i] != types2->data[i])
      return WABT_FALSE;
  }
  return WABT_TRUE;
}

WABT_EXTERN_C_END

#endif /* WABT_TYPE_VECTOR_H_ */
