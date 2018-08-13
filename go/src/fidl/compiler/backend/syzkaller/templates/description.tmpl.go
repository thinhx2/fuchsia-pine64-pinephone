// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package templates

const SyscallDescription = `
{{- define "GenerateSyscallDescription" -}}
# Copyright 2018 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be found in the LICENSE file.
#
# WARNING: This file is machine generated by fidlgen.

{{ range $interface := .Interfaces -}}
{{ template "InterfaceDefinition" $interface }}
{{ end -}}

{{- end -}}
`
