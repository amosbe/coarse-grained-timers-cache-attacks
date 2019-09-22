/*
 * WASM with WebAssemblyThreads:	emcc -O0 toolkit.c vlist.c -s WASM=1 -s TOTAL_MEMORY=655360000 -o toolkit.js -DTHREADS -s USE_PTHREADS=1 -s PTHREAD_POOL_SIZE=2
 * WASM: 							emcc -O0 toolkit.c vlist.c -s WASM=1 -s TOTAL_MEMORY=655360000 -o toolkit.js
 * Native Process:					gcc -O0 toolkit.c vlist.c -DNATIVE -o toolkit
*/
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h> //getpid()
#include <fcntl.h>
#include <assert.h>
#include <time.h>
#include "vlist.h"
#include <pthread.h>
#include <signal.h>

#define STR_(X) #X
#define STR(X) STR_(X)

//#define NATIVE

#ifdef NATIVE
#define U64STR lu
#define EMSCRIPTEN_KEEPALIVE
#else
#define U64STR llu
#include <emscripten/emscripten.h>
#endif

#define NSETS 128
#define NSETSL2 8
#define NSETSL3_LOC 16
#define PAGESIZE 4096
#define LINESIZE 64
#define ADDR2RAWSET(x) (uint32_t)(((uint64_t)x >> 6) & 0x3f)  //bits 6-11
#define ADDR2SET(x) (uint32_t)(((uint64_t)x >> 6) & 0x7ff) //valid on physical only bits: 6-16
#define ADDR2SETL2(x) (uint32_t)(((uint64_t)x >> 6) & 0x1ff) //valid on physical only bits: 6-14
#define OFFSET(x,offset) (((uint64_t)x) & ~0xfff) + offset
#define OFFSETIDX(x,offset) x + offset*LINESIZE
#define L3WAYS 12
#define L2WAYS 8

#define POOLSIZE 7000//20000
#define L3_TRIES L3WAYS+1
#define L3_SAMPLES 1
#define L2_SAMPLES 3

#define L3_THRESH 40
#define L3_COLLECT_LOWTHRESH 40
#define L3_COLLECT_HIGHTHRESH 45
#define L3_STEPS 2000000

#define L2_THRESH 48
#define L2_STEPS 4e6
#define L2_THRESH_COLLECT 16
#define L2_STEPS_COLLECT 1e6

#ifdef THREADS
#define L3_THRESH_THREAD 340e3//todo
#define L3_STEPS_THREAD 5e4 //todo
#define L3_COLLECT_LOWTHRESH_THREAD L3_THRESH_THREAD//todo
#define L3_COLLECT_HIGHTHRESH_THREAD L3_THRESH_THREAD//todo

#define L2_THRESH_THREAD 41e5
#define L2_STEPS_THREAD 1e6
#define L2_THRESH_COLLECT_THREAD L2_THRESH_THREAD/10 //todo
#define L2_STEPS_COLLECT_THREAD L2_STEPS_THREAD/10 //todo
#endif

#ifdef NATIVE
#define L3_THRESH 40
#define L3_COLLECT_LOWTHRESH 40
#define L3_COLLECT_HIGHTHRESH 45
#define L3_STEPS 2000000
#define L2_THRESH 35
#define L2_THRESH_COLLECT 60
#define L2_STEPS_COLLECT 5000000
#endif

#define CLKSRC_MS 0
#define CLKSRC_THREAD 1

typedef struct receiveArgs {
	int * setsidx;
	int * offsets;
	int len;
	int nsamples;
	int steps;
	int lines;
} receiveArgs;

uint64_t l3_thresh_global = 				L3_THRESH;
uint64_t l3_collect_lowthresh_global =		L3_COLLECT_LOWTHRESH;
uint64_t l3_collect_highthresh_global =		L3_COLLECT_HIGHTHRESH;
int l3_steps_global	=						L3_STEPS;
uint64_t l2_thresh_global = 				L2_THRESH;
int l2_steps_global = 						L2_STEPS;
uint64_t l2_thresh_collect_global = 		L2_THRESH_COLLECT;
int l2_steps_collect_global = 				L2_STEPS_COLLECT;


int clocksrc = 0;

//Pthreads
#ifdef THREADS
pthread_t   bg_clock_thread;
uint64_t * bg_clock_val;
sig_atomic_t bg_clock_flag = 0;

pthread_t bg_receive_thread;

uint32_t * EMSCRIPTEN_KEEPALIVE receive(int * setsidx, int * offsets, int len, int nsamples, int steps, int lines);

void *bg_clock_func(void *arg)
{
	sig_atomic_t **bg_clock_value = (void *)arg;
	while(bg_clock_flag) {
		(**bg_clock_value)++;
	}
	return arg;
}

void *bg_receive_func(void *arg)
{
	receiveArgs args = *(receiveArgs*) arg;
	uint32_t * res = receive(args.setsidx, args.offsets, args.len,args.nsamples,args.steps,args.lines);
	/*printf("[");
	for (int i = 0; i < args.nsamples * 2; i++) {
		printf("%u",res[i]);
		if (i < args.nsamples-1) printf(",");
	}
	printf("]\n");*/
	free(arg);
	return res;
}

void EMSCRIPTEN_KEEPALIVE startClockThread()
{
	printf("startClockThread()\n");
	bg_clock_val  = malloc(sizeof(sig_atomic_t));
	if (!bg_clock_val)
		printf("malloc failed\n");
	*bg_clock_val = 0;

	if(bg_clock_flag) {
		printf("Already run");
		return;
	}
	bg_clock_flag = 1;
	if (pthread_create(&bg_clock_thread, NULL, bg_clock_func, &bg_clock_val)) {
		perror("Thread create failed");
		return;
	}
}



void EMSCRIPTEN_KEEPALIVE while1fu()
{
	while(1);
}

void EMSCRIPTEN_KEEPALIVE stopClockThread()
{
	printf("stopClockThread()\n");
	bg_clock_flag = 0;
	free(bg_clock_val);
	if (pthread_join(bg_clock_thread, NULL)) {
		perror("Thread join failed");
		return;
	}
}

void EMSCRIPTEN_KEEPALIVE resetThreadVal()
{
	*bg_clock_val = 0;
}

uint64_t EMSCRIPTEN_KEEPALIVE readThreadVal()
{
	return (uint64_t)*bg_clock_val;
}
#endif

void EMSCRIPTEN_KEEPALIVE setClockSrc(int src)
{
	switch(src) {
	case CLKSRC_MS:
		#ifdef THREADS
		if (bg_clock_flag) stopClockThread();
		#endif
		l3_thresh_global = 				L3_THRESH;
		l3_collect_lowthresh_global =	L3_COLLECT_LOWTHRESH;
		l3_collect_highthresh_global =	L3_COLLECT_HIGHTHRESH;
		l3_steps_global	=				L3_STEPS;
		l2_thresh_global = 				L2_THRESH;
		l2_steps_global = 				L2_STEPS;
		l2_thresh_collect_global = 		L2_THRESH_COLLECT;
		l2_steps_collect_global = 		L2_STEPS_COLLECT;
		break;
	case CLKSRC_THREAD:
		#ifdef THREADS
		if(!bg_clock_flag) startClockThread();
		l3_thresh_global = 				L3_THRESH_THREAD;
		l3_collect_lowthresh_global =	L3_COLLECT_LOWTHRESH_THREAD;
		l3_collect_highthresh_global =	L3_COLLECT_HIGHTHRESH_THREAD;
		l3_steps_global	=				L3_STEPS_THREAD;
		l2_thresh_global = 				L2_THRESH_THREAD;
		l2_steps_global = 				L2_STEPS_THREAD;
		l2_thresh_collect_global = 		L2_THRESH_COLLECT_THREAD;
		l2_steps_collect_global = 		L2_STEPS_COLLECT_THREAD;
		#endif
		break;

	}
	clocksrc = src;
}

char * buffer;

vlist_t sets[NSETS] = {0};
int nsets = 0;

vlist_t setsl2[NSETSL2] = {0};
int nsetsl2 = 0;

vlist_t pool;

uint32_t bits_to_reduct = 0;
void EMSCRIPTEN_KEEPALIVE setBitsToReduct(int val){
	bits_to_reduct = val;
}

struct timespec ts;
uint64_t last_clocksample;
static inline uint64_t EMSCRIPTEN_KEEPALIVE clocksamplems()
{
	clock_gettime(CLOCK_REALTIME,&ts);
	uint64_t res =  (uint64_t)(ts.tv_nsec+ts.tv_sec*1e9);
	last_clocksample = (res & (uint64_t)~0xFFFFF) >> 20;
	return last_clocksample;
}



