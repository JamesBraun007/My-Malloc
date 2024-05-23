#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "myMalloc.h"
#include "printing.h"

/* Due to the way assert() prints error messges we use out own assert function
 * for deteminism when testing assertions
 */
#ifdef TEST_ASSERT
  inline static void assert(int e) {
    if (!e) {
      const char * msg = "Assertion Failed!\n";
      write(2, msg, strlen(msg));
      exit(1);
    }
  }
#else
  #include <assert.h>
#endif

/*
 * Mutex to ensure thread safety for the freelist
 */
static pthread_mutex_t mutex;

/*
 * Array of sentinel nodes for the freelists
 */
header freelistSentinels[N_LISTS];

/*
 * Pointer to the second fencepost in the most recently allocated chunk from
 * the OS. Used for coalescing chunks
 */
header * lastFencePost;

/*
 * Pointer to maintian the base of the heap to allow printing based on the
 * distance from the base of the heap
 */ 
void * base;

/*
 * List of chunks allocated by  the OS for printing boundary tags
 */
header * osChunkList [MAX_OS_CHUNKS];
size_t numOsChunks = 0;

/*
 * direct the compiler to run the init function before running main
 * this allows initialization of required globals
 */
static void init (void) __attribute__ ((constructor));

// Helper functions for manipulating pointers to headers
static inline header * get_header_from_offset(void * ptr, ptrdiff_t off);
static inline header * get_left_header(header * h);
static inline header * ptr_to_header(void * p);

// Helper functions for allocating more memory from the OS
static inline void initialize_fencepost(header * fp, size_t left_size);
static inline void insert_os_chunk(header * hdr);
static inline void insert_fenceposts(void * raw_mem, size_t size);
static header * allocate_chunk(size_t size);

// Helper functions for freeing a block
static inline void deallocate_object(void * p);

// Helper functions for allocating a block
static inline header * allocate_object(size_t raw_size);

// Helper functions for verifying that the data structures are structurally 
// valid
static inline header * detect_cycles();
static inline header * verify_pointers();
static inline bool verify_freelist();
static inline header * verify_chunk(header * chunk);
static inline bool verify_tags();

static void init();

static bool isMallocInitialized;

// my own helper functions that I added
static inline header * findBlock(size_t blockSize);
static inline header * searchLast(size_t blockSize);
static inline header * splitBlock(header * block, size_t blockSize);
static inline void insertIntoFreeList(size_t remainder, header * remainderHead);
static inline void removeFromFreeList(header * neighbor);

/**
 * @brief Helper function to retrieve a header pointer from a pointer and an 
 *        offset
 *
 * @param ptr base pointer
 * @param off number of bytes from base pointer where header is located
 *
 * @return a pointer to a header offset bytes from pointer
 */
static inline header * get_header_from_offset(void * ptr, ptrdiff_t off) {
	return (header *)((char *) ptr + off);
}

/**
 * @brief Helper function to get the header to the right of a given header
 *
 * @param h original header
 *
 * @return header to the right of h
 */
header * get_right_header(header * h) {
	return get_header_from_offset(h, get_size(h));
}

/**
 * @brief Helper function to get the header to the left of a given header
 *
 * @param h original header
 *
 * @return header to the right of h
 */
inline static header * get_left_header(header * h) {
  return get_header_from_offset(h, -h->left_size);
}

/**
 * @brief Fenceposts are marked as always allocated and may need to have
 * a left object size to ensure coalescing happens properly
 *
 * @param fp a pointer to the header being used as a fencepost
 * @param left_size the size of the object to the left of the fencepost
 */
inline static void initialize_fencepost(header * fp, size_t left_size) {
	set_state(fp,FENCEPOST);
	set_size(fp, ALLOC_HEADER_SIZE);
	fp->left_size = left_size;
}

/**
 * @brief Helper function to maintain list of chunks from the OS for debugging
 *
 * @param hdr the first fencepost in the chunk allocated by the OS
 */
