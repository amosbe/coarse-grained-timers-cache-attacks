#ifndef __VLIST_H__
#define __VLIST_H__

typedef int element_t;
typedef struct vlist *vlist_t;

vlist_t vl_new();
void vl_free(vlist_t vl);

inline element_t vl_get(vlist_t vl, int ind);
void vl_set(vlist_t vl, int ind, element_t dat);
int vl_push(vlist_t vl, element_t dat);
int vl_pushfirst(vlist_t vl, element_t dat);
element_t vl_pop(vlist_t vl);
element_t vl_poprand(vlist_t vl);
element_t vl_del(vlist_t vl, int ind);
inline int vl_len(vlist_t vl);
void vl_insert(vlist_t vl, int ind, element_t dat);
int vl_find(vlist_t vl, element_t dat);
void vl_shuffle(vlist_t vl);
int vl_countDoubles(vlist_t vl);
void vl_pushall(vlist_t to, vlist_t from);
vlist_t vl_copy(vlist_t vl);

//---------------------------------------------
// Implementation details
//---------------------------------------------

struct vlist {
  int size;
  int len;
  element_t *data;
};

inline element_t vl_get(vlist_t vl, int ind) {
  assert(vl != NULL);
  assert(ind < vl->len);
  return vl->data[ind];
}

inline int vl_len(vlist_t vl) {
  assert(vl != NULL);
  return vl->len;
}



#endif // __VLIST_H__