static inline uint64_t EMSCRIPTEN_KEEPALIVE clocksample()
{
	switch (clocksrc) {
	case CLKSRC_MS: {
		return clocksamplems();
	}
	break;
#ifdef THREADS
	case CLKSRC_THREAD: {
		return readThreadVal() >> bits_to_reduct;
	}
	break;
#endif
	}
	return 0;
}

void EMSCRIPTEN_KEEPALIVE testclock(){
	uint64_t timestamps1 [9999] = {0};
	uint64_t timestamps2 [9999] = {0};

	for(int i = 0; i < 9999; i++){
		timestamps1[i] = clocksamplems();
		timestamps2[i] = clocksample();
	}

	for(int i = 0; i < 9999; i++){
		printf("%" STR(U64STR) ",",timestamps1[i]);
	}
	printf("\n");
	printf("---------------------\n");
	for(int i = 0; i < 9999; i++){
		printf("%" STR(U64STR) ",",timestamps2[i]);
	}
	printf("\n");
}

static inline uint64_t EMSCRIPTEN_KEEPALIVE clocksampleEdge()
{
	#ifdef THREADS
	if (CLKSRC_THREAD == clocksrc)
		return readThreadVal() >> bits_to_reduct;
	#endif
	uint64_t t,s;
	s = last_clocksample;
	do t = clocksample();
	while (t==s);
	return t;
}

static inline void EMSCRIPTEN_KEEPALIVE syncclockms(){
	uint64_t t,s;
	s = last_clocksample;
	do t = clocksamplems();
	while (t==s);
	#ifdef THREADS
	resetThreadVal();
	#endif
}

void shuffle(char **array, uint64_t n)
{
	if (n <= 1)
		return;
	uint64_t i;
	for (i = 0; i < n - 1; i++) {
		uint64_t j = i + rand() / (RAND_MAX / (n - i) + 1);
		char * t = array[j];
		array[j] = array[i];
		array[i] = t;
	}
}

void printvlist(vlist_t list)
{
	printf("[");
	for (int i = 0; i < vl_len(list); i++) {
		printf("%d",vl_get(list,i));
		if(i < vl_len(list)-1) printf(",");
	}
	printf("]\n");
}

//note: must run in sudo - only for Native Mode
uint64_t getPhysicalAddr(uint64_t virtual_addr)
{
#ifdef NATIVE
	int g_pagemap_fd = - 1;
	uint64_t value;
	off_t offset = (virtual_addr / 4096) * sizeof(value);
	g_pagemap_fd = open("/proc/self/pagemap", O_RDONLY);
	assert(g_pagemap_fd >= 0);
	int got = pread(g_pagemap_fd, &value, sizeof(value), offset);
	assert(got == 8);
	// Check the "page present" flag.
	assert(value & (1ULL << 63));

	uint64_t frame_num = value & ((1ULL << 54) - 1);
	close(g_pagemap_fd);

	return (frame_num * 4096) | (virtual_addr & (4095));
#else
	return 0;
#endif
}

int EMSCRIPTEN_KEEPALIVE max(int x, int y)
{
	if (x > y)
		return x;
	else
		return y;
}

int EMSCRIPTEN_KEEPALIVE min(int x, int y)
{
	if (x < y)
		return x;
	else
		return y;
}

element_t sethead(vlist_t list,int offset,int lines)
{
	if (!lines)	lines = vl_len(list);
	for (int i = 0; i < vl_len(list); i++)
		*(element_t*)&buffer[OFFSETIDX(vl_get(list,i),offset)] = OFFSETIDX(vl_get(list,(i+1)%lines),offset);

	return OFFSETIDX(vl_get(list,0),offset);
}

uint32_t timedWalkSteps(vlist_t vl, int offset, int lines, int time)
{
	element_t start = sethead(vl,offset,lines);
	uint64_t delta = time;
	uint64_t st;
	uint32_t accesscounter = 0;
	int curr = *(int*)&buffer[start];
	st = clocksampleEdge();
	printf("st = %" STR(U64STR) "\n",st);
	while(clocksample()-st < delta){
		for(int i = 1e3; i; i--){
			curr = *(int*)&buffer[curr] * 2;
			curr /= 2;
		}
		accesscounter++;
	}
	printf("accesscounter = %u\n",accesscounter);
	return accesscounter;
}

uint32_t EMSCRIPTEN_KEEPALIVE timedWalkStepsSet(int setidx, int offset, int lines, int time){
	return timedWalkSteps(sets[setidx],offset,lines,time);
}

//int hits, misses;
//uint64_t sum, count;
void walkSteps(int start, int steps)
{
	int curr = *(int*)&buffer[start];
	for(int i = 0; i < steps; i++) {
		curr = *(int*)&buffer[curr] * 2;
		curr /= 2;
		//curr = ((int*)buffer)[curr>>2];
		//printf("curr = %d\n",curr/PAGESIZE);
	}
}

void EMSCRIPTEN_KEEPALIVE prime(int setidx,int offset,int rounds)  //L3
{
	if (setidx >= nsets) {
		printf("No such L3 set\n");
		return;
	}
	vlist_t es = sets[setidx];
	element_t start = sethead(es,offset,L3WAYS);
	walkSteps(start,rounds * L3WAYS);
}

void walkPrint(int start, int steps)
{
	int curr = *(int*)&buffer[start];
	for(int i = 0; i < steps; i++) {
		printf("curr = %d, vaddr = %p\n",curr,&buffer[curr]);
		curr = *(int*)&buffer[curr] * 2;
		curr /= 2;
	}
}
void chainPrint(vlist_t list, int offset, int lines)
{
	element_t start = sethead(list,offset,lines);
	walkPrint(start,lines*2);
}

void EMSCRIPTEN_KEEPALIVE chainPrintL2(int setidx,int offset, int lines)
{
	if (setidx >= nsetsl2) {
		printf("No such L2 set\n");
		return;
	}
	chainPrint(setsl2[setidx],offset,lines);
}

uint64_t EMSCRIPTEN_KEEPALIVE slowProbeLine(element_t el, int steps)
{
	uint64_t s,e;
	*(element_t*)&buffer[el] = el;
	s = clocksampleEdge();
	walkSteps(el,steps);
	e = clocksample();
	return e-s;
}


uint64_t slowProbe(vlist_t list, int steps, int offset, int lines)
{
	element_t start = sethead(list,offset,lines);
	uint64_t s,e;
	s = clocksampleEdge();
	walkSteps(start,steps);
	e = clocksample();
	return e-s;
}




void EMSCRIPTEN_KEEPALIVE manualAddL3Set(uint32_t * lines, int n)
{
	vlist_t tmp = vl_new();
	for (int i = 0; i < n; i++)
		vl_push(tmp,lines[i]);
	sets[nsets++] = tmp;
}


uint32_t EMSCRIPTEN_KEEPALIVE slowProbeList(uint32_t * lines, int n, int steps)
{
	vlist_t tmp = vl_new();
	for (int i = 0; i < n; i++)
		vl_push(tmp,lines[i]);
	uint32_t res = (uint32_t)slowProbe(tmp, steps, 0, 0);
	while(vl_len(tmp)) vl_pop(tmp);
	vl_free(tmp);
	return res;
}


uint64_t EMSCRIPTEN_KEEPALIVE slowProbeL2(int setidx,int offset, int lines)
{
	if (setidx >= nsetsl2) {
		printf("No such L2 set\n");
		return 0;
	}
	return slowProbe(setsl2[setidx],l2_steps_global,offset,lines);
}

#ifdef THREADS
uint64_t slowProbeSAB(vlist_t list, int steps, int offset, int lines)
{
	element_t start = sethead(list,offset,lines);
	uint64_t s,e;
	s = readThreadVal();
	walkSteps(start,steps);
	e = readThreadVal();
	return e-s;
}


uint64_t EMSCRIPTEN_KEEPALIVE slowProbeL2SAB(int setidx,int offset, int lines,int steps)
{
	if (setidx >= nsetsl2) {
		printf("No such L2 set\n");
		return 0;
	}
	return slowProbeSAB(setsl2[setidx],steps,offset,lines);
}
#endif

uint64_t EMSCRIPTEN_KEEPALIVE slowProbeL3(int setidx,int offset, int lines)
{
	if (setidx >= nsets) {
		printf("No such L3 set\n");
		return 0;
	}
	return slowProbe(sets[setidx],l3_steps_global,offset,lines);
}

uint64_t EMSCRIPTEN_KEEPALIVE slowProbeL3steps(int setidx,int offset, int lines,int steps)
{
	if (setidx >= nsets) {
		printf("No such L3 set\n");
		return 0;
	}
	return slowProbe(sets[setidx],steps,offset,lines);
}