inline static void insert_os_chunk(header * hdr) {
  if (numOsChunks < MAX_OS_CHUNKS) {
    osChunkList[numOsChunks++] = hdr;
  }
}

/**
 * @brief given a chunk of memory insert fenceposts at the left and 
 * right boundaries of the block to prevent coalescing outside of the
 * block
 *
 * @param raw_mem a void pointer to the memory chunk to initialize
 * @param size the size of the allocated chunk
 */
inline static void insert_fenceposts(void * raw_mem, size_t size) {
  // Convert to char * before performing operations
  char * mem = (char *) raw_mem;

  // Insert a fencepost at the left edge of the block
  header * leftFencePost = (header *) mem;
  initialize_fencepost(leftFencePost, ALLOC_HEADER_SIZE);

  // Insert a fencepost at the right edge of the block
  header * rightFencePost = get_header_from_offset(mem, size - ALLOC_HEADER_SIZE);
  initialize_fencepost(rightFencePost, size - 2 * ALLOC_HEADER_SIZE);
}

/**
 * @brief Allocate another chunk from the OS and prepare to insert it
 * into the free list
 *
 * @param size The size to allocate from the OS
 *
 * @return A pointer to the allocable block in the chunk (just after the 
 * first fencpost)
 */
static header * allocate_chunk(size_t size) {
  void * mem = sbrk(size);
  
  insert_fenceposts(mem, size);
  header * hdr = (header *) ((char *)mem + ALLOC_HEADER_SIZE);
  set_state(hdr, UNALLOCATED);
  set_size(hdr, size - 2 * ALLOC_HEADER_SIZE);
  hdr->left_size = ALLOC_HEADER_SIZE;
  return hdr;
}

/**
 * @brief Helper allocate an object given a raw request size from the user
 *
 * @param raw_size number of bytes the user needs
 *
 * @return A block satisfying the user's request
 */
static inline header * allocate_object(size_t raw_size) {

  if (raw_size == 0) {
    return NULL;
  }
  
  // calculating blockSize and rounding up to nearest modulo(8) = 0
  size_t blockSize = raw_size + ALLOC_HEADER_SIZE;
  blockSize = (blockSize + 7) & (~7);


  // determining if size of header is larger than calculated block size
  if (sizeof(header) > blockSize) {
    blockSize = sizeof(header);    
  }
  
  header * newHeader = findBlock(blockSize);

  if (newHeader != NULL) {
    set_state(newHeader, ALLOCATED);
    return (header *) newHeader->data;
  }
  
  // if memory chunk was not found, search in last index of segregated free lists
  newHeader = searchLast(blockSize);
  if (newHeader != NULL) {

    // if size of chunk is equal to requested size or it is not 32 bytes greater than requested size, return chunk
    if (get_size(newHeader) == blockSize || get_size(newHeader) - blockSize < sizeof(header)) {
      set_state(newHeader, ALLOCATED);
      return (header *) newHeader->data;
    }

    header * remainderHead = splitBlock(newHeader, blockSize);

    // then insert remaining block back into segregated free list 
    insertIntoFreeList(get_size(remainderHead), remainderHead);

    set_state(newHeader, ALLOCATED);
    return (header *) newHeader->data;

  }

  size_t totalNew = 0;

  do {
    // getting a new chunk of memory from OS
    newHeader = allocate_chunk(ARENA_SIZE);

    // can use OSchunklist to access left fencepost of new chunk
    header * newLeftPost = get_left_header(newHeader);
    header * oldRightPost = get_left_header(newLeftPost);
    header * leftChunk = get_left_header(oldRightPost);

    // if the memory to the left of the new chunk is unallocated then coalesce with new chunk
    if (get_state(leftChunk) == UNALLOCATED && get_size(leftChunk) != 0) {

      removeFromFreeList(leftChunk);
      size_t newSize = get_size(newHeader);
      
      // point newHeader to leftChunk
      newHeader = leftChunk;

      set_size(newHeader, get_size(leftChunk)  + get_size(newLeftPost) + get_size(oldRightPost) +  newSize);  
      
    // if memory to left of new chunk is allocated, simply remove adjacent fenceposts
    } else if (get_state(leftChunk) == ALLOCATED) {
      set_state(newLeftPost, UNALLOCATED);
      size_t newSize = get_size(newHeader);

      newHeader = oldRightPost;
      set_state(newHeader, UNALLOCATED);
      
      set_size(newHeader, newSize + 2 * get_size(newLeftPost));

    } else {
      insert_os_chunk(newLeftPost);
    }

    // update left size of newly coalesced block
    newHeader->left_size = get_size(get_left_header(newHeader));
    
    // updating lastFencePost pointer to point to right of new OS chunk
    lastFencePost = get_right_header(newHeader);
    lastFencePost->left_size = get_size(newHeader);

    totalNew = totalNew + get_size(newHeader);

  } while (totalNew < blockSize);

  if (get_size(newHeader) == blockSize || get_size(newHeader) - blockSize < sizeof(header)) {
    set_state(newHeader, ALLOCATED);
    return (header *) newHeader->data;
  }

  header * remainderHead = splitBlock(newHeader, blockSize);
  insertIntoFreeList(get_size(remainderHead), remainderHead);

  set_state(newHeader, ALLOCATED);
  return (header *) newHeader->data;

}

