; RUN: llc < %s -mtriple=aarch64-none-linux-gnu  | FileCheck %s

define i8* @t() nounwind {
entry:
; CHECK-LABEL: t:
; CHECK: mov x0, xsp
	%0 = call i8* @llvm.stackpointer()
        ret i8* %0
}

define i8* @t2() nounwind {
entry:
; CHECK-LABEL: t2:
; CHECK: ldr x[[reg:[0-9]+]], [xsp]
; CHECK: ldr x[[reg]], [x[[reg]]]
	%0 = call i8* @llvm.stackpointer()
        ret i8* %0
}

declare i8* @llvm.stackpointer() nounwind readnone
