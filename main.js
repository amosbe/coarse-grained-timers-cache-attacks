function transferUintToHeap(arr) {
 const uintArray = toUintArr(arr);
 heapSpace = Module._malloc(uintArray.length * uintArray.BYTES_PER_ELEMENT); // 1
 Module.HEAPU32.set(uintArray, heapSpace >> 2); // 2 
 return heapSpace;
 function toUintArr(arr) {
 const res = new Uint32Array(arr.length); // 3 
     for (let i=0; i < arr.length; i++)
         res[i] = arr[i];
     return res;
 }
}
//uint32_t EMSCRIPTEN_KEEPALIVE chainTimeList(uint32_t * lines, int n, int steps){
function chainTimeList(lines,steps) {
 let linesHeap = transferUintToHeap(lines);
 let res = _chainTimeList(linesHeap,lines.length,steps);
 Module._free(linesHeap);		
 return res;
}

//void EMSCRIPTEN_KEEPALIVE sendbits(uint32_t * values, int len,int setidx, int offset,int lines,uint64_t duration){
function sendbits(arr,setidx,offset,lines,duration) {
 let values = transferUintToHeap(arr);
 _sendbits(values,arr.length,setidx,offset,lines,duration);
 Module._free(values);
}

//void EMSCRIPTEN_KEEPALIVE sendbits_new(uint32_t * values, int len,int setidx,int offset,int lines,uint64_t dur)
function sendbits_new(arr,setidx,offset,lines,duration) {
 let values = transferUintToHeap(arr);
 _sendbits_new(values,arr.length,setidx,offset,lines,duration);
 Module._free(values);
}

//uint32_t * EMSCRIPTEN_KEEPALIVE receive_new(int setidx, int offset, int dur, int nsamples, int steps, int lines)
function receive_new(setidx,offset,dur,nsamples,lines) {
 let tmp = _receive_new(setidx,offset,dur,nsamples, lines);
 let res = [];
 for (let i = 0; i < nsamples; i++)
	 res.push(Module.HEAPU32[tmp/Uint32Array.BYTES_PER_ELEMENT+i])
 Module._free(tmp);
 return res;
}

//uint32_t * EMSCRIPTEN_KEEPALIVE receive(int * setsidx, int * offsets, int len, int nsamples, int steps, int lines)
function receive(setslist,offsetlist,times,steps,lines) {
 if (setslist.length != offsetlist.length)
	 return "length not match";
 let setsarray = transferUintToHeap(setslist);
 let offsetsarray = transferUintToHeap(offsetlist);

 let tmp = _receive(setsarray,offsetsarray,setslist.length, times, steps, lines);
 let res = [];
 for (let i = 0; i < times*setslist.length*2; i++)
	 res.push(Module.HEAPU32[tmp/Uint32Array.BYTES_PER_ELEMENT+i])
 
 Module._free(setsarray);
 Module._free(offsetsarray);
 Module._free(tmp);
 return [res.slice(0,times),res.slice(times,res.length)];
}

let rec_setsarray;
let rec_offsetsarray;
let rec_times;
let rec_len;
function receive_up(setslist,offsetlist,times,steps,lines) {
 if (setslist.length != offsetlist.length)
 return "length not match";
 rec_len = setslist.length;
 rec_times = times;
 rec_setsarray = transferUintToHeap(setslist);
 rec_offsetsarray = transferUintToHeap(offsetlist);
 _receive_up(rec_setsarray,rec_offsetsarray,rec_len, times, steps, lines);

}
function receive_down() {
 let tmp = _receive_down();
 let res = [];
 for (let i = 0; i < rec_times*rec_len*2; i++)
	 res.push(Module.HEAPU32[tmp/Uint32Array.BYTES_PER_ELEMENT+i])
 
 Module._free(tmp);
 Module._free(rec_setsarray);
 Module._free(rec_offsetsarray);
 return [res.slice(0,rec_times),res.slice(rec_times,res.length)];

}

//void EMSCRIPTEN_KEEPALIVE manualAddL3Set(uint32_t * lines, int n)
function manualAddL3Set(lines) {
 let array = transferUintToHeap(lines);
 _manualAddL3Set(array,lines.length);
 Module._free(array);
}