static inline header * splitBlock(header * block, size_t blockSize) {
  // need to split block and allocate left-most to user
  size_t remainder = get_size(block) - blockSize;
  set_size(block, blockSize);

  header * remainderHead = get_right_header(block);
  set_size(remainderHead, remainder);
  
  // updating left->size of remainderHead and block to the right of remainderHead
  remainderHead->left_size = blockSize;
  header * rightHeader = get_right_header(remainderHead);
  rightHeader->left_size = remainder;
  
  return remainderHead;
}


static inline header * findBlock(size_t blockSize) {
  // searching through segregated free list to find chunk of memory
  for (int i = 0; i < N_LISTS - 1; i++) {
    header* sentinel = &freelistSentinels[i];

    if (sentinel->next != sentinel && get_size(sentinel->next) >= blockSize) {

      header * headerPtr = sentinel->next;

      // checks if there is only one block other than sentinel
      if (sentinel->next->next == sentinel) {
        sentinel->next = sentinel;
        sentinel->prev = sentinel;

      // if not, simply remove from list
      } else {
        sentinel->next->next->prev = sentinel;
        sentinel->next = sentinel->next->next;
      }

      // if size of chunk is equal to requested size or it is not 32 bytes greater than requested size, return chunk
      if (get_size(headerPtr) == blockSize || get_size(headerPtr) - blockSize < sizeof(header)) {
        return headerPtr;
      }

      header * remainderHead = splitBlock(headerPtr, blockSize);

      // then insert remaining block back into segregated free list 
      insertIntoFreeList(get_size(remainderHead), remainderHead);

      return headerPtr;
    }
  }

  return NULL;
}


static inline header * searchLast(size_t blockSize) {
  header* sentinel = &freelistSentinels[N_LISTS - 1];
  header* current = sentinel->next;
  
  while (current != sentinel) {
    if (get_size(current) >= blockSize) {
      // checks if there is only one block other than sentinel
      if (sentinel->next->next == sentinel) {
        sentinel->next = sentinel;
        sentinel->prev = sentinel;


      // if not, simply remove from list
      } else {
        sentinel->next->next->prev = sentinel;
        sentinel->next = sentinel->next->next;
      }

      return current;
    }
    current = current->next;
  }

  return NULL;
}

static inline void insertIntoFreeList(size_t remainder, header * remainderHead) {
  size_t index = (remainder - 16)/8 - 2;
  header * sentinel;

  if (index >= N_LISTS - 1) {
    sentinel = &freelistSentinels[N_LISTS - 1];
  } else {
    sentinel = &freelistSentinels[index];
  }

  header * last = sentinel->prev;
  remainderHead->next = sentinel;
  sentinel->prev = remainderHead;
  remainderHead->prev = last;
  last->next = remainderHead;

  set_state(remainderHead, UNALLOCATED);
}

