/////////////////////////////////////////////////////////////////////
// = NMatrix
//
// A linear algebra library for scientific computation in Ruby.
// NMatrix is part of SciRuby.
//
// NMatrix was originally inspired by and derived from NArray, by
// Masahiro Tanaka: http://narray.rubyforge.org
//
// == Copyright Information
//
// SciRuby is Copyright (c) 2010 - 2012, Ruby Science Foundation
// NMatrix is Copyright (c) 2012, Ruby Science Foundation
//
// Please see LICENSE.txt for additional copyright notices.
//
// == Contributing
//
// By contributing source code to SciRuby, you agree to be bound by
// our Contributor Agreement:
//
// * https://github.com/SciRuby/sciruby/wiki/Contributor-Agreement
//
// == list.c
//
// List-of-lists n-dimensional matrix storage. Uses singly-linked
// lists.

/*
 * Standard Includes
 */

#include <ruby.h>

/*
 * Project Includes
 */

#include "types.h"

#include "data/data.h"

#include "common.h"
#include "list.h"

#include "util/sl_list.h"

/*
 * Macros
 */

/*
 * Global Variables
 */

extern bool (*ElemEqEq[NUM_DTYPES][2])(const void*, const void*, const int, const int);

/*
 * Forward Declarations
 */

static bool list_storage_cast_copy_contents_dense(LIST* lhs, const char* rhs, void* zero, int8_t l_dtype, int8_t r_dtype, size_t* pos, size_t* coords, const size_t* shape, size_t rank, size_t recursions);

/*
 * Functions
 */

////////////////
// Lifecycle //
///////////////

/*
 * Creates a list-of-lists(-of-lists-of-lists-etc) storage framework for a
 * matrix.
 *
 * Note: The pointers you pass in for shape and init_val become property of our
 * new storage. You don't need to free them, and you shouldn't re-use them.
 */
LIST_STORAGE* list_storage_create(dtype_t dtype, size_t* shape, size_t rank, void* init_val) {
  LIST_STORAGE* s;

  s = ALLOC( LIST_STORAGE );

  s->rank  = rank;
  s->shape = shape;
  s->dtype = dtype;

  s->rows  = list_create();

  s->default_val = init_val;

  return s;
}

/*
 * Documentation goes here.
 */
void list_storage_delete(LIST_STORAGE* s) {
  if (s) {
    //fprintf(stderr, "* Deleting list storage rows at %p\n", s->rows);
    list_delete( s->rows, s->rank - 1 );

    //fprintf(stderr, "  Deleting list storage shape at %p\n", s->shape);
    free(s->shape);
    //fprintf(stderr, "  Deleting list storage default_val at %p\n", s->default_val);
    free(s->default_val);
    //fprintf(stderr, "  Deleting list storage at %p\n", s);
    free(s);
  }
}

/*
 * Documentation goes here.
 */
void list_storage_mark(LIST_STORAGE* storage) {

  if (storage && storage->dtype == RUBYOBJ) {
    rb_gc_mark(*((VALUE*)(storage->default_val)));
    list_mark(storage->rows, storage->rank - 1);
  }
}

///////////////
// Accessors //
///////////////


/*
 * Documentation goes here.
 */
void* list_storage_get(LIST_STORAGE* s, SLICE* slice) {
  rb_raise(rb_eNotImpError, "This type of slicing not supported yet");
}


/*
 * Get the contents of some set of coordinates. Note: Does not make a copy!
 * Don't free!
 */
void* list_storage_ref(LIST_STORAGE* s, SLICE* slice) {
  //LIST_STORAGE* s = (LIST_STORAGE*)(t);
  size_t r;
  NODE*  n;
  LIST*  l = s->rows;

  for (r = s->rank; r > 1; --r) {
    n = list_find(l, slice->coords[s->rank - r]);
    if (n)  l = n->val;
    else return s->default_val;
  }

  n = list_find(l, slice->coords[s->rank - r]);
  if (n) return n->val;
  else   return s->default_val;
}

/*
 * Documentation goes here.
 *
 * TODO: Allow this function to accept an entire row and not just one value -- for slicing
 */
void* list_storage_insert(LIST_STORAGE* s, SLICE* slice, void* val) {
  // Pretend ranks = 2
  // Then coords is going to be size 2
  // So we need to find out if some key already exists
  size_t r;
  NODE*  n;
  LIST*  l = s->rows;

  // drill down into the structure
  for (r = s->rank; r > 1; --r) {
    n = list_insert(l, false, slice->coords[s->rank - r], list_create());
    l = n->val;
  }

  n = list_insert(l, true, slice->coords[s->rank - r], val);
  return n->val;
}