uint64_t EMSCRIPTEN_KEEPALIVE lineVsL2(int line,int setidx,int offset)
{
	if (setidx >= nsetsl2) {
		printf("No such L2 set\n");
		return 0;
	}
	uint64_t ct;
	if (vl_len(setsl2[setidx]) < L2WAYS)
		return 0;
	vlist_t tmp = vl_new();
	for (int i = 0; i < L2WAYS; i++) {
		vl_push(tmp,vl_get(setsl2[setidx],i));
	}
	vl_push(tmp,line);
	slowProbe(tmp,l2_steps_global,offset,0);
	ct = slowProbe(tmp,l2_steps_global,offset,0);
	while(vl_len(tmp)) vl_pop(tmp);
	vl_free(tmp);

	return ct;
}

uint64_t EMSCRIPTEN_KEEPALIVE lineVsL3(int line,int setidx,int offset)
{
	if (setidx >= nsets) {
		printf("No such L3 set\n");
		return 0;
	}
	uint64_t ct;
	if (vl_len(sets[setidx]) < L3WAYS)
		return 0;
	vlist_t tmp = vl_new();
	for (int i = 0; i < L3WAYS; i++) {
		vl_push(tmp,vl_get(sets[setidx],i));
	}
	vl_push(tmp,line);
	slowProbe(tmp,l3_steps_global,offset,0);
	ct = slowProbe(tmp,l3_steps_global,offset,0);
	while(vl_len(tmp)) vl_pop(tmp);
	vl_free(tmp);

	return ct;
}

void printHistL2(vlist_t list)
{
#ifdef NATIVE
	//printf("printHistL2(%d)\n",vl_len(list));
	int hist[8] = {0};
	for (int i = 0; i < vl_len(list); i++) {
		uintptr_t addr = getPhysicalAddr((uint64_t)&buffer[vl_get(list,i)]);
		int l2set = ADDR2SETL2(addr);
		hist[l2set/64]++;
		//printf("%d) %p\t%d\n",i,(void*)addr,l2set);
	}
	printf("L2 Histogram: ");
	for (int i = 0; i < 8; i ++) {
		printf("%d",hist[i]);
		if (i < 7)
			printf(",");
		else
			printf("\n");
	}
#endif
}

void printHistL3RS(vlist_t list)  // just rawsets
{
#ifdef NATIVE
	//printf("printHistL2(%d)\n",vl_len(list));
	int hist[4] = {0};
	for (int i = 0; i < vl_len(list); i++) {
		uintptr_t addr = getPhysicalAddr((uint64_t)&buffer[vl_get(list,i)]);
		int rawset = ADDR2SET(addr);
		hist[rawset>>9]++;
		//printf("%d) %p\t%d\n",i,(void*)addr,rawset>>9);
	}
	printf("RAWSETS Histogram: ");
	for (int i = 0; i < 4; i ++) {
		printf("%d",hist[i]);
		if (i < 3)
			printf(",");
		else
			printf("\n");
	}
#endif
}


void verifyCollect(vlist_t list, int offset, int n, int steps, vlist_t uncollected, int thresh)
{
	//printf("verifyCollect(%d,%d,%d,%d,%d,%d)\n",vl_len(list),offset,n,steps,uncollected ? vl_len(uncollected) : -1,thresh);
	printHistL2(list);
	printHistL3RS(list);
	vlist_t tmp = vl_new();
	vlist_t toDel = vl_new();
	uint64_t ct;
	int i,j;
	for (i = 0; i < vl_len(list)/n+1; i++) {
		int from = -i, to = n-i;
		for (j = from; j < to; j++) {
			//printf("push %d (%d) \n",(i*n+j),(i*n+j)%vl_len(list));
			vl_push(tmp,vl_get(list,(i*n+j)%vl_len(list)));
		}
		ct = slowProbe(tmp,steps,offset,0);
		if (ct < thresh) {
			while(vl_len(tmp)) vl_push(toDel,vl_pop(tmp));
		}
		printf("%d - %d: %" STR(U64STR) " %s\n",(i*n+from)%vl_len(list),(i*n+to-1)%vl_len(list),ct,ct < thresh ? "Delete!" : "");
		while(vl_len(tmp)) vl_pop(tmp);
	}
	element_t x;
	while(vl_len(toDel)) {
		x = vl_pop(toDel);
		vl_del(list,vl_find(list,x));
		vl_push(uncollected,x);
	}
	vl_free(toDel);
	vl_free(tmp);
	//printf("end()\n");

}

void EMSCRIPTEN_KEEPALIVE verifyCollectL2(int setidx, int offset)
{
	if (setidx >= nsetsl2) {
		printf("No such L2 set\n");
		return;
	}
	verifyCollect(setsl2[setidx],offset,L2WAYS+1,l2_steps_global,NULL,0);
}

void EMSCRIPTEN_KEEPALIVE verifyCollectL3(int setidx, int offset)
{
	if (setidx >= nsets) {
		printf("No such L3 set\n");
		return;
	}
	verifyCollect(sets[setidx],offset,L3WAYS+1,l3_steps_global,NULL,0);
}

void profilerl2_sudo(vlist_t candidates)
{
	int measurments = 10;
	uint64_t profiles[65][measurments];
	vlist_t lines;
	int profile_steps = l2_steps_global;
TEST1:
	printf("----------test 1------------\n");
	lines = vl_new();
	for (int i = 0; i < 65; i++) {
		printf("vl_len(lines) = %d\n",vl_len(lines) );

		vl_push(lines,vl_poprand(candidates));
		slowProbe(lines,profile_steps,0,0);
		for (int j = 0; j < measurments; j ++)
			profiles[i][j] = slowProbe(lines,profile_steps,0,0);
	}
	printf("[");
	for (int i = 0; i < 65; i++) {
		//printf("%d lines:\n",i);
		for (int j = 0; j < measurments; j ++) {
			if(j == 0)printf("[");
			printf("%" STR(U64STR) "",profiles[i][j]);
			if(j != measurments-1)printf(",");
			if(j == measurments-1)printf("]");
			if(j == measurments-1 && i!=64)printf(",");
		}
		printf("\n");
	}
	printf("]\n\n");

	int hist[8] = {0};
	for (int i = 0; i < 65; i++) {
		uintptr_t addr = getPhysicalAddr((uint64_t)&buffer[vl_get(lines,i)]);
		printf("%d) %p %d ",i+1,(void*)addr,ADDR2SETL2(addr));
		hist[ADDR2SETL2(addr)/64]++;
		if(hist[ADDR2SETL2(addr)/64] == 9)
			printf("************");
		printf("(%d)\n",hist[ADDR2SETL2(addr)/64]);
	}
	vl_free(lines);

TEST2:
	printf("----------test 2------------\n");
	lines = vl_new();
	for(int i = 0; i < 8; i++) hist[i] = 0;
	while(true) {
		printf("vl_len(lines) = %d\n",vl_len(lines) );
		element_t x = vl_pop(candidates);
		uintptr_t addr = getPhysicalAddr((uint64_t)&buffer[x]);
		if(hist[ADDR2SETL2(addr)/64] < 8) {
			vl_push(lines,x);
			slowProbe(lines,profile_steps,0,0);
			for (int i =0; i < measurments; i++) {
				profiles[vl_len(lines)-1][i] = slowProbe(lines,profile_steps,0,0);
			}
			hist[ADDR2SETL2(addr)/64]++;
		}
		int flag = 0;
		for(int i = 0; i < 8; i++)
			if(hist[i] < 8)
				flag = 1;
		if (flag) continue;
		else break;
	}
	for(int i = 0 ; i < 8; i ++)printf("hist[%i] = %d\n",i,hist[i]);
	element_t x = vl_pop(candidates);
	vl_push(lines,x);

	for (int i =0; i < measurments; i++) {
		slowProbe(lines,profile_steps,0,0);
		profiles[vl_len(lines)-1][i] = slowProbe(lines,profile_steps,0,0);
	}

	printf("[");
	for (int i = 0; i < 65; i++) {
		//printf("%d lines:\n",i);
		for (int j = 0; j < measurments; j ++) {
			if(j == 0)printf("[");
			printf("%" STR(U64STR) "",profiles[i][j]);
			if(j != measurments-1)printf(",");
			if(j == measurments-1)printf("]");
			if(j == measurments-1 && i!=64)printf(",");
		}
		printf("\n");
	}
	printf("]\n\n");
	for(int i = 0; i < 8; i++) hist[i] = 0;
	for (int i = 0; i < vl_len(lines); i++) {
		uintptr_t addr = getPhysicalAddr((uint64_t)&buffer[vl_get(lines,i)]);
		printf("%d) %p %d ",i+1,(void*)addr,ADDR2SETL2(addr));
		hist[ADDR2SETL2(addr)/64]++;
		printf("(%d)\n",hist[ADDR2SETL2(addr)/64]);
	}
	vl_free(lines);

TEST3:
	printf("----------test 3------------\n");
	lines = vl_new();
	int cnt = 0;
	while (cnt < 9) {
		element_t x = vl_pop(candidates);
		uintptr_t addr = getPhysicalAddr((uint64_t)&buffer[x]);
		if (ADDR2SETL2(addr) == 0) {
			vl_push(lines,x);
			cnt++;
		}
	}
	slowProbe(lines,profile_steps,0,0);
	for(int i = 0; i < measurments; i++) {
		uint64_t ct = slowProbe(lines,profile_steps,0,0);
		printf("%" STR(U64STR) ",",ct);
	}
}

