// RUN: sair-opt %s -sair-introduce-loops | FileCheck %s

func.func @foo(%arg0: index, %arg1: index) { return }

// CHECK-LABEL: @map
// CHECK: %[[ARG0:.*]]: index
func.func @map(%arg0: index) {
  sair.program {
    // CHECK: %[[V0:.*]] = sair.from_scalar %[[ARG0]]
    %0 = sair.from_scalar %arg0 { instances = [{}] } : !sair.value<(), index>
    %1 = sair.dyn_range %0 { instances = [{}] } : !sair.dyn_range
    %2 = sair.static_range { instances = [{}] } : !sair.static_range<8>
    // CHECK: sair.map %[[V0]] attributes
    // CHECK-SAME: {instances = [{loop_nest = []}]} {
    sair.map[d0: %1, d1: %2] attributes {
      instances = [{
        loop_nest = [
          {name = "A", iter = #sair.mapping_expr<d1>},
          {name = "B", iter = #sair.mapping_expr<d0>}
        ]
      }]
    } {
      // CHECK: ^{{.*}}(%[[ARG1:.*]]: index):
      ^bb0(%arg1: index, %arg2: index):
        // CHECK-DAG: %[[C0:.*]] = arith.constant 0 : index
        // CHECK-DAG: %[[C1:.*]] = arith.constant 1 : index
        // CHECK-DAG: %[[C8:.*]] = arith.constant 8 : index
        // CHECK: scf.for %[[V1:.*]] = %[[C0]] to %[[C8]] step %[[C1]] {
        // CHECK-DAG:   %[[C2:.*]] = arith.constant 0 : index
        // CHECK-DAG:   %[[C3:.*]] = arith.constant 1 : index
        // CHECK:   scf.for %[[V2:.*]] = %[[C2]] to %[[ARG1]] step %[[C3]] {
        // CHECK:     call @foo(%[[V2]], %[[V1]]) : (index, index) -> ()
                      func.call @foo(%arg1, %arg2) : (index, index) -> ()
        // CHECK:   }
        // CHECK: }
        // CHECK: sair.return
        sair.return
    // CHECK: } : #sair.shape<()>, (index) -> ()
    } : #sair.shape<d0:dyn_range x d1:static_range<8>>, () -> ()
    sair.exit { instances = [{}] }
  }
  func.return
}

// CHECK-LABEL: @proj_last
func.func @proj_last(%arg0: f32) {
  sair.program {
    %0 = sair.static_range { instances = [{}] }  : !sair.static_range<8>
    %1 = sair.from_scalar %arg0 { instances = [{}] }  : !sair.value<(), f32>
    // CHECK: %[[V0:.*]] = sair.map %{{.*}}
    %2 = sair.map[d0:%0] %1 attributes {
      instances = [{
        loop_nest = [{name = "A", iter = #sair.mapping_expr<d0>}]
      }]
    } {
      ^bb0(%arg1: index, %arg2: f32):
        sair.return %arg2: f32
    } : #sair.shape<d0:static_range<8>>, (f32) -> f32
    // CHECK-NOT: sair.proj_last
    %3 = sair.proj_last of[d0:%0] %2(d0)  { instances = [{}] } : #sair.shape<d0:static_range<8>>, f32
    // CHECK: sair.exit %[[V0]]
    sair.exit %3 { instances = [{}] } : f32
  } : f32
  func.return
}

func.func @bar(%arg0: f32) -> f32 { return %arg0 : f32 }

// CHECK-LABEL: @fby
func.func @fby(%arg0: f32) {
  sair.program {
    %0 = sair.static_range { instances = [{}] } : !sair.static_range<8>
    // CHECK: %[[V0:.*]] = sair.from_scalar
    %1 = sair.from_scalar %arg0 { instances = [{}] } : !sair.value<(), f32>
    // CHECK-NOT: sair.fby
    %2 = sair.fby %1 then[d0:%0] %3(d0) { instances = [{}] } : !sair.value<d0:static_range<8>, f32>
    // CHECK: %[[V1:.*]] = sair.map %[[V0]] attributes
    %3 = sair.map[d0: %0] %2(d0) attributes {
      instances = [{
        loop_nest = [{name = "A", iter = #sair.mapping_expr<d0>}],
        storage = [{space = "register", layout = #sair.named_mapping<[] -> ()>}]
      }]
    } {
    // CHECK: ^bb0(%[[V2:.*]]: f32):
      ^bb0(%arg1: index, %5: f32):
        %6 = func.call @bar(%5) : (f32) -> f32
        sair.return %6 : f32
    } : #sair.shape<d0:static_range<8>>, (f32) -> (f32)
    %4 = sair.proj_last of[d0:%0] %3(d0) { instances = [{}] } : #sair.shape<d0:static_range<8>>, f32
    sair.exit %4 { instances = [{}] } : f32
  } : f32
  func.return
}

// CHECK-LABEL: @fuse
func.func @fuse(%arg0: f32) {
  sair.program {
    %0 = sair.static_range { instances = [{}] } : !sair.static_range<4>
    %1 = sair.static_range { instances = [{}] } : !sair.static_range<8>
    // CHECK: %[[V0:.*]] = sair.from_scalar
    %2 = sair.from_scalar %arg0 { instances = [{}] } : !sair.value<(), f32>
    // CHECK: sair.map %[[V0]] attributes
    %3 = sair.map[d0:%0, d1:%1] attributes {
      instances = [{
        loop_nest = [
          {name = "A", iter = #sair.mapping_expr<d0>},
          {name = "B", iter = #sair.mapping_expr<d1>}
        ]
      }]
    } {
    // CHECK: ^{{.*}}(%[[ARG0:.*]]: f32):
      ^bb0(%arg1: index, %arg2: index):
        // CHECK: scf.for %[[I0:.*]] = %{{.*}} to %{{.*}}
        // CHECK: scf.for %[[I1:.*]] = %{{.*}} to %{{.*}}
        // CHECK: call @foo(%[[I0]], %[[I1]])
        func.call @foo(%arg1, %arg2) : (index, index) -> ()
        // CHECK: %[[V1:.*]] = arith.constant
        %4 = arith.constant 1.0 : f32
        sair.return %4 : f32
    } : #sair.shape<d0:static_range<4> x d1:static_range<8>>, () -> (f32)
    // CHECK-NOT: sair.map
    sair.map[d0:%1, d1:%0] %2, %3(d1, d0) attributes {
      instances = [{
        loop_nest = [
          {name = "A", iter = #sair.mapping_expr<d1>},
          {name = "B", iter = #sair.mapping_expr<d0>}
        ]
      }]
    } {
      ^bb0(%arg1:index, %arg2: index, %arg3: f32, %arg4: f32):
        // CHECK: call @foo(%[[I1]], %[[I0]])
        func.call @foo(%arg1, %arg2) : (index, index) -> ()
        // CHECK: call @bar(%[[ARG0]])
        func.call @bar(%arg3) : (f32) -> f32
        // CHECK: call @bar(%[[V1]])
        func.call @bar(%arg4) : (f32) -> f32
        sair.return
    } : #sair.shape<d0:static_range<8> x d1:static_range<4>>, (f32, f32) -> ()
    sair.exit { instances = [{}] }
  }
  func.return
}

// CHECK-LABEL: @fuse_reorder
func.func @fuse_reorder(%arg0: f32) {
  sair.program {
    %0 = sair.static_range { instances = [{}] } : !sair.static_range<8>
    %1 = sair.static_range { instances = [{}] } : !sair.static_range<16>
    %2 = sair.from_scalar %arg0 { instances = [{}] } : !sair.value<(), f32>
    // Check that loop introduction and fusion accounts for sequence numbers.
    // In particular, the body of the second map (sequence number 1) should
    // come first, and have a separate inner loop. The bodies of the first
    // (sequence number 2) and third (sequence number 3) map should be fused
    // and not include the body of the second map.
    // CHECK: sair.map
    // CHECK: ^{{.*}}(%[[ARG:.*]]: f32):
    // CHECK:   scf.for %[[IV1:.*]] = {{.*}} {
    // CHECK:     scf.for {{.*}} {
    // CHECK:       call @bar(%[[ARG]])
    // CHECK:     }
    // CHECK:     scf.for %[[IV2:.*]] = {{.*}} {
    // CHECK:       call @foo(%[[IV1]], %[[IV2]])
    // CHECK-NOT:   call @bar
    // CHECK:       constant 1
    // CHECK:       addf %[[ARG]]
    // CHECK:       index_cast
    // CHECK:       sitofp
    // CHECK:       call @bar
    // CHECK:     }
    // CHECK:   }
    %3 = sair.map[d0:%0, d1:%1] %2 attributes {
      instances = [{
        loop_nest = [
          {name = "A", iter = #sair.mapping_expr<d0>},
          {name = "C", iter = #sair.mapping_expr<d1>}
        ],
        sequence = 2
      }]
    } {
    ^bb0(%arg1: index, %arg2: index, %arg3: f32):
      func.call @foo(%arg1, %arg2) : (index, index) -> ()
      %4 = arith.constant 1.0 : f32
      %5 = arith.addf %arg3, %4 : f32
      sair.return %5 : f32
    } : #sair.shape<d0:static_range<8> x d1:static_range<16>>, (f32) -> (f32)
    %6 = sair.map[d0:%0, d1:%1] %2 attributes {
      instances = [{
        loop_nest = [
          {name = "A", iter = #sair.mapping_expr<d0>},
          {name = "B", iter = #sair.mapping_expr<d1>}
        ],
        sequence = 1
      }]
    } {
    ^bb0(%arg1: index, %arg2: index, %arg3: f32):
      %7 = func.call @bar(%arg3) : (f32) -> f32
      sair.return %7 : f32
    } : #sair.shape<d0:static_range<8> x d1:static_range<16>>, (f32) -> (f32)
    sair.map[d0:%0, d1:%1] attributes {
      instances = [{
        loop_nest = [
          {name = "A", iter = #sair.mapping_expr<d0>},
          {name = "C", iter = #sair.mapping_expr<d1>}
        ],
        sequence = 3
      }]
    } {
    ^bb0(%arg1: index, %arg2: index):
      %9 = arith.index_cast %arg1 : index to i32
      %10 = arith.sitofp %9 : i32 to f32
      func.call @bar(%10) : (f32) -> f32
      sair.return
    } : #sair.shape<d0:static_range<8> x d1:static_range<16>>, () -> ()
    sair.exit { instances = [{}] }
  }
  func.return
}

// CHECK-LABEL: @dependent_dims
func.func @dependent_dims() {
  sair.program {
    // CHECK: sair.map
      // CHECK-DAG: %[[V0:.*]] = arith.constant 0
      // CHECK-DAG: %[[V1:.*]] = arith.constant 64
      // CHECK-DAG: %[[V2:.*]] = arith.constant 8
      // CHECK: scf.for %[[V3:.*]] = %[[V0]] to %[[V1]] step %[[V2]] {
    %0 = sair.static_range { instances = [{}] } : !sair.static_range<64, 8>
    %1, %2 = sair.map[d0:%0] attributes {
      instances = [{
        loop_nest = [{name = "A", iter = #sair.mapping_expr<d0>}],
        storage = [
          {space = "register", layout = #sair.named_mapping<[] -> ()>},
          {space = "register", layout = #sair.named_mapping<[] -> ()>}
        ]
      }]
    } {
      ^bb0(%arg0: index):
        // CHECK: %[[V4:.*]] = arith.constant 8
        %4 = arith.constant 8 : index
        // CHECK: %[[V5:.*]] = arith.addi %[[V3]], %[[V4]]
        %5 = arith.addi %arg0, %4 : index
        sair.return %arg0, %5 : index, index
    } : #sair.shape<d0:static_range<64, 8>>, () -> (index, index)
        // CHECK: %[[V6:.*]] = arith.constant 1
    %3 = sair.dyn_range[d0:%0] %1(d0), %2(d0) { instances = [{}] } : !sair.dyn_range<d0:static_range<64, 8>>
        // CHECK: scf.for %[[V7:.*]] = %[[V3]] to %[[V5]] step %[[V6]] {
    sair.map[d0:%0, d1:%3] attributes {
      instances = [{
        loop_nest = [
          {name = "A", iter = #sair.mapping_expr<d0>},
          {name = "B", iter = #sair.mapping_expr<d1>}
        ]
      }]
    } {
      ^bb0(%arg0: index, %arg1: index):
          // CHECK: call @foo(%[[V3]], %[[V7]])
        func.call @foo(%arg0, %arg1) : (index, index) -> ()
        sair.return
    } : #sair.shape<d0:static_range<64, 8> x d1:dyn_range(d0)>, () -> ()
        // CHECK: }
      // CHECK: }
      // CHECK: sair.return
    // CHECK: } : #sair.shape<()>, () -> ()
    // CHECK: sair.exit
    sair.exit { instances = [{}] }
  }
  func.return
}

func.func private @baz()

// CHECK-LABEL: @full_unroll
func.func @full_unroll() {
  sair.program {
    %0 = sair.static_range { instances = [{}] } : !sair.static_range<3>
    // CHECK: sair.map
    // CHECK-SAME: loop_nest = []
    // CHECK-NOT: scf.for
    // CHECK-COUNT-3: call @baz()
    sair.map[d0:%0] attributes {
      instances = [{
        loop_nest = [
          {name = "A", iter = #sair.mapping_expr<d0>, unroll = 3}
        ]
      }]
    } {
    ^bb0(%arg0: index):
      func.call @baz() : () -> ()
      sair.return
    } : #sair.shape<d0:static_range<3>>, () -> ()
    sair.exit { instances = [{}] }
  }
  func.return
}

// CHECK-LABEL: @partial_unroll
func.func @partial_unroll() {
  sair.program {
    %0 = sair.static_range { instances = [{}] } : !sair.static_range<5>
    // CHECK: sair.map
    // CHECK-SAME: loop_nest = []
    // CHECK: %[[STEP:.*]] = arith.constant 2 : index
    // CHECK: scf.for %{{.*}} = %{{.*}} to %{{.*}} step %[[STEP]] {
    // CHECK-COUNT-2: call @baz()
    // CHECK: }
    // Epilogue loop is simplified.
    // CHECK-NOT: scf.for
    // CHECK: call @baz()
    sair.map[d0:%0] attributes {
      instances = [{
        loop_nest = [
          {name = "A", iter = #sair.mapping_expr<d0>, unroll = 2}
        ]
      }]
    } {
    ^bb0(%arg0: index):
      func.call @baz() : () -> ()
      sair.return
    } : #sair.shape<d0:static_range<5>>, () -> ()
    sair.exit { instances = [{}] }
  }
  func.return
}

// CHECK-LABEL: @dyn_range_unroll
func.func @dyn_range_unroll(%sz: index) {
  sair.program {
    %0 = sair.from_scalar %sz { instances = [{}] } : !sair.value<(), index>
    %1 = sair.dyn_range %0 { instances = [{}] } : !sair.dyn_range
    // CHECK: sair.map
    // CHECK-SAME: loop_nest = []
    // CHECK: scf.for {{.*}} {
    // CHECK-COUNT-2: call @baz()
    // CHECK: }
    // Epilogue loop.
    // CHECK: scf.for {{.*}} {
    // CHECK: call @baz()
    // CHECK: }
    sair.map[d0:%1] attributes {
      instances = [{
        loop_nest = [
          {name = "A", iter = #sair.mapping_expr<d0>, unroll = 2}
        ]
      }]
    } {
    ^bb0(%arg0: index):
      func.call @baz() : () -> ()
      sair.return
    } : #sair.shape<d0:dyn_range>, () -> ()
    sair.exit { instances = [{}] }
  }
  func.return
}

// CHECK-LABEL: @nested_unroll
func.func @nested_unroll() {
  sair.program {
    %0 = sair.static_range { instances = [{}] } : !sair.static_range<2>
    // CHECK: sair.map
    // CHECK-SAME: loop_nest = []
    // CHECK-NOT: scf.for
    // CHECK-COUNT-8: call @baz()
    sair.map[d0:%0, d1:%0, d2:%0] attributes {
      instances = [{
        loop_nest = [
          {name = "A", iter = #sair.mapping_expr<d0>, unroll = 2},
          {name = "B", iter = #sair.mapping_expr<d1>, unroll = 2},
          {name = "C", iter = #sair.mapping_expr<d2>, unroll = 2}
        ]
      }]
    } {
    ^bb0(%arg0: index, %arg1: index, %arg2: index):
      func.call @baz() : () -> ()
      sair.return
    } : #sair.shape<d0:static_range<2> x d1:static_range<2> x d2:static_range<2>>, () -> ()
    sair.exit { instances = [{}] }
  }
  func.return
}