/*
 * Documentation goes here.
 *
 * TODO: Speed up removal.
 */
void* list_storage_remove(LIST_STORAGE* s, SLICE* slice) {
  int r;
  NODE  *n = NULL;
  LIST*  l = s->rows;
  void*  rm = NULL;

  // keep track of where we are in the traversals
  NODE** stack = ALLOCA_N( NODE*, s->rank - 1 );

  for (r = (int)(s->rank); r > 1; --r) {
  	// does this row exist in the matrix?
    n = list_find(l, slice->coords[s->rank - r]);

    if (!n) {
    	// not found
      free(stack);
      return NULL;
      
    } else {
    	// found
      stack[s->rank - r]    = n;
      l                     = n->val;
    }
  }

  rm = list_remove(l, slice->coords[s->rank - r]);

  // if we removed something, we may now need to remove parent lists
  if (rm) {
    for (r = (int)(s->rank) - 2; r >= 0; --r) {
    	// walk back down the stack
      
      if (((LIST*)(stack[r]->val))->first == NULL) {
        free(list_remove(stack[r]->val, slice->coords[r]));
        
      } else {
      	// no need to continue unless we just deleted one.
        break;
      }
    }
  }

  return rm;
}

///////////
// Tests //
///////////

/*
 * Do these two dense matrices of the same dtype have exactly the same
 * contents?
 *
 * FIXME: Add templating.
 */
bool list_storage_eqeq(const LIST_STORAGE* left, const LIST_STORAGE* right) {

  // in certain cases, we need to keep track of the number of elements checked.
  size_t num_checked  = 0,
         max_elements = storage_count_max_elements(left->rank, left->shape);
  
  bool (*eqeq)(const void*, const void*, const int, const int) = ElemEqEq[left->dtype][0];

  if (!left->rows->first) {
    // fprintf(stderr, "!left->rows true\n");
    // Easy: both lists empty -- just compare default values
    if (!right->rows->first) {
    	return eqeq(left->default_val, right->default_val, 1, DTYPE_SIZES[left->dtype]);
    	
    } else if (!list_eqeq_value(right->rows, left->default_val, left->dtype, left->rank-1, &num_checked)) {
    	// Left empty, right not empty. Do all values in right == left->default_val?
    	return false;
    	
    } else if (num_checked < max_elements) {
    	// If the matrix isn't full, we also need to compare default values.
    	return eqeq(left->default_val, right->default_val, 1, DTYPE_SIZES[left->dtype]);
    }

  } else if (!right->rows->first) {
    // fprintf(stderr, "!right->rows true\n");
    // Right empty, left not empty. Do all values in left == right->default_val?
    if (!list_eqeq_value(left->rows, right->default_val, left->dtype, left->rank-1, &num_checked)) {
    	return false;
    	
    } else if (num_checked < max_elements) {
   		// If the matrix isn't full, we also need to compare default values.
    	return eqeq(left->default_val, right->default_val, 1, DTYPE_SIZES[left->dtype]);
    }

  } else {
    // fprintf(stderr, "both matrices have entries\n");
    // Hardest case. Compare lists node by node. Let's make it simpler by requiring that both have the same default value
    if (!list_eqeq_list(left->rows, right->rows, left->default_val, right->default_val, left->dtype, left->rank-1, &num_checked)) {
    	return false;
    	
    } else if (num_checked < max_elements) {
    	return eqeq(left->default_val, right->default_val, 1, DTYPE_SIZES[left->dtype]);
    }
  }

  return true;
}

/////////////
// Utility //
/////////////

/*
 * Documentation goes here.
 */
size_t list_storage_count_elements_r(const LIST* l, size_t recursions) {
  size_t count = 0;
  NODE* curr = l->first;
  
  if (recursions) {
    while (curr) {
      count += list_storage_count_elements_r(curr->val, recursions - 1);
      curr   = curr->next;
    }
    
  } else {
    while (curr) {
      ++count;
      curr = curr->next;
    }
  }
  
  return count;
}

/*
 * Count non-diagonal non-zero elements.
 */
size_t count_list_storage_nd_elements(const LIST_STORAGE* s) {
  NODE *i_curr, *j_curr;
  size_t count = 0;
  
  if (s->rank != 2) {
  	rb_raise(rb_eNotImpError, "non-diagonal element counting only defined for rank = 2");
  }

  for (i_curr = s->rows->first; i_curr; i_curr = i_curr->next) {
    for (j_curr = ((LIST*)(i_curr->val))->first; j_curr; j_curr = j_curr->next) {
      if (i_curr->key != j_curr->key) {
      	++count;
      }
    }
  }
  
  return count;
}