void profilerl3_sudo(vlist_t candidates)
{
	int measurments = 10;
	int num_of_lines = 49; //193
	uint64_t profiles[num_of_lines][measurments];
	vlist_t lines;
	vlist_t candidatesl2 = vl_new();
	while(vl_len(candidatesl2) < 300) {
		element_t x = vl_pop(candidates);
		uintptr_t addr = getPhysicalAddr((uint64_t)&buffer[x]);
		int l2set = ADDR2SET(addr);
		if (l2set == 0) {
			vl_push(candidatesl2,x);
		}
	}
	printHistL2(candidatesl2);
	lines = vl_new();
	int profile_steps = l3_steps_global;
	for (int i = 0; i < num_of_lines; i++) {
		vl_push(lines,vl_pop(candidatesl2));
		int ct = slowProbe(lines,profile_steps,0,0);
		for (int j = 0; j < measurments; j ++)
			profiles[i][j] = slowProbe(lines,profile_steps,0,0);
		printf("vl_len(lines) = %d, took %d\n",vl_len(lines),ct);

	}
	printf("[");
	for (int i = 0; i < num_of_lines; i++) {
		//printf("%d lines:\n",i);
		for (int j = 0; j < measurments; j ++) {
			if(j == 0)printf("[");
			printf("%" STR(U64STR) "",profiles[i][j]);
			if(j != measurments-1)printf(",");
			if(j == measurments-1)printf("]");
			if(j == measurments-1 && i!=(num_of_lines-1))printf(",");
		}
		printf("\n");
	}
	printf("]\n\n");

	int hist[8] = {0};
	for (int i = 0; i < num_of_lines; i++) {
		uintptr_t addr = getPhysicalAddr((uint64_t)&buffer[vl_get(lines,i)]);
		printf("%d) %p %d ",i+1,(void*)addr,ADDR2SET(addr));
		hist[ADDR2SET(addr)/(64 * 8) ]++;
		printf("(%d)\n",hist[ADDR2SETL2(addr)/(64 * 8)]);
	}
	vl_free(lines);
}


void collectL2(vlist_t list,vlist_t candidates,int limit)
{
	printf("collectL2(%d,%d,%d)\n",vl_len(list),vl_len(candidates),limit);
	assert(list != NULL);
	assert(candidates != NULL);
	assert(vl_len(list) >= L2WAYS);
	vlist_t tmp0 = vl_new();
	vlist_t conflict = vl_new();
	vlist_t noconflict = vl_new();
	element_t x;
	uint64_t ct0;
	int chainSteps = l2_steps_collect_global;
	int thresh = l2_thresh_collect_global;
	bool collect;
	for(int i = 0; i < L2WAYS; i++) {
		vl_push(tmp0,vl_get(list,i));
	}

	int count = 0;

	while(vl_len(candidates) && (limit == 0 || (vl_len(conflict)+vl_len(list) < limit))) {
		while(vl_len(candidates) && (limit == 0 || (vl_len(conflict)+vl_len(list) < limit))) {
			//printf("vl_len(candidates) = %d\n",vl_len(candidates));
			x = vl_poprand(candidates);
			count++;
			uintptr_t addr = getPhysicalAddr((uint64_t)&buffer[x]);
			int l2set = ADDR2SETL2(addr);
			vl_push(tmp0,x);
			assert(vl_len(tmp0) == L2WAYS+1);
			ct0 = slowProbe(tmp0,chainSteps,0,0);
			//printf("ct = %" STR(U64STR) "\n",ct0);
			collect = ct0 >= thresh;
			if(collect) {
				vl_push(conflict,vl_pop(tmp0));
				if ((vl_len(conflict))%10 == 0) printf("%d\n",vl_len(conflict));
				// printf("V\n");
			} else {
				vl_push(noconflict,vl_pop(tmp0));
				//  printf("X\n");
			}
			//printHistL2(conflict);
		}
		if(vl_len(conflict)) printf("Collected %d / %d\n",vl_len(conflict),count);
		vl_pushall(list,conflict);
		printf("Before verify: vl_len(list) = %d\n",vl_len(list));
		verifyCollect(list,0,L2WAYS+1,l2_steps_global,NULL,0);
		//verifyCollect(list,0,L2WAYS+1,l2_steps_global,noconflict,l2_thresh_global);
		printf("After verify: vl_len(list) = %d\n",vl_len(list));
	}
	vl_pushall(candidates,noconflict);

	vl_free(conflict);
	vl_free(noconflict);
	while(vl_len(tmp0)) vl_pop(tmp0);
	vl_free(tmp0);
}

void EMSCRIPTEN_KEEPALIVE collectL2frompool (int setidx,int limit)
{
	if (setidx >= nsetsl2) {
		printf("No such L2 set\n");
		return;
	}
	collectL2(setsl2[setidx],pool,limit);
}

void collectL3(vlist_t list,vlist_t candidates,int limit)
{
	printf("collectL3(%d,%d,%d)\n",vl_len(list),vl_len(candidates),limit);
	assert(list != NULL);
	assert(candidates != NULL);
	assert(vl_len(list) >= L2WAYS);

	vlist_t tmp0 = vl_new();
	vlist_t tmp1 = vl_new();
	vlist_t conflict = vl_new();
	vlist_t noconflict = vl_new();
	element_t x;
	int ct0,ct1;
	int chainSteps = l3_steps_global;
	for(int i = 0; i < L3WAYS; i++) {
		vl_push(tmp0,vl_get(list,i));
		vl_push(tmp1,vl_get(list,i));
	}
	vl_poprand(tmp1);

	while(vl_len(candidates) && (limit == 0 || vl_len(conflict) < limit)) {
		//printf("vl_len(candidates) = %d\n",vl_len(candidates));
		x = vl_poprand(candidates);
		uintptr_t addr = getPhysicalAddr((uint64_t)&buffer[x]);
		int l2set = ADDR2SETL2(addr);
		//printf("x = %p\t%d " ,(void*)addr,l2set);
		vl_push(tmp0,x);
		vl_push(tmp1,x);
		assert(vl_len(tmp0) == L3WAYS+1);
		assert(vl_len(tmp1) == L3WAYS);
		ct0 = slowProbe(tmp0,chainSteps,0,0);
		ct1 = slowProbe(tmp1,chainSteps,0,0);
		//printf("ct0 = %d, ct1 = %d\n",ct0,ct1);

		if(ct0 >= l3_collect_highthresh_global && ct1 <= l3_collect_lowthresh_global) { //conflict!
			vl_push(conflict,vl_pop(tmp0));
			vl_pop(tmp1);
			//printf("ct0 = %d, ct1 = %d\n",ct0,ct1);
		} else { //no conflict / unknown
			vl_push(noconflict,vl_pop(tmp0));
			vl_pop(tmp1);
		}
		//printHistL3RS(conflict);
	}
	if(vl_len(conflict)) printf("Collected %d\n",vl_len(conflict));
	verifyCollect(list,0,L3WAYS+1,chainSteps,NULL,0);
	//verifyCollect(list,0,L3WAYS+1,chainSteps,noconflict,l3_collect_highthresh_global);
	//printHistL3RS(conflict);
	vl_pushall(list,conflict);
	vl_pushall(candidates,noconflict);

	vl_free(conflict);
	vl_free(noconflict);
	while(vl_len(tmp0)) vl_pop(tmp0);
	vl_free(tmp0);
	while(vl_len(tmp1)) vl_pop(tmp1);
	vl_free(tmp1);

}

void profiler_montecarlo(vlist_t from, int nlines, int steps)
{
	int n = 50;
	uint64_t ct;
	uint64_t *samples = malloc(n * sizeof(uint64_t));
	vlist_t tmp = vl_new();
	for (int i = 0; i < n; i++) {
		for (int j = 0; j < nlines; j++) vl_push(tmp,vl_poprand(from));
		ct = slowProbe(tmp,steps,0,0);
		printf("%d) %" STR(U64STR) "\n",i,ct);
		samples[i] = ct;
		vl_pushall(from,tmp);
	}
	for (int i = 0; i < n; i++) printf("%" STR(U64STR) ", ",samples[i]);
	printf("\n");
	vl_free(tmp);
	free(samples);
}

