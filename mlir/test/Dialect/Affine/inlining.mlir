// RUN: mlir-opt -allow-unregistered-dialect %s -inline="disable-simplify" | FileCheck %s

// Basic test that functions within affine operations are inlined.
func @func_with_affine_ops(%N: index) {
  %c = constant 200 : index
  affine.for %i = 1 to 10 {
    affine.if affine_set<(i)[N] : (i - 2 >= 0, 4 - i >= 0)>(%i)[%c]  {
      %w = affine.apply affine_map<(d0,d1)[s0] -> (d0+d1+s0)> (%i, %i) [%N]
    }
  }
  return
}

// CHECK-LABEL: func @inline_with_affine_ops
func @inline_with_affine_ops() {
  %c = constant 1 : index

  // CHECK: affine.for
  // CHECK-NEXT: affine.if
  // CHECK-NEXT: affine.apply
  // CHECK-NOT: call
  call @func_with_affine_ops(%c) : (index) -> ()
  return
}

// CHECK-LABEL: func @not_inline_in_affine_op
func @not_inline_in_affine_op() {
  %c = constant 1 : index

  // CHECK-NOT: affine.if
  // CHECK: call
  affine.for %i = 1 to 10 {
    call @func_with_affine_ops(%c) : (index) -> ()
  }
  return
}

// -----

// Test when an invalid operation is nested in an affine op.
func @func_with_invalid_nested_op() {
  affine.for %i = 1 to 10 {
    "foo.opaque"() : () -> ()
  }
  return
}

// CHECK-LABEL: func @not_inline_invalid_nest_op
func @not_inline_invalid_nest_op() {
  // CHECK: call @func_with_invalid_nested_op
  call @func_with_invalid_nested_op() : () -> ()
  return
}

// -----

// Test that calls are inlined into affine structures.
func @func_noop() {
  return
}

// CHECK-LABEL: func @inline_into_affine_ops
func @inline_into_affine_ops() {
  // CHECK-NOT: call @func_noop
  affine.for %i = 1 to 10 {
    call @func_noop() : () -> ()
  }
  return
}

// -----

// Test that calls with dimension arguments are properly inlined.
func @func_dim(%arg0: index, %arg1: memref<?xf32>) {
  affine.load %arg1[%arg0] : memref<?xf32>
  return
}

// CHECK-LABEL: @func_dim
func @inline_dimension(%arg0: memref<?xf32>) {
  // CHECK: affine.for %[[IV:.*]] =
  affine.for %i = 1 to 42 {
    // CHECK-NOT: call @func_dim
    // CHECK: affine.load %{{.*}}[%[[IV]]]
    call @func_dim(%i, %arg0) : (index, memref<?xf32>) -> ()
  }
  return
}

// -----

// Test that calls that would result in violation of affine value
// categorization (top-level value stop being top-level) are not inlined.
func private @get_index() -> index

func @func_top_level(%arg0: memref<?xf32>) {
  %0 = call @get_index() : () -> index
  affine.load %arg0[%0] : memref<?xf32>
  return
}

// CHECK-LABEL: @no_inline_not_top_level
func @no_inline_not_top_level(%arg0: memref<?xf32>) {
  affine.for %i = 1 to 42 {
    // CHECK: call @func_top_level
    call @func_top_level(%arg0) : (memref<?xf32>) -> ()
  }
  return
}