/**
 * @brief Helper to get the header from a pointer allocated with malloc
 *
 * @param p pointer to the data region of the block
 *
 * @return A pointer to the header of the block
 */
static inline header * ptr_to_header(void * p) {
  return (header *)((char *) p - ALLOC_HEADER_SIZE); //sizeof(header));
}

/**
 * @brief Helper to manage deallocation of a pointer returned by the user
 *
 * @param p The pointer returned to the user by a call to malloc
 */
static inline void deallocate_object(void * p) {

  if (p == NULL) {
    return;
  } 
  
  // retrieving headers to left and right of the header to be freed
  header * freeHeader = ptr_to_header(p);
  
  if (get_state(freeHeader) == UNALLOCATED || get_state(freeHeader) == FENCEPOST) {
    printf("Double Free Detected\n");
    assert(false);
    return;
  }

  header * leftHeader = get_left_header(freeHeader);
  header * rightHeader = get_right_header(freeHeader);

  // Check state of memory chunks surrounding freeHeader
  if (get_state(rightHeader) != FENCEPOST && get_state(rightHeader) == UNALLOCATED 
  && get_state(leftHeader) != FENCEPOST && get_state(leftHeader) == UNALLOCATED) {
    size_t freeSize = get_size(freeHeader);

    // update the free list
    removeFromFreeList(rightHeader);
    removeFromFreeList(leftHeader);

    // must point freeHeader to leftHeader since it is more 'left' in heap than freeHeader
    freeHeader = leftHeader;
    set_size(freeHeader, get_size(leftHeader) + get_size(rightHeader) + freeSize);

  } else if (get_state(rightHeader) != FENCEPOST && get_state(rightHeader) == UNALLOCATED) {
    set_size(freeHeader, get_size(freeHeader) + get_size(rightHeader));

    // update the free list
    removeFromFreeList(rightHeader);

  } else if (get_state(leftHeader) != FENCEPOST && get_state(leftHeader) == UNALLOCATED) {
    size_t freeSize = get_size(freeHeader);

    // update the free list
    removeFromFreeList(leftHeader);
    
    // must point freeHeader to leftHeader since it is more 'left' in heap than freeHeader
    freeHeader = leftHeader;
    set_size(freeHeader, get_size(leftHeader) + freeSize);

  }

  rightHeader = get_right_header(freeHeader);
  rightHeader->left_size = get_size(freeHeader);

  leftHeader = get_left_header(freeHeader);
  freeHeader->left_size = get_size(leftHeader);

  // make sure to update the free list after coalescing
  set_state(freeHeader, UNALLOCATED);
  insertIntoFreeList(get_size(freeHeader), freeHeader);

}

static inline void removeFromFreeList(header * neighbor) { 
  neighbor->prev->next = neighbor->next;
  neighbor->next->prev = neighbor->prev;
}


/**
 * @brief Helper to detect cycles in the free list
 * https://en.wikipedia.org/wiki/Cycle_detection#Floyd's_Tortoise_and_Hare
 *
 * @return One of the nodes in the cycle or NULL if no cycle is present
 */
static inline header * detect_cycles() {
  for (int i = 0; i < N_LISTS; i++) {
    header * freelist = &freelistSentinels[i];
    for (header * slow = freelist->next, * fast = freelist->next->next; 
         fast != freelist; 
         slow = slow->next, fast = fast->next->next) {
      if (slow == fast) {
        return slow;
      }
    }
  }
  return NULL;
}

/**
 * @brief Helper to verify that there are no unlinked previous or next pointers
 *        in the free list
 *
 * @return A node whose previous and next pointers are incorrect or NULL if no
 *         such node exists
 */
static inline header * verify_pointers() {
  for (int i = 0; i < N_LISTS; i++) {
    header * freelist = &freelistSentinels[i];
    for (header * cur = freelist->next; cur != freelist; cur = cur->next) {
      if (cur->next->prev != cur || cur->prev->next != cur) {
        return cur;
      }
    }
  }
  return NULL;
}

