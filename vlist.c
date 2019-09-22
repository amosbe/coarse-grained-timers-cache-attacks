#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <strings.h>

#include "vlist.h"

extern element_t vl_get(vlist_t vl, int ind);
extern int vl_len(vlist_t vl);

#define VLIST_DEF_SIZE 16

void swap(element_t * a,element_t * b){
	element_t t;
	t = *a;
	*a = *b;
	*b = t;
}
vlist_t vl_new() {
  vlist_t vl = (vlist_t)malloc(sizeof(struct vlist));
  vl->size = VLIST_DEF_SIZE;
  vl->data = (element_t *)calloc(VLIST_DEF_SIZE, sizeof(element_t ));
  vl->len = 0;
  return vl;
}
vlist_t vl_copy(vlist_t vl) {
  assert(vl != NULL);
  vlist_t copy = (vlist_t)malloc(sizeof(struct vlist));
  copy->size = vl->size;
  copy->data = (element_t *)calloc(copy->size, sizeof(element_t));
  copy->len = vl->len;
  for (int i = 0; i < copy->len; i++)
	  copy->data[i] = vl->data[i];
  return copy;
}
void vl_free(vlist_t vl) {
  assert(vl != NULL);
  if(vl_len(vl) > 0)
	  printf("vl_free(): Warning - vl_len(vl) = %d\n",vl_len(vl));
  free(vl->data);
  bzero(vl, sizeof(struct vlist));
  free(vl);
}


void vl_set(vlist_t vl, int ind, element_t dat) {
  assert(vl != NULL);
  assert(dat != -1);
  assert(ind < vl->len);
  vl->data[ind] = dat;
}

static void vl_setsize(vlist_t vl, int size) {
  assert(vl != NULL);
  assert(size >= vl->len);
  element_t *old = vl->data;
  vl->data = (element_t *)realloc(old, size * sizeof(element_t ));
  vl->size = size;
}


int vl_push(vlist_t vl, element_t dat) {
  assert(vl != NULL);
  assert(dat != -1);
  if (vl->len == vl->size)
    vl_setsize(vl, vl->size * 2);
  assert(vl->len < vl->size);
  vl->data[vl->len++] = dat;
  return vl->len - 1;
}

int vl_pushfirst(vlist_t vl, element_t dat) {
  vl_push(vl,dat);
  element_t tmp;
  for(int i=0; i < vl_len(vl); i++){
	tmp = vl->data[i];
	vl->data[i] = vl->data[vl_len(vl)-1];
	vl->data[vl_len(vl)-1] = tmp;
  }
  return 0;
}

element_t vl_pop(vlist_t vl) {
  assert(vl != NULL);
  element_t res;
  if (vl->len == 0)
    return -1;
  res = vl->data[vl->len-1];
  vl->data[vl->len-1] = -1;
  vl->len--;
  return res;
}

element_t vl_del(vlist_t vl, int ind) {
  assert(vl != NULL);
  assert(ind < vl->len);
  element_t rv = vl->data[ind];
  vl->data[ind] = vl->data[--vl->len];
  return rv;
}

element_t vl_poprand(vlist_t vl) {
  assert(vl != NULL);
  if (vl->len == 0)
    return -1;
  int ind = random() % vl->len;
  element_t rv = vl->data[ind];
  vl_del(vl, ind);
  return rv;
}

void vl_print(vlist_t vl){
	assert(vl != NULL);
	for (int i = 0; i < vl->len; i++){
		printf("%d) %d\n",i,vl_get(vl,i)/4096);
	}
}

void vl_shuffle(vlist_t vl) {
  assert(vl != NULL);
  int len = vl->len;
  if (len < 2) return;
  for (int i = 0; i < len; i++) {
	  int j = i + random() % (len - i);
      element_t tmp = vl->data[j];
      vl->data[j] = vl->data[i];
      vl->data[i] = tmp;
  }
}

int vl_countDoubles(vlist_t vl) {
  assert(vl != NULL);
  int ndoubles = 0;
  int len = vl->len;
  for(int i = 0; i < len; i++){
  	for(int j = i+1; j < len; j++){
		ndoubles += (vl->data[i] == vl->data[j]);
	}
  }
  return ndoubles;
}

void vl_insert(vlist_t vl, int ind, element_t dat) {
  assert(vl != NULL);
  assert(dat != -1);
  assert(ind <= vl->len);
  if (ind == vl->len) {
    vl_push(vl, dat);
  } else {
    vl_push(vl, vl->data[ind]);
    vl->data[ind] = dat;
  }
}

int vl_find(vlist_t vl, element_t dat) {
  assert(vl != NULL);
  assert(dat != -1);
  for (int i = 0; i < vl->len; i++)
    if (vl->data[i] == dat)
      return i;
  return -1;
}

void vl_pushall(vlist_t to, vlist_t from){
	assert(from != NULL);
	assert(to != NULL);
	while(vl_len(from)>0){
		vl_push(to,vl_pop(from));
	}
}