# Implementation ownership

Private decoder, architectural state transitions and native JIT lowering belong
here. No execution implementation exists yet. Keep LLVM and host backend details
out of public headers; implement generic processor behavior, not device firmware.
