; RUN: llvm-as < %s | llc -march=x86 | not grep fstp
; RUN: llvm-as < %s | llc -march=x86 -mcpu=yonah | not grep movsd

declare double @foo()

define double @bar() {
entry:
	%tmp5 = tail call double @foo()
	ret double %tmp5
}