/////////////////////////
// Copying and Casting //
/////////////////////////

/*
 * Documentation goes here.
 */
LIST_STORAGE* list_storage_copy(LIST_STORAGE* rhs) {
  LIST_STORAGE* lhs;
  size_t* shape;
  void* default_val = ALLOC_N(char, DTYPE_SIZES[rhs->dtype]);

  //fprintf(stderr, "copy_list_storage\n");

  // allocate and copy shape
  shape = ALLOC_N(size_t, rhs->rank);
  memcpy(shape, rhs->shape, rhs->rank * sizeof(size_t));
  memcpy(default_val, rhs->default_val, DTYPE_SIZES[rhs->dtype]);

  lhs = list_storage_create(rhs->dtype, shape, rhs->rank, default_val);

  if (lhs) {
    lhs->rows = list_create();
    list_cast_copy_contents(lhs->rows, rhs->rows, rhs->dtype, rhs->dtype, rhs->rank - 1);
  } else {
  	free(shape);
  }

  return lhs;
}

/*
 * List storage copy constructor C access.
 */
LIST_STORAGE* list_storage_cast_copy(const LIST_STORAGE* rhs, dtype_t new_dtype) {
  LR_DTYPE_TEMPLATE_TABLE(list_storage_cast_copy_template, LIST_STORAGE*, const LIST_STORAGE*, dtype_t);

  return ttable[new_dtype][rhs->dtype](rhs, new_dtype);
}


/*
 * List storage copy constructor for changing dtypes.
 */
template <typename LDType, typename RDType>
LIST_STORAGE* list_storage_cast_copy_template(const LIST_STORAGE* rhs, dtype_t new_dtype) {

  NewDType* default_val = ALLOC_N(LDType, 1);

  // allocate and copy shape
  size_t* shape = ALLOC_N(size_t, rhs->rank);
  memcpy(shape, rhs->shape, rhs->rank * sizeof(size_t));

  // copy default value
  *default_val = static_cast<LDType>(*reinterpret_cast<RDType*>(rhs->default_val));

  LIST_STORAGE* lhs = list_storage_create(new_dtype, shape, rhs->rank, default_val);
  lhs->rows         = list_create();
  list_cast_copy_contents_template<LDType, RDType>(lhs->rows, rhs->rows, rhs->rank - 1);

  return lhs;
}


/* Copy dense into lists recursively
 *
 * FIXME: This works, but could probably be cleaner (do we really need to pass coords around?)
 */
template <typename LDType, typename RDType>
static bool list_storage_cast_copy_contents_dense_template(LIST* lhs, const RDType* rhs, RDType* zero, size_t& pos, size_t* coords, const size_t* shape, size_t rank, size_t recursions) {
  NODE *prev;
  LIST *sub_list;
  bool added = false, added_list = false;
  void* insert_value;

  for (coords[rank-1-recursions] = 0; coords[rank-1-recursions] < shape[rank-1-recursions]; ++coords[rank-1-recursions], ++pos) {
    //fprintf(stderr, "(%u)\t<%u, %u>: ", recursions, coords[0], coords[1]);

    if (recursions == 0) {
    	// create nodes

      if (rhs[pos] != zero) {
      	// is not zero
        //fprintf(stderr, "inserting value\n");

        // Create a copy of our value that we will insert in the list
        LDType* insert_value = ALLOC_N(LDType, 1);
        *insert_value        = static_cast<LDType>(rhs[pos]);

        if (!lhs->first)    prev = list_insert(lhs, false, coords[rank-1-recursions], insert_value);
        else               	prev = list_insert_after(prev, coords[rank-1-recursions], insert_value);
        
        added = true;
      }
      // no need to do anything if the element is zero
      
    } else { // create lists
      // create a list as if there's something in the row in question, and then delete it if nothing turns out to be there
      sub_list = list_create();

      added_list = list_storage_cast_copy_contents_dense_template<LDType,RDType>(sub_list, rhs, zero, pos, coords, shape, rank, recursions-1);

      if (!added_list)      	list_delete(sub_list, recursions-1);
      else if (!lhs->first)  	prev = list_insert(lhs, false, coords[rank-1-recursions], sub_list);
      else                  	prev = list_insert_after(prev, coords[rank-1-recursions], sub_list);

      // added = (added || added_list);
    }
  }

  coords[rank-1-recursions] = 0;
  --pos;

  return added;
}
