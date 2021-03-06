; RUN: llvm-as -o - %s | llc -march=cellspu > %t1.s
; RUN: grep shlh   %t1.s | count 84
; RUN: grep shlhi  %t1.s | count 51
; RUN: grep shl    %t1.s | count 168
; RUN: grep shli   %t1.s | count 51
; RUN: grep xshw   %t1.s | count 5
; RUN: grep and    %t1.s | count 5
target datalayout = "E-p:32:32:128-f64:64:128-f32:32:128-i64:32:128-i32:32:128-i16:16:128-i8:8:128-i1:8:128-a0:0:128-v128:128:128-s0:128:128"
target triple = "spu"

; Vector shifts are not currently supported in gcc or llvm assembly. These are
; not tested.

; Shift left i16 via register, note that the second operand to shl is promoted
; to a 32-bit type:

define i16 @shlh_i16_1(i16 %arg1, i16 %arg2) {
        %A = shl i16 %arg1, %arg2
        ret i16 %A
}

define i16 @shlh_i16_2(i16 %arg1, i16 %arg2) {
        %A = shl i16 %arg2, %arg1
        ret i16 %A
}

define i16 @shlh_i16_3(i16 signext %arg1, i16 signext %arg2) signext {
        %A = shl i16 %arg1, %arg2
        ret i16 %A
}

define i16 @shlh_i16_4(i16 signext %arg1, i16 signext %arg2) signext {
        %A = shl i16 %arg2, %arg1
        ret i16 %A
}

define i16 @shlh_i16_5(i16 zeroext %arg1, i16 zeroext %arg2) zeroext {
        %A = shl i16 %arg1, %arg2
        ret i16 %A
}

define i16 @shlh_i16_6(i16 zeroext %arg1, i16 zeroext %arg2) zeroext {
        %A = shl i16 %arg2, %arg1
        ret i16 %A
}

; Shift left i16 with immediate:
define i16 @shlhi_i16_1(i16 %arg1) {
        %A = shl i16 %arg1, 12
        ret i16 %A
}

; Should not generate anything other than the return, arg1 << 0 = arg1
define i16 @shlhi_i16_2(i16 %arg1) {
        %A = shl i16 %arg1, 0
        ret i16 %A
}

define i16 @shlhi_i16_3(i16 %arg1) {
        %A = shl i16 16383, %arg1
        ret i16 %A
}

; Should generate 0, 0 << arg1 = 0
define i16 @shlhi_i16_4(i16 %arg1) {
        %A = shl i16 0, %arg1
        ret i16 %A
}

define i16 @shlhi_i16_5(i16 signext %arg1) signext {
        %A = shl i16 %arg1, 12
        ret i16 %A
}

; Should not generate anything other than the return, arg1 << 0 = arg1
define i16 @shlhi_i16_6(i16 signext %arg1) signext {
        %A = shl i16 %arg1, 0
        ret i16 %A
}

define i16 @shlhi_i16_7(i16 signext %arg1) signext {
        %A = shl i16 16383, %arg1
        ret i16 %A
}

; Should generate 0, 0 << arg1 = 0
define i16 @shlhi_i16_8(i16 signext %arg1) signext {
        %A = shl i16 0, %arg1
        ret i16 %A
}

define i16 @shlhi_i16_9(i16 zeroext %arg1) zeroext {
        %A = shl i16 %arg1, 12
        ret i16 %A
}

; Should not generate anything other than the return, arg1 << 0 = arg1
define i16 @shlhi_i16_10(i16 zeroext %arg1) zeroext {
        %A = shl i16 %arg1, 0
        ret i16 %A
}

define i16 @shlhi_i16_11(i16 zeroext %arg1) zeroext {
        %A = shl i16 16383, %arg1
        ret i16 %A
}

; Should generate 0, 0 << arg1 = 0
define i16 @shlhi_i16_12(i16 zeroext %arg1) zeroext {
        %A = shl i16 0, %arg1
        ret i16 %A
}

; Shift left i32 via register, note that the second operand to shl is promoted
; to a 32-bit type:

define i32 @shl_i32_1(i32 %arg1, i32 %arg2) {
        %A = shl i32 %arg1, %arg2
        ret i32 %A
}

define i32 @shl_i32_2(i32 %arg1, i32 %arg2) {
        %A = shl i32 %arg2, %arg1
        ret i32 %A
}

define i32 @shl_i32_3(i32 signext %arg1, i32 signext %arg2) signext {
        %A = shl i32 %arg1, %arg2
        ret i32 %A
}

define i32 @shl_i32_4(i32 signext %arg1, i32 signext %arg2) signext {
        %A = shl i32 %arg2, %arg1
        ret i32 %A
}

define i32 @shl_i32_5(i32 zeroext %arg1, i32 zeroext %arg2) zeroext {
        %A = shl i32 %arg1, %arg2
        ret i32 %A
}

define i32 @shl_i32_6(i32 zeroext %arg1, i32 zeroext %arg2) zeroext {
        %A = shl i32 %arg2, %arg1
        ret i32 %A
}

; Shift left i32 with immediate:
define i32 @shli_i32_1(i32 %arg1) {
        %A = shl i32 %arg1, 12
        ret i32 %A
}

; Should not generate anything other than the return, arg1 << 0 = arg1
define i32 @shli_i32_2(i32 %arg1) {
        %A = shl i32 %arg1, 0
        ret i32 %A
}

define i32 @shli_i32_3(i32 %arg1) {
        %A = shl i32 16383, %arg1
        ret i32 %A
}

; Should generate 0, 0 << arg1 = 0
define i32 @shli_i32_4(i32 %arg1) {
        %A = shl i32 0, %arg1
        ret i32 %A
}

define i32 @shli_i32_5(i32 signext %arg1) signext {
        %A = shl i32 %arg1, 12
        ret i32 %A
}

; Should not generate anything other than the return, arg1 << 0 = arg1
define i32 @shli_i32_6(i32 signext %arg1) signext {
        %A = shl i32 %arg1, 0
        ret i32 %A
}

define i32 @shli_i32_7(i32 signext %arg1) signext {
        %A = shl i32 16383, %arg1
        ret i32 %A
}

; Should generate 0, 0 << arg1 = 0
define i32 @shli_i32_8(i32 signext %arg1) signext {
        %A = shl i32 0, %arg1
        ret i32 %A
}

define i32 @shli_i32_9(i32 zeroext %arg1) zeroext {
        %A = shl i32 %arg1, 12
        ret i32 %A
}

; Should not generate anything other than the return, arg1 << 0 = arg1
define i32 @shli_i32_10(i32 zeroext %arg1) zeroext {
        %A = shl i32 %arg1, 0
        ret i32 %A
}

define i32 @shli_i32_11(i32 zeroext %arg1) zeroext {
        %A = shl i32 16383, %arg1
        ret i32 %A
}

; Should generate 0, 0 << arg1 = 0
define i32 @shli_i32_12(i32 zeroext %arg1) zeroext {
        %A = shl i32 0, %arg1
        ret i32 %A
}
