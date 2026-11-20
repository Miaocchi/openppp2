# VMUX fuzz targets

[中文交接](HANDOFF_CN.md)

These targets exercise untrusted VMUX reliability and flow-control bytes without
starting a PPP process or changing host network state. They are optional and are
not registered with CTest because libFuzzer targets run until their budget ends.

```sh
cmake -S tests/cpp -B build/vmux-fuzz -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release -DENABLE_VMUX_FUZZ=ON
cmake --build build/vmux-fuzz --target \
  vmux_ack_frame_fuzz vmux_fec_frame_fuzz vmux_flow_control_fuzz --parallel 3

build/vmux-fuzz/vmux_ack_frame_fuzz -runs=10000
build/vmux-fuzz/vmux_fec_frame_fuzz -runs=10000
build/vmux-fuzz/vmux_flow_control_fuzz -runs=10000
```

The targets use Clang libFuzzer with AddressSanitizer and UndefinedBehaviorSanitizer.
Seed corpora can be passed as directories; crashing inputs should be retained as
regression artifacts and reviewed before being added to a permanent corpus.
