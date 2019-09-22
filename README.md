# Coarse Grained Timers Cache Attacks
This repository contains the source code of: 
* Proof-of-concept of the how to create cache eviction sets.
* Proof-of-concept of covert channel
As described in the paper "Coarse Grained Timers Cache Attacks".

# Compilation

 * WASM with WebAssemblyThreads:

    emcc -O0 toolkit.c vlist.c -s WASM=1 -s TOTAL_MEMORY=655360000 -o toolkit.js -DTHREADS -s USE_PTHREADS=1 -s PTHREAD_POOL_SIZE=2

 * WASM without threads:

      emcc -O0 toolkit.c vlist.c -s WASM=1 -s TOTAL_MEMORY=655360000 -o toolkit.js`

 * Native Process (for debug purposes):	

     gcc -O0 toolkit.c vlist.c -DNATIVE -o toolkit

## Running
After compilation, run main.html in your browser. It has been tested in Chrome / Firefox latest stable versions as of September 2019.