/**
 * @brief Verify the structure of the free list is correct by checkin for 
 *        cycles and misdirected pointers
 *
 * @return true if the list is valid
 */
static inline bool verify_freelist() {
  header * cycle = detect_cycles();
  if (cycle != NULL) {
    fprintf(stderr, "Cycle Detected\n");
    print_sublist(print_object, cycle->next, cycle);
    return false;
  }

  header * invalid = verify_pointers();
  if (invalid != NULL) {
    fprintf(stderr, "Invalid pointers\n");
    print_object(invalid);
    return false;
  }

  return true;
}

/**
 * @brief Helper to verify that the sizes in a chunk from the OS are correct
 *        and that allocated node's canary values are correct
 *
 * @param chunk AREA_SIZE chunk allocated from the OS
 *
 * @return a pointer to an invalid header or NULL if all header's are valid
 */
static inline header * verify_chunk(header * chunk) {
	if (get_state(chunk) != FENCEPOST) {
		fprintf(stderr, "Invalid fencepost\n");
		print_object(chunk);
		return chunk;
	}
	
	for (; get_state(chunk) != FENCEPOST; chunk = get_right_header(chunk)) {
		if (get_size(chunk)  != get_right_header(chunk)->left_size) {
			fprintf(stderr, "Invalid sizes\n");
			print_object(chunk);
			return chunk;
		}
	}
	
	return NULL;
}

/**
 * @brief For each chunk allocated by the OS verify that the boundary tags
 *        are consistent
 *
 * @return true if the boundary tags are valid
 */
static inline bool verify_tags() {
  for (size_t i = 0; i < numOsChunks; i++) {
    header * invalid = verify_chunk(osChunkList[i]);
    if (invalid != NULL) {
      return invalid;
    }
  }

  return NULL;
}

/**
 * @brief Initialize mutex lock and prepare an initial chunk of memory for allocation
 */
static void init() {
  // Initialize mutex for thread safety
  pthread_mutex_init(&mutex, NULL);

#ifdef DEBUG
  // Manually set printf buffer so it won't call malloc when debugging the allocator
  setvbuf(stdout, NULL, _IONBF, 0);
#endif // DEBUG

  // Allocate the first chunk from the OS
  header * block = allocate_chunk(ARENA_SIZE);

  header * prevFencePost = get_header_from_offset(block, -ALLOC_HEADER_SIZE);
  insert_os_chunk(prevFencePost);

  lastFencePost = get_header_from_offset(block, get_size(block));

  // Set the base pointer to the beginning of the first fencepost in the first
  // chunk from the OS
  base = ((char *) block) - ALLOC_HEADER_SIZE; //sizeof(header);

  // Initialize freelist sentinels
  for (int i = 0; i < N_LISTS; i++) {
    header * freelist = &freelistSentinels[i];
    freelist->next = freelist;
    freelist->prev = freelist;
  }

  // Insert first chunk into the free list
  header * freelist = &freelistSentinels[N_LISTS - 1];
  freelist->next = block;
  freelist->prev = block;
  block->next = freelist;
  block->prev = freelist;
}

/* 
 * External interface
 */
void * my_malloc(size_t size) {
  pthread_mutex_lock(&mutex);
  header * hdr = allocate_object(size); 
  pthread_mutex_unlock(&mutex);
  return hdr;
}

void * my_calloc(size_t nmemb, size_t size) {
  return memset(my_malloc(size * nmemb), 0, size * nmemb);
}

void * my_realloc(void * ptr, size_t size) {
  void * mem = my_malloc(size);
  memcpy(mem, ptr, size);
  my_free(ptr);
  return mem; 
}

void my_free(void * p) {
  pthread_mutex_lock(&mutex);
  deallocate_object(p);
  pthread_mutex_unlock(&mutex);
}

bool verify() {
  return verify_freelist() && verify_tags();
}
