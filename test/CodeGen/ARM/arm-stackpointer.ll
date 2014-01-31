; RUN: llc < %s -mtriple=arm-apple-darwin  | FileCheck %s
; RUN: llc < %s -mtriple=arm-linux-gnueabi | FileCheck %s

define i8* @t() nounwind {
entry:
; CHECK-LABEL: t:
; CHECK: mov r0, sp
	%0 = call i8* @llvm.stackpointer()
        ret i8* %0
}

declare i8* @llvm.stackpointer() nounwind readnone