void EMSCRIPTEN_KEEPALIVE profilerl2()
{
	int nlines = L2WAYS*NSETSL2+1; // 65
	int measurments = 10;
	if(vl_len(pool) < nlines) return;
	uint64_t profiles[nlines][measurments];
	vlist_t lines = vl_new();
	int ct;
	for (int i = 0; i < nlines; i++) {
		vl_push(lines,vl_poprand(pool));
		printf("vl_len(lines) = %d",vl_len(lines) );
		ct = slowProbe(lines,l2_steps_global,0,0);
		printf("\t %d\n",ct);
		for (int j = 0; j < measurments; j ++)
			profiles[i][j] = slowProbe(lines,l2_steps_global,0,0);
	}
	printf("[");
	for (int i = 0; i < nlines; i++) {
		//printf("%d lines:\n",i);
		for (int j = 0; j < measurments; j ++) {
			if(j == 0)printf("[");
			printf("%" STR(U64STR) "",profiles[i][j]);
			if(j != measurments-1)printf(",");
			if(j == measurments-1)printf("]");
			if(j == measurments-1 && i!=(nlines-1))printf(",");
		}
		printf("\n");
	}
	printf("]\n\n");
	vl_pushall(pool,lines);
	vl_free(lines);
}

void EMSCRIPTEN_KEEPALIVE profilerl3()
{
	if (nsetsl2==0) return;
	int measurments = 3;
	int nlines = L3WAYS*NSETSL3_LOC+1;
	uint64_t profiles[nlines][measurments];
	vlist_t lines = vl_new();
	vlist_t candidatesl2 = setsl2[0];
	if (vl_len(candidatesl2) < nlines)
		collectL2(candidatesl2,pool,nlines);

	int ct;
	for (int i = 0; i < nlines; i++) {
		vl_push(lines,vl_poprand(candidatesl2));
		if (i > 12) {
			printf("vl_len(lines) = %d",vl_len(lines) );
			ct = slowProbe(lines,l3_steps_global,0,0);
			printf("\t %d\n",ct);
			for (int j = 0; j < measurments; j ++)
				profiles[i][j] = slowProbe(lines,l3_steps_global,0,0);
		} else {
			for (int j = 0; j < measurments; j ++)
				profiles[i][j] = 0;
		}
	}
	printf("[");
	for (int i = 0; i < nlines; i++) {
		//printf("%d lines:\n",i);
		for (int j = 0; j < measurments; j ++) {
			if(j == 0)printf("[");
			printf("%" STR(U64STR) "",profiles[i][j]);
			if(j != measurments-1)printf(",");
			if(j == measurments-1)printf("]");
			if(j == measurments-1 && i!=(nlines-1))printf(",");
		}
		printf("\n");
	}
	printf("]\n\n");
	vl_pushall(candidatesl2,lines);
	vl_free(lines);
}


void EMSCRIPTEN_KEEPALIVE free_setsl2()
{
	while(nsetsl2) {
		vl_pushall(pool,setsl2[nsetsl2-1]);
		vl_free(setsl2[--nsetsl2]);
	}
	vl_shuffle(pool);
}

void EMSCRIPTEN_KEEPALIVE free_sets()
{
	free_setsl2();
	while(nsets) {
		vl_pushall(pool,sets[nsets-1]);
		vl_free(sets[--nsets]);
	}
	vl_shuffle(pool);
}

#ifdef NATIVE
void mapl2_native()
{
	printf("mapl2_native()\n");
	free_setsl2();
	int nlines = 400; //-1 = no limit
	if(vl_len(pool) == 0) return;
	for (int i = 0; i < NSETSL2; i++) {
		setsl2[nsetsl2] = vl_new();
		nsetsl2++;
		for (int j = 0, len = vl_len(pool); j < len; j++) {
			element_t x = vl_pop(pool);
			uintptr_t addr = getPhysicalAddr((uint64_t)&buffer[x]);
			int l2set = (addr >> 12) & 0x7;
			if (l2set == i)
				vl_push(setsl2[i],x);
			else
				vl_pushfirst(pool,x);
			if(nlines > 0 && vl_len(setsl2[i]) == nlines) break;
		}
	}
	for (int i = 0; i < 8; i ++) printf("%d ",vl_len(setsl2[i]));
	printf("\n");
}
#endif

void EMSCRIPTEN_KEEPALIVE noiseline(element_t x,uint64_t time)
{
	printf("noiseline(%d,%" STR(U64STR) ")\n",x,time);
	int tmp;
	uint64_t start = clocksamplems();
	uint64_t now;
	int flag = true;
	while(flag) {
		for (int i = 0; i < 1000; i++) tmp = *(int*)&buffer[x];
		if(time > 0) {
			now = clocksamplems();
			if (now-start >= time)
				flag = false;
		}
	}
	flag = false;
}

void noise(vlist_t list,uint64_t time)
{
	printf("noise(%d,%" STR(U64STR) ")\n",vl_len(list),time);
	int tmp;
	int len = vl_len(list);
	uint64_t start = clocksamplems();
	uint64_t now;
	int flag = true;
	while(flag) {
		for (int i = 0; i < len; i++) tmp = *(int*)&buffer[vl_get(list,i)];
		if(time > 0) {
			now = clocksamplems();
			if (now-start >= time)
				flag = false;
		}
	}
}


static inline void sendbit(uint32_t value,int setidx,int offset,int lines,uint64_t dur)
{
	vlist_t list = sets[setidx];
	element_t tmp;
	uint64_t start;
	uint64_t now;
	now = start = clocksamplems();
	if (value)
		while(now-start < dur) {
			for (int i = 0; i < lines; i++) tmp = *(int*)&buffer[OFFSETIDX(vl_get(list,i),offset)];
			now = clocksamplems();
		}
	else
		while(now-start < dur) {
			for (int i = 0; i < lines; i++) tmp = 0; //todo
			now = clocksamplems();
		}
}


void EMSCRIPTEN_KEEPALIVE sendbits(uint32_t * values, int len,int setidx,int offset,int lines,uint64_t dur)
{
	if (setidx >= nsets) {
		printf("No such L3 set\n");
		return;
	}
	if (vl_len(sets[setidx]) < lines) {
		printf("vl_len(sets[%d]) < %d\n",setidx,lines);
		return;
	}

	for (int i = 0; i < len; i++) {
		sendbit(values[i],setidx,offset,lines,dur);
	}

}

void EMSCRIPTEN_KEEPALIVE sendbits_new(uint32_t * values, int len,int setidx,int offset,int lines,uint64_t dur)
{
	if (setidx >= nsets) {
		printf("No such L3 set\n");
		return;
	}
	if (vl_len(sets[setidx]) < lines) {
		printf("vl_len(sets[%d]) < %d\n",setidx,lines);
		return;
	}
	vlist_t list = sets[setidx];
	element_t tmp;
	syncclockms();
	uint64_t start = clocksampleEdge();
	for (int i = 0; i < len; i++) {
		while(clocksample() - start < (i+1)*dur){
			if(values[i]){
				for (int i = 0; i < lines; i++) tmp = *(int*)&buffer[OFFSETIDX(vl_get(list,i),offset)];
			} else {
				for (int i = 0; i < lines; i++) tmp = *(int*)&buffer[OFFSETIDX(vl_get(list,i),(offset^(LINESIZE>>1)))];
			}
		}
	}

}

