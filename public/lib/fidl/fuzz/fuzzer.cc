// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "test.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    TestParserByteArrayInput(data, size);
    return 0;
}
