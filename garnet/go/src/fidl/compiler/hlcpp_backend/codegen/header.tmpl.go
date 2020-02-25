// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package codegen

const headerTemplate = `
{{- define "Header" -}}
// WARNING: This file is machine generated by fidlgen.

#pragma once

#include "lib/fidl/cpp/internal/header.h"

{{ range .Headers -}}
#include <{{ . }}>
{{ end -}}

{{- range .Library }}
namespace {{ . }} {
{{- end }}
{{ "" }}

{{- range .Decls }}
{{- if Eq .Kind Kinds.Bits }}{{ template "BitsForwardDeclaration" . }}{{- end }}
{{- if Eq .Kind Kinds.Enum }}{{ template "EnumForwardDeclaration" . }}{{- end }}
{{- if Eq .Kind Kinds.Interface }}{{ template "DispatchInterfaceForwardDeclaration" . }}{{- end }}
{{- if Eq .Kind Kinds.Service }}{{ template "DispatchServiceForwardDeclaration" . }}{{- end }}
{{- if Eq .Kind Kinds.Struct }}{{ template "StructForwardDeclaration" . }}{{- end }}
{{- if Eq .Kind Kinds.Table }}{{ template "TableForwardDeclaration" . }}{{- end }}
{{- if Eq .Kind Kinds.Union }}{{ template "UnionForwardDeclaration" . }}{{- end }}
{{- end }}

{{- range .Decls }}
{{- if Eq .Kind Kinds.Const }}{{ template "ConstDeclaration" . }}{{- end }}
{{- if Eq .Kind Kinds.Interface }}{{ template "DispatchProtocolDeclaration" . }}{{- end }}
{{- if Eq .Kind Kinds.Service }}{{ template "DispatchServiceDeclaration" . }}{{- end }}
{{- if Eq .Kind Kinds.Struct }}{{ template "StructDeclaration" . }}{{- end }}
{{- if Eq .Kind Kinds.Table }}{{ template "TableDeclaration" . }}{{- end }}
{{- if Eq .Kind Kinds.Union }}{{ template "UnionDeclaration" . }}{{- end }}
{{- end }}

{{- range .LibraryReversed }}
}  // namespace {{ . }}
{{- end }}
namespace fidl {
{{ "" }}

{{- range .Decls }}
{{- if Eq .Kind Kinds.Bits }}{{ template "BitsTraits" . }}{{- end }}
{{- if Eq .Kind Kinds.Enum }}{{ template "EnumTraits" . }}{{- end }}
{{- if Eq .Kind Kinds.Interface }}{{ template "DispatchProtocolTraits" . }}{{- end }}
{{- if Eq .Kind Kinds.Struct }}{{ template "StructTraits" . }}{{- end }}
{{- if Eq .Kind Kinds.Table }}{{ template "TableTraits" . }}{{- end }}
{{- if Eq .Kind Kinds.Union }}{{ template "UnionTraits" . }}{{- end }}
{{- end -}}

}  // namespace fidl
{{ end }}

{{- define "DispatchInterfaceForwardDeclaration" -}}
{{- range $transport, $_ := .Transports }}
{{- if eq $transport "Channel" -}}{{ template "InterfaceForwardDeclaration" $ }}{{- end }}
{{- end }}
{{- end -}}

{{- define "DispatchProtocolDeclaration" -}}
{{- range $transport, $_ := .Transports }}
{{- if eq $transport "Channel" -}}{{ template "ProtocolDeclaration" $ }}{{- end }}
{{- end }}
{{- end -}}

{{- define "DispatchProtocolTraits" -}}
{{- range $transport, $_ := .Transports }}
{{- if eq $transport "Channel" -}}{{ template "ProtocolTraits" $ }}{{- end }}
{{- end }}
{{- end -}}

{{- define "DispatchServiceForwardDeclaration" -}}
{{- range $transport, $_ := .Transports }}
{{- if eq $transport "Channel" -}}{{ template "ServiceForwardDeclaration" $ }}{{- end }}
{{- end }}
{{- end }}

{{- define "DispatchServiceDeclaration" -}}
{{- range $transport, $_ := .Transports }}
{{- if eq $transport "Channel" -}}{{ template "ServiceDeclaration" $ }}{{- end }}
{{- end }}
{{- end }}
`