void mapl2tol3(vlist_t candidatesl2,int n, int cheatmode)
{
	int startagain_cnt = 0;
	int conflict_assurance = NSETSL3_LOC*L3WAYS+1; //16*12+1 = 193
	uint64_t thresh = l3_thresh_global; //empirical
	uint64_t super_thresh = thresh;
	int chainSteps = l3_steps_global; //empirical
	int nlines = NSETSL3_LOC*L3WAYS*3; //16*12*2 = 384
	uint64_t ct;
	int nsetsl3_local = 0;
	printf("\nmapl2tol3(%d)\n",vl_len(candidatesl2));
	fflush(stdout);
	if (!n) n = NSETSL3_LOC;
	if(!cheatmode) {
		while (vl_len(candidatesl2) < nlines)
			collectL2(candidatesl2,pool,nlines);
		assert(vl_len(candidatesl2) >= nlines);
	}
	ct = slowProbe(candidatesl2,chainSteps,0,0);
	printf("Sanity: %d lines, ct = %" STR(U64STR) "\n",vl_len(candidatesl2),ct);
	vlist_t lines;
	bool flag = true;
	while(flag && vl_len(candidatesl2) && nsetsl3_local < n ) {
		uint64_t ctlog[1000] = {0};
		int ctlogidx = 0;
		uint64_t local_thresh = thresh;
		vl_shuffle(candidatesl2);
		vlist_t lines = vl_new();
		printHistL2(candidatesl2);
		printHistL3RS(candidatesl2);
		for(int i = 0, len = vl_len(candidatesl2); i < min(conflict_assurance, len); i++) {
			assert(vl_len(candidatesl2) > 0);
			vl_push(lines,vl_pop(candidatesl2));
		}
		printf("Extracting L3 eviction set from %d lines\n",vl_len(lines));
		ct = slowProbe(lines,chainSteps,0,0);
		ctlog[ctlogidx++] = ct;
		printf("Sanity: %d lines, ct = %" STR(U64STR) "\n",vl_len(lines),ct);
		if (ct < local_thresh) {
			printf("Sanity failed\n");
			break;
		}
		printHistL2(lines);
		printHistL3RS(lines);
		int tries = 0;
		bool dumpLine;
		bool fast = false;
		element_t x;
		while(vl_len(lines) > L3WAYS+1) {
			int fastcount = 0;
			while(fast) {
				fastcount++;
				int len = vl_len(lines);
				int fastlines = min(max(vl_len(lines)*.6,L3WAYS+1) + fastcount,vl_len(lines));
				printf("fastcount = %d, fastlines = %d, vl_len(lines) = %d\n",fastcount,fastlines,vl_len(lines));
				while(vl_len(lines)>fastlines) vl_push(candidatesl2,vl_poprand(lines));
				ct = slowProbe(lines,chainSteps,0,0);
				printf("# ct = %" STR(U64STR) " (thresh = %" STR(U64STR) ", vl_len(lines) = %d)\n",ct,thresh,vl_len(lines));
				ctlog[ctlogidx++] = ct;
				if (ct >= local_thresh) {
					printf("%d/%d l3 fastcount = %d\n",vl_len(lines),len,fastcount);
					fast = false;
				} else {
					for(int i = len-fastlines; i; i--) vl_push(lines,vl_pop(candidatesl2));
				}
			}
			x = vl_pop(lines);
			//printvlist(lines);
			ct = slowProbe(lines,chainSteps,0,0);
			dumpLine = true;
			for(int i = 0; i < L3_SAMPLES && dumpLine; i++) {
				ct = slowProbe(lines,chainSteps,0,0);
				ctlog[ctlogidx++] = ct;
				dumpLine = ct >= local_thresh;
				printf("%d lines, ct = %" STR(U64STR) ", local_thresh = %" STR(U64STR) ", dump = %d, (%d)\n",vl_len(lines), ct,local_thresh,dumpLine,tries);
			}
			if (dumpLine) {
				vl_push(candidatesl2,x);
				tries = 0;
			} else {
				//printf("dumpline = false\n");
				//printHistL3RS(lines);
				//ct = slowProbe(lines,chainSteps,0,0);        printf("before ct = %" STR(U64STR) "\n",ct);
				vl_pushfirst(lines,x);
				tries++;
				//ct = slowProbe(lines,chainSteps,0,0);        printf("after ct = %" STR(U64STR) "\n",ct);
				if(tries > L3_TRIES) {
					tries = 0;
					local_thresh = local_thresh * 0.98;
					while (true) {
						vl_push(lines,vl_pop(candidatesl2));
						printf("ROLLBACK push! ");
						ct = slowProbe(lines,chainSteps,0,0);
						printf("after push ct = %" STR(U64STR) "\n",ct);
						ctlog[ctlogidx++] = ct;
						if(ct >= local_thresh || !vl_len(candidatesl2) || vl_len(lines) >= conflict_assurance) break;
					}
				}
			}

		}
		ct = slowProbe(lines,chainSteps,0,0);
		ctlog[ctlogidx++] = ct;
		printf("Validation of L3 eviction set (%d): ct = %" STR(U64STR) "\n",vl_len(lines), ct);
		printHistL3RS(lines);
		fflush(stdout);
		printf("[");
		for(int i = 0; i < ctlogidx && ctlog[i]; i++) {
			if(i) printf(",");
			printf("%" STR(U64STR) "",ctlog[i]);
		}
		printf("] (%d)\n",ctlogidx);

		if (ct <= super_thresh) {
			startagain_cnt++;
			printf("ROLLBACK startagain (ct = %" STR(U64STR) ", super_thresh = %" STR(U64STR) ")\n",ct,super_thresh);
			vl_pushall(candidatesl2,lines);
			vl_free(lines);
			if(startagain_cnt > 3)  flag = false;
			continue;
		}
		printHistL3RS(lines);
		fflush(stdout);
		collectL3(lines, candidatesl2,0);
		printHistL3RS(lines);
		fflush(stdout);
		sets[nsets] = vl_new();
		vl_pushall(sets[nsets],lines);
		nsets++;
		nsetsl3_local++;
		//flag = false; //todo remove
		printf("nsets = %d\n",nsets);
		vl_free(lines);
		printf("\n");
	}
}

void EMSCRIPTEN_KEEPALIVE mapl2tol3i(int setl2idx,int n)
{
	if (setl2idx >= nsetsl2) {
		printf("No such L2 set\n");
		return;
	}
	uint64_t start_time = clocksample();
	mapl2tol3(setsl2[setl2idx],n,0);
	uint64_t end_time = clocksample();
	printf("Total time = %" STR(U64STR) "\n",end_time - start_time);

}

void EMSCRIPTEN_KEEPALIVE mapl3()
{
	if(nsetsl2 == 0) return;
	uint64_t start_time = clocksampleEdge();
	mapl2tol3(setsl2[0],0,0);
	uint64_t end_time = clocksampleEdge();
	printf("Total time = %" STR(U64STR) "\n",end_time - start_time);
	for(int i = 0; i < nsets; i++) {
		printf("l3set %d: %d lines\n",i,vl_len(sets[i]));
		printHistL3RS(sets[i]);
	}
	return;
	//todo
	for (int i = 0; i < nsetsl2; i++) {
		mapl2tol3(setsl2[i],0,0);
	}
}

void EMSCRIPTEN_KEEPALIVE mapl2()
{
	printf("mapl2()\n");
	uint64_t start,end;
	start = clocksamplems();
	int conflict_assurance = NSETSL2*L2WAYS+1;
	uint64_t thresh = l2_thresh_global; //empirical
	int chainSteps = l2_steps_global; //empirical
	int nlines =  NSETSL2*L2WAYS*4;
	uint64_t ct;
	vlist_t candidates = vl_new();
	free_setsl2();
	printf("vl_len(pool) = %d\n",vl_len(pool));
	printHistL2(pool);
	for (int i = 0; i < nlines; i++) vl_push(candidates,vl_pop(pool));
	while(vl_len(candidates) && nsetsl2 < NSETSL2) {
		printf("vl_len(candidates) = %d, nsetsl2 = %d\n",vl_len(candidates),nsetsl2);
		printHistL2(candidates);
		vl_shuffle(candidates);
		vlist_t rest = vl_new();
		vlist_t lines = vl_new();
		for(int i = min(conflict_assurance, vl_len(candidates)); i; i--)
			vl_push(lines,vl_pop(candidates));

		printf("Extracting L2 eviction set from %d lines...\n",vl_len(lines));
		fflush(stdout);
		printHistL2(lines);

		bool dumpLine;
		bool fast = true;
		int tries = 0;
		element_t x;
		slowProbe(lines,chainSteps,0,0);
		while(vl_len(lines) > L2WAYS+1) {
			int fastcount = 0;
			while(fast) {
				int fastlines = min(max(vl_len(lines)*.6,L2WAYS+1) + fastcount,vl_len(lines));
				fastcount++;
				while(vl_len(lines) > fastlines) vl_push(rest,vl_poprand(lines));
				ct = slowProbe(lines,chainSteps,0,0);
				printf("#ct = %" STR(U64STR) " (thresh = %" STR(U64STR) ", vl_len(lines) = %d)\n",ct,thresh,vl_len(lines));
				if (ct >= thresh) {
					fast = false;
					printf("%d/%d l2 fastcount = %d\n",fastlines,vl_len(lines)+vl_len(rest),fastcount);
				} else
					vl_pushall(lines,rest);
			}

			x = vl_pop(lines);
			dumpLine = true;
			for (int i = 0; i < L2_SAMPLES && dumpLine; i++) {
				ct = slowProbe(lines,chainSteps,0,0); printf("#%d ct = %" STR(U64STR) " (thresh = %" STR(U64STR) ", vl_len(lines) = %d)\n",i,ct,thresh,vl_len(lines));
				if(ct < thresh)  dumpLine = false;
			}
			if(dumpLine) {
				vl_push(rest,x);
				tries = 0;
			} else {
				vl_pushfirst(lines,x);
				tries++;
				if(tries > vl_len(lines)) {
					tries = 0;
					while (true) {
						if(vl_len(rest)) vl_push(lines,vl_pop(rest));
						printf("ROLLBACK push! ");
						ct = slowProbe(lines,chainSteps,0,0);
						printf("after push ct = %" STR(U64STR) " (%d lines)\n",ct,vl_len(lines));
						if(ct >= thresh) break;
					}
				}
			}
		}

		ct = slowProbe(lines,chainSteps,0,0);
		printf("Validation of L2 eviction set: ct = %" STR(U64STR) "\n", ct);
		if (ct <= thresh) {
			printf("ROLLBACK: start again...\n");
			vl_pushall(candidates,lines);
			vl_free(lines);
			continue;
		}
		printHistL2(lines);
		vl_pushall(candidates,rest);
		vl_free(rest);
		collectL2(lines,candidates,0);
		printHistL2(lines);
		setsl2[nsetsl2] = vl_new();
		vl_pushall(setsl2[nsetsl2],lines);
		nsetsl2++;
		vl_free(lines);
		printf("vl_len(candidates) = %d\n",vl_len(candidates));
		printf("\n");
		//break; //todo
	}

	end = clocksamplems();
	printf("Total mapl2() = %" STR(U64STR) " [ms]\n",end-start);
	for(int i = 0; i < nsetsl2; i++) {
		printf("l2set %d: %d lines\n",i,vl_len(setsl2[i]));
		printHistL2(setsl2[i]);
	}
	vl_pushall(pool,candidates);
	vl_free(candidates);

}
void EMSCRIPTEN_KEEPALIVE printShort()
{
	for (int i = 0; i < nsetsl2; i++) {
		printf("L2 Set %d: %d lines\n",i,vl_len(setsl2[i]));
	}
	for (int i = 0; i < nsets; i++) {
		printf("L3 Set %d: %d lines\n",i,vl_len(sets[i]));
		for (int j = 0; j < vl_len(sets[i]); j++) {
			printf("%d,",vl_get(sets[i],j));
		}
		printf("\n");
	}
}

