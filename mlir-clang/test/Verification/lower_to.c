// RUN: mlir-clang %s --function=foo | FileCheck %s

#pragma lower_to(bar, "std.addf")
extern float bar(float a, float b);
float foo(float a, float b) { return bar(a, b); }

// CHECK: func @foo(%[[ARG0:.*]]: f32, %[[ARG1:.*]]: f32)
// CHECK-NEXT: %[[VAL0:.*]] = addf %[[ARG0]], %[[ARG1]]
// CHECK-NEXT: return %[[VAL0]]
