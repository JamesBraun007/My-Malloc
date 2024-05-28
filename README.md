# MyMalloc

## Author: 
### Roger Braun
### roger.t.braun@gmail.com


## Description:
MyMalloc is a custom implementation of the standard malloc() function found in the C standard library. It support dynamic memory allocation and deallocation, and is capable of splitting and coalescing memory in constant time due to the usage of a segregated free list data structure. 


## Build Instructions:
After downloading the files in this repository to your personal computer, the project can be built by compiling the source code file (myMalloc.c) using gcc as follows: gcc myMalloc.c -o myMalloc. Once built, the functions in myMalloc.c can be utilized to allocate and deallocate 
memory dynamically by the user. 


## Example Usage:
Note: The mallocing() and freeing() functions are customs functions defined in testing.c. These are formatted in such a way to make debugging easier. 
```
#include "testing.h"

int main() {
  initialize_test(__FILE__);

  void * ptr = mallocing(8, print_status, false);
  void * ptr2 = mallocing(8, print_status, false);
  freeing(ptr, 8, print_status, false);
  freeing(ptr2, 8, print_status, false);

  finalize_test();
}
```