void EMSCRIPTEN_KEEPALIVE printAll(int toFile)
{
	if (!nsets && !nsetsl2) return;
	FILE * out;
	out = toFile ? fopen("sets.txt", "w+") : stdout;
	int stat[32] = {0};
	int cnt_doubles = 0;
	int cnt_wrongcollect = 0;
	fprintf(out,"--------------------L2---------------------");
	for (int i = 0; i < nsetsl2; i++) {
		fprintf(out,"L2 Set %d:\n",i);
		uint64_t addr;
		int addr16_6,addr11_6,addr16_12;
		for (int j = 0; j < vl_len(setsl2[i]); j++) {
			addr = (uint64_t)&buffer[vl_get(setsl2[i],j)];
			fprintf(out,"%d)\t%d,\t&buffer[%d] = %p",j,vl_get(setsl2[i],j),vl_get(setsl2[i],j),(void*)addr);
			addr16_6 = ADDR2SET(getPhysicalAddr(addr));
			addr11_6 = addr16_6 & 0b111111;
			addr16_12 = addr16_6 >> 6;
			fprintf(out,"\t addr[16:6] = %d",addr16_6);
			if (addr11_6) fprintf(out,"\t addr[11:6] = %d",addr11_6);
			fprintf(out,"\t addr[16:12] = *%d*",addr16_12);
			fprintf(out,"\n");
		}
	}
	fprintf(out,"--------------------L3---------------------");
	for (int i = 0; i < nsets; i++) {
		fprintf(out,"Set %d:\n",i);
		int hist[2048] = {0};
		uint64_t addr;
		int addr16_6,addr11_6,addr16_12;
		for (int j = 0; j < vl_len(sets[i]); j++) {
			addr = (uint64_t)&buffer[vl_get(sets[i],j)];
			fprintf(out,"%d)\t%d,\t&buffer[%d] = %p",j,vl_get(sets[i],j),vl_get(sets[i],j),(void*)addr);
			addr16_6 = ADDR2SET(getPhysicalAddr(addr));
			addr11_6 = addr16_6 & 0b111111;
			addr16_12 = addr16_6 >> 6;
			fprintf(out,"\t addr[16:6] = %d",addr16_6);
			if (addr11_6) fprintf(out,"\t addr[11:6] = %d",addr11_6);
			fprintf(out,"\t addr[16:12] = *%d*",addr16_12);
			fprintf(out,"\n");
			hist[addr16_6]++;
		}
		if(vl_len(sets[i])) stat[addr16_12]++;
		if(stat[addr16_12] > 4) {
			fprintf(out,">>>>>>>>>>Double!\n");
			cnt_doubles++;
		}
		for (int cnt = 0, j = 0; j < 2048; j++) {
			if (hist[j] > 0) {
				fprintf(out,"total set %d: %d\n",j,hist[j]);
				cnt++;
				if(cnt > 1) {
					fprintf(out,">>>>>>>>>>WrongCollect!\n");
					cnt_wrongcollect++;
				}
			}
		}
	}
	for (int i = 0; i < 32; i++)
		fprintf(out,"stat[%d] = %d (set %d)\n", i,stat[i],i << 6);
	for (int i = 0; i < nsets; i++)
		fprintf(out,"set %d: %d lines\n",i,vl_len(sets[i]));
	fprintf(out,"Doubles = %d, WrongCollect = %d\n",cnt_doubles,cnt_wrongcollect);
	if(toFile)
		fclose(out);
}

void EMSCRIPTEN_KEEPALIVE swapsetidx(int setidx1,int setidx2)
{
	if (setidx1 >= nsets || setidx2 >= nsets) {
		printf("No such L3 set\n");
		return;
	}
	vlist_t tmp;
	tmp = sets[setidx1];
	sets[setidx1] = sets[setidx2];
	sets[setidx2] = tmp;
}


//cheating section
void EMSCRIPTEN_KEEPALIVE cheatmapL3(int duration,int steps,int threshold)
{
	free_sets();
	vlist_t candidates = vl_new();
	uint64_t s,e,ct;
	element_t el;
	for (int i = 0; i < L3WAYS*NSETS*2; i++) vl_push(candidates,vl_pop(pool));
	slowProbe(candidates,l3_steps_global,0,0);
	for (int i = 0; i < NSETS; i++) {
		s = clocksample();
		vlist_t newset = vl_new();
		uint64_t hist[L3WAYS*NSETS*2] = {0};
		for (int j = vl_len(candidates)-1; j>=0; j--) {
			el = vl_pop(candidates);
			if(j%100 == 0) printf("%d) check line %d\n",j,el);
			ct = slowProbeLine(el,steps);
			//printf("ct = %" STR(U64STR) "\n",ct);

			if (ct > threshold) {
				vl_push(newset,el);
			} else {
				vl_pushfirst(candidates,el);
			}
			hist[j] = ct;
		}
		for (int j = L3WAYS*NSETS*2-1; j>=0; j--) {
			printf("%" STR(U64STR) ",",hist[j]);
		}
		printf("\n");
		return;
		printf("New set: %d lines\n",vl_len(newset));
		sets[nsets++] = newset;
		e = clocksample();
		printf("e-s = %" STR(U64STR) "\n",(e-s));
	}
	vl_free(candidates);

}


void EMSCRIPTEN_KEEPALIVE receive_up(int * setsidx, int * offsets, int len, int nsamples, int steps, int lines)
{
	#ifdef THREADS
	printf("receive_withthread(%d sets,%d nsamples,%d steps,%d lines)\n",len,nsamples,steps,lines);
	receiveArgs * args = malloc(sizeof(receiveArgs));
	args->setsidx = setsidx;
	args->offsets = offsets;
	args->len = len;
	args->nsamples = nsamples;
	args->steps = steps;
	args->lines = lines;
	if (pthread_create(&bg_receive_thread, NULL, bg_receive_func, (void*)args)) {
		perror("Thread create failed");
		return;
	}
	#endif
}

uint32_t * EMSCRIPTEN_KEEPALIVE receive_down()
{
	uint32_t * res = NULL;
	#ifdef THREADS
	if (pthread_join(bg_receive_thread, (void**)&res)) {
		perror("Thread join failed");
		return NULL;
	}
	#endif
	return res;
}

uint32_t * EMSCRIPTEN_KEEPALIVE receive_new(int setidx, int offset, int dur, int nsamples, int lines)
{
	printf("receive_new(setidx %d,offset %d, duration %d, %d nsamples,%d lines)\n",setidx,offset,dur,nsamples,lines);
	if (setidx >= nsets) {
		printf("No such L3 set\n");
		return NULL;
	}
	uint32_t * res = malloc(nsamples * sizeof(uint32_t));
	element_t start;
	int n = (lines==0) ? vl_len(sets[setidx]) : min(lines,vl_len(sets[setidx]));
	start = sethead(sets[setidx],offset,n);
	syncclockms();
	uint64_t st = clocksampleEdge();
	int samplecounter = 0;
	uint64_t accesscounter = 0;
	while(clocksample()-st < nsamples * dur){
		accesscounter = 0;
		while(clocksample() - st < (samplecounter+1)*dur){
			walkSteps(start,lines);
			accesscounter++;
		}
		res[samplecounter++] = accesscounter;
	}
	return res;
}

uint32_t * EMSCRIPTEN_KEEPALIVE receive(int * setsidx, int * offsets, int len, int nsamples, int steps, int lines)
{
	printf("receive(%d sets,%d nsamples,%d steps,%d lines)\n",len,nsamples,steps,lines);
	//printf("offsets[0] = %d\n",offsets[0]);
	for (int i = 0; i < len; i++) {
		if (setsidx[i] >= nsets) {
			printf("No such L3 set\n");
			return NULL;
		}
	}
	for(int i = 0; i < len; i++)
		printf("set %d (%d,%d)\n",i,setsidx[i],offsets[i]);
	uint32_t * res = malloc(nsamples * len * sizeof(uint32_t) * 2);
	uint32_t * samples = res;
	uint32_t * timestamps = &res[nsamples * len];
	element_t * starts = malloc(len * sizeof(element_t));
	for (int i = 0; i < len; i++) {
		int n = (lines==0) ? vl_len(sets[setsidx[i]]) : min(lines,vl_len(sets[setsidx[i]]));
		starts[i] = sethead(sets[setsidx[i]],offsets[i],n);
	}
	uint64_t s,e;
	for (int i = 0; i < nsamples; i++) {
		for (int j = 0; j < len; j+=1) {
			s = clocksampleEdge();
			walkSteps(starts[j], steps);
			e = clocksample();
			samples[i*len+j] = (uint32_t)(e-s);
			timestamps[i*len+j] = (uint32_t)(e);
		}
	}
	free(starts);
	return res;
}



void EMSCRIPTEN_KEEPALIVE printParameters()
{
	printf("buffer = %p, l2sets = %d, l3sets = %d, pool = %d\n",buffer,nsetsl2,nsets, vl_len(pool));
	printf("# l3_thresh_global:\t%" STR(U64STR) "\n",l3_thresh_global);
	printf("# l3_collect_lowthresh_global:\t%" STR(U64STR) "\n",l3_collect_lowthresh_global);
	printf("# l3_collect_highthresh_global:\t%" STR(U64STR) "\n",l3_collect_highthresh_global);
	printf("# l3_steps_global:\t%d\n",l3_steps_global);
	printf("# l2_thresh_global:\t%" STR(U64STR) "\n",l2_thresh_global);
	printf("# l2_steps_global:\t%d\n",l2_steps_global);
	printf("# l2_thresh_collect_global:\t%" STR(U64STR) "\n",l2_thresh_collect_global);
	printf("# l2_steps_collect_global:\t%d\n",l2_steps_collect_global);
	printf("# clocksrc:\t%d\n",clocksrc);
}

void EMSCRIPTEN_KEEPALIVE setParameters(int l3_thresh,
                                        int l3_collect_lowthresh,
                                        int l3_collect_highthresh,
                                        int l3_steps,
                                        int l2_thresh,
                                        int l2_steps,
                                        int l2_thresh_collect,
                                        int l2_steps_collect
                                       )
{
	printf("0) l3_thresh,\n\
			1) l3_collect_lowthresh,\n\
			2) l3_collect_highthresh,\n\
			3) l3_steps,\n\
			4) l2_thresh,\n\
			5) l2_steps,\n\
			6) l2_thresh_collect,\n\
			7) l2_steps_collect,\n");
	if (l3_thresh) 				l3_thresh_global 				= (uint64_t)l3_thresh;
	if (l3_collect_lowthresh) 	l3_collect_lowthresh_global 	= (uint64_t)l3_collect_lowthresh;
	if (l3_collect_highthresh)	l3_collect_highthresh_global	= (uint64_t)l3_collect_highthresh;
	if (l3_steps) 				l3_steps_global					= l3_steps;
	if (l2_thresh) 				l2_thresh_global				= (uint64_t)l2_thresh;
	if (l2_steps)				l2_steps_global					= l2_steps;
	if (l2_thresh_collect)		l2_thresh_collect_global		= (uint64_t)l2_thresh_collect;
	if (l2_steps_collect)		l2_steps_collect_global			= l2_steps_collect;
}

void EMSCRIPTEN_KEEPALIVE line2addr(int line)
{
	printf("%p\n",&buffer[line]);
}

vlist_t cheat;
void EMSCRIPTEN_KEEPALIVE cheat_getset(int thresh, int offset)
{
	offset = offset % LINESIZE;
	vlist_t es = vl_new();
	element_t el;
	char junk = 0;
	cheat = vl_new();
	for (int i = 0; i < L3WAYS*NSETS*2; i++) {
		el = vl_pop(pool);
		int outer_cnt = 0;
		for (int j = 0; j < 10; j++) {
			int inner_cnt = 0;
			for (int k = 0; k < 1000; k++) {
				uint64_t s = clocksample();
				junk ^= buffer[OFFSETIDX(el,offset)];
				uint64_t e = clocksample();
				if (e-s > thresh) {
					inner_cnt++;
				}
			}
			if (inner_cnt > 0) {
				outer_cnt++;
			}
		}
		if (outer_cnt == 10) {
			vl_push(cheat,el);
		} else {
			vl_pushfirst(pool,el);
		}
	}
	printvlist(cheat);
}

/*
 * 	uint32_t * res = malloc(nsamples * sizeof(uint32_t));
	element_t start;
	int n = (lines==0) ? vl_len(sets[setidx]) : min(lines,vl_len(sets[setidx]));
	start = sethead(sets[setidx],offset,n);
	syncclockms();
	uint64_t st = clocksampleEdge();
	int samplecounter = 0;
	uint64_t accesscounter = 0;
	while(clocksample()-st < nsamples * dur){
		accesscounter = 0;
		while(clocksample() - st < (samplecounter+1)*dur){
			walkSteps(start,lines);
			accesscounter++;
		}
		res[samplecounter++] = accesscounter;
	}
	return res;
 */
 
 
void EMSCRIPTEN_KEEPALIVE cheat_checkline(element_t el){
	uint64_t st,en,accesscounter;	
	st = en = clocksample();
		printf("el = %d ",el);
		buffer[el] = el;
		while(en - st < 100){
			for (int i = 0; i < 100; i++){
				el = buffer[el];
			}
			accesscounter++;
			en = clocksample();
		}
		printf("\t %" STR(U64STR) "\n",accesscounter);

}


void EMSCRIPTEN_KEEPALIVE cheat_getset_steps(int thresh, int offset)
{
	offset = offset % LINESIZE;
	vlist_t es = vl_new();
	element_t el;
	char junk = 0;
	cheat = vl_new();
	uint64_t accesscounter = 0;
	uint64_t st,en;
	for (int i = 0; i < L3WAYS*NSETS*2; i++) {
		el = vl_pop(pool);
		accesscounter = 0;
		st = en = clocksample();
		printf("i = %d, el = %d ",i,el);
		while(en - st < 100){
			for (int i = 0; i < 100; i++){
				buffer[OFFSETIDX(el,offset)]*=2;
				buffer[OFFSETIDX(el,offset)]/=2;
			}
			accesscounter++;
			en = clocksample();
		}
		printf("\t %" STR(U64STR) "\n",accesscounter);
		if (0) {
			vl_push(cheat,el);
		} else {
			vl_pushfirst(pool,el);
		}
	}
	printvlist(cheat);
}


void EMSCRIPTEN_KEEPALIVE cheat_extract()
{
	int clocksrc_original = clocksrc;
	setClockSrc(0);
	mapl2tol3(cheat,1,1);
	setClockSrc(clocksrc_original);
}

void EMSCRIPTEN_KEEPALIVE cheat_newset()
{
	sets[nsets++] = cheat;
}

void EMSCRIPTEN_KEEPALIVE cheat_print()
{
	printvlist(cheat);
}

void EMSCRIPTEN_KEEPALIVE cheat_free()
{
	vl_pushall(pool,cheat);
	vl_free(cheat);
}

void EMSCRIPTEN_KEEPALIVE cheat_fakeset(){
	cheat = vl_new();
	for (int i = 0; i < L3WAYS*2; i++){
		vl_push(cheat,i*4096);
	}
	cheat_newset();
}
 

void EMSCRIPTEN_KEEPALIVE clocktest()
{
	uint64_t e,s;
	while(1) {
		s = clocksamplems();
		e = clocksamplems();
		if (e-s) {
			printf("%" STR(U64STR) "\n",e-s);
			break;
		}
	}
}
int main(int argc,  const char ** argv)
{
	buffer = (char*)malloc((POOLSIZE+1)*PAGESIZE);
	buffer = (char*)OFFSET(buffer,0) + 0x1000;

	assert(buffer!=NULL);
	for (int i = 0; i < POOLSIZE*PAGESIZE; i++)	buffer[i] = 0;
	pool = vl_new();
	for (int i = 0; i < POOLSIZE; i++) vl_push(pool,i*PAGESIZE);
	
#ifndef NATIVE
	printf("Ready\n");
#endif
	//...

}
