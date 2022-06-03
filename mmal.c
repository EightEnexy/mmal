#include "mmal.h"
#include <sys/mman.h>   // mmap
#include <stdbool.h>    // bool
#include <assert.h>     // assert
#include <string.h>     // memcpy
#include <stdio.h>

#ifdef NDEBUG
/**
 * The structure header encapsulates data of a single memory block.
 *   ---+------+----------------------------+---
 *      |Header|DDD not_free DDDDD...free...|
 *   ---+------+-----------------+----------+---
 *             |-- Header.asize -|
 *             |-- Header.size -------------|
 */
typedef struct header Header;
struct header {
    /**
     * Pointer to the next header. Cyclic list. If there is no other block,
     * points to itself.
     */
    Header *next;

    /// size of the block
    size_t size;

    /**
     * Size of block in bytes allocated for program. asize=0 means the block 
     * is not used by a program.
     */
    size_t asize;
};

/**
 * The arena structure.
 *   /--- arena metadata
 *   |     /---- header of the first block
 *   v     v
 *   +-----+------+-----------------------------+
 *   |Arena|Header|.............................|
 *   +-----+------+-----------------------------+
 *
 *   |--------------- Arena.size ---------------|
 */
typedef struct arena Arena;
struct arena {

    /**
     * Pointer to the next arena. Single-linked list.
     */
    Arena *next;

    /// Arena size.
    size_t size;
};

#define PAGE_SIZE (128*1024)

#endif // NDEBUG

Arena* first_arena = NULL;

/**
 * Return size alligned to PAGE_SIZE
 */
static
size_t allign_page(size_t size) {
    return (size<PAGE_SIZE)?PAGE_SIZE:((size/PAGE_SIZE)+1)*PAGE_SIZE;
}

/**
 * Allocate a new arena using mmap.
 * @param req_size requested size in bytes. Should be alligned to PAGE_SIZE.
 * @return pointer to a new arena, if successfull. NULL if error.
 * @pre req_size > sizeof(Arena) + sizeof(Header)
 */
/**
 *   +-----+------------------------------------+
 *   |Arena|....................................|
 *   +-----+------------------------------------+
 *
 *   |--------------- Arena.size ---------------|
 */
static
Arena* arena_alloc(size_t req_size){
    /// Check function arguments
    if(req_size <= sizeof(Arena)+sizeof(Header))
        fprintf(stderr,"%s\n","Arena Allocation Failed");
    
    /// Map the requested space into virtual memory
    size_t arena_size = allign_page(req_size);
    Arena* tmp = mmap(  NULL, arena_size,
                        PROT_WRITE|PROT_READ,
                        MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(tmp == NULL) return NULL;
    
    /// Initialize 'tmp' structure
    tmp->next = NULL;
    tmp->size = arena_size;
    return tmp;
}

/**
 * Appends a new arena to the end of the arena list.
 * @param a already allocated arena
 */
static
void arena_append(Arena* a){
    /// Check function argument
    if(first_arena == NULL){
        first_arena = a;
        return;
    }

    /// Add 'a' to the end of 'first_arena' linked list
    Arena* last_arena = first_arena;
    while(last_arena->next != NULL) last_arena = last_arena->next;
    last_arena -> next = a;
}

/**
 * Header structure constructor (alone, not used block).
 * @param hdr       pointer to block metadata.
 * @param size      size of free block
 * @pre size > 0
 */
/**
 *   +-----+------+------------------------+----+
 *   | ... |Header|........................| ...|
 *   +-----+------+------------------------+----+
 *
 *                |-- Header.size ---------|
 */
static
void hdr_ctor(Header* hdr, size_t size){
    /// Check function arguments
    if(hdr == NULL) return;
    if(size <= 0)
        fprintf(stderr,"Wrong size\n");

    /// Initialize the header
    hdr -> size  = size;
    hdr -> asize = 0;
    hdr -> next  = NULL;
}

/**
 * Checks if the given free block should be split in two separate blocks.
 * @param hdr       header of the free block
 * @param size      requested size of data
 * @return true if the block should be split
 * @pre hdr->asize == 0
 * @pre size > 0
 */
static
bool hdr_should_split(Header *hdr, size_t size){
    /// Check function arguments & necessary conditions
    if(hdr == NULL || hdr->asize != 0 || size == 0) return false;

    /// Check if remaining size isn't 0
    return (hdr->size-sizeof(Header)-size) > 0;
}

/**
 * Splits one block in two.
 * @param hdr       pointer to header of the big block
 * @param req_size  requested size of data in the (left) block.
 * @return pointer to the new (right) block header.
 * @pre   (hdr->size >= req_size + 2*sizeof(Header))
 */
/**
 * Before:        |---- hdr->size ---------|
 *
 *    -----+------+------------------------+----
 *         |Header|........................|
 *    -----+------+------------------------+----
 *            \----hdr->next---------------^
 */
/**
 * After:         |- req_size -|
 *
 *    -----+------+------------+------+----+----
 *     ... |Header|............|Header|....|
 *    -----+------+------------+------+----+----
 *             \---next--------^  \--next--^
 */
static
Header* hdr_split(Header* hdr, size_t req_size){
    /// Check function arguments
    if(hdr == NULL || req_size > hdr->size) return NULL;

    /// Create new header
    Header* new_hdr = (Header*)(((char*)&hdr[1])+req_size);
    hdr_ctor(new_hdr, hdr->size-req_size-sizeof(Header));

    /// Set header size
    hdr -> size = req_size;

    /// Reassign linked list pointers
    new_hdr -> next = hdr->next;
    hdr -> next = new_hdr;

    return new_hdr;
}

/**
 * Detect if two adjacent blocks could be merged.
 * @param left      left block
 * @param right     right block
 * @return true if two block are free and adjacent in the same arena.
 * @pre left->next == right
 * @pre left != right
 */
static
bool hdr_can_merge(Header* left, Header* right){   
    /// Check function arguments
    if(left==NULL || right==NULL) return false;

    /// Check if both headers are in one arena
    Arena* last_arena = first_arena;
    while(last_arena->next != NULL){
        if(right->next!=(Header*)(&last_arena[1])) return false;
        last_arena = last_arena->next;
    }

    /// Check if headers are both free and adjecent
    return (left->asize==0 && right->asize==0 && left->next==right && left != right && left < right);
}

/**
 * Merge two adjacent free blocks.
 * @param left      left block
 * @param right     right block
 * @pre left->next == right
 * @pre left != right
 */
static
void hdr_merge(Header *left, Header *right){
    /// Check function arguments
    if(left==NULL || right==NULL || left->next != right || left == right) return;

    /// Set new header size
    left->size+=right->size+sizeof(Header);

    /// Reassign 'Header' linked list pointers
    left->next=right->next;
}

/**
 * Finds the first free block that fits to the requested size.
 * @param size      requested size
 * @return pointer to the header of the block or NULL if no block is available.
 * @pre size > 0
 */
static
Header* first_fit(size_t size)
{
    /// Check function argument
    if(size <= 0 || first_arena == NULL) return NULL;

    /// Loop through each header and check for size and availability
    Header* appropriate_hdr = (Header*)(&first_arena[1]);
    while(appropriate_hdr->next != (Header*)(&first_arena[1])){
        if(appropriate_hdr->asize == 0 && appropriate_hdr->size >= size)
            return appropriate_hdr;
        appropriate_hdr = appropriate_hdr->next;
    }
    if(appropriate_hdr->asize == 0 && appropriate_hdr->size >= size)
        return appropriate_hdr;
    else 
        return NULL;
}

/**
 * Search the header which is the predecessor to the hdr. Note that if 
 * @param hdr       successor of the search header
 * @return pointer to predecessor, hdr if there is just one header.
 * @pre first_arena != NULL
 * @post predecessor->next == hdr
 */
static
Header* hdr_get_prev(Header* hdr){
    /// Check function argument
    if(first_arena == NULL || hdr == NULL) return hdr;

    /// Loop through 'Header' linked list and find 'hdr' predecessor
    Header* temp = hdr;
    while(temp->next != hdr) temp = temp->next;
    return temp;
}

/**
 * Allocate memory. Use first-fit search of available block.
 * @param size      requested size for program
 * @return pointer to allocated data or NULL if error or size = 0.
 */
void* mmalloc(size_t size){
    /// Check function argument
    if(size <= 0) return NULL;

    /// Find free space
    Header* free_hdr = first_fit(size);
    if(free_hdr != NULL){
        /// True: Split unsed space

        if(hdr_should_split(free_hdr, size))
            hdr_split(free_hdr, size);

    }
    else{
        /// False: Create new space
        Arena* new_arena = arena_alloc(size+sizeof(Arena)+sizeof(Header));
        if(new_arena == NULL) fprintf(stderr,"Arena Allocation Failed\n");
        arena_append(new_arena);
        free_hdr = (Header*)(&new_arena[1]);
        hdr_ctor(free_hdr, new_arena->size-sizeof(Arena)-sizeof(Header));

        /// Assign 'Header' linked list pointers
        free_hdr->next = (Header*)(&first_arena[1]);
        hdr_get_prev((Header*)(&first_arena[1])) -> next = free_hdr;

        /// Split unsued space

        if(hdr_should_split(free_hdr, size)){
            hdr_split(free_hdr, size);
        }

    }
    free_hdr->asize = size;
    return &free_hdr[1];
}

/**
 * Free memory block.
 * @param ptr       pointer to previously allocated data
 * @pre ptr != NULL
 */
void mfree(void* ptr){
    /// Check function argument
    if(ptr!=NULL){
        /// "Take" away the data
        Header* free_hdr=&((Header*)ptr)[-1];
        free_hdr->asize=0;

        /// Check if headers can merge
        if(hdr_can_merge(free_hdr,free_hdr->next))
            hdr_merge(free_hdr,free_hdr->next);
        if(hdr_can_merge(hdr_get_prev(free_hdr),free_hdr))
            hdr_merge(hdr_get_prev(free_hdr),free_hdr);
    }
}

/**
 * Reallocate previously allocated block.
 * @param ptr       pointer to previously allocated data
 * @param size      a new requested size. Size can be greater, equal, or less
 * then size of previously allocated block.
 * @return pointer to reallocated space or NULL if size equals to 0.
 * @post header_of(return pointer)->size == size
 */
void* mrealloc(void* ptr, size_t size){
    /// Check function arguments
    if(ptr == NULL) return NULL;
    if(size == 0){
        mfree(ptr);
        return NULL;
    }

    /// Check if location has to be changed
    Header* used_hdr = &((Header*)ptr)[-1];
    if(size < used_hdr->size){ // 'size' is smaller than is allocated
        /// Split unused space
        used_hdr->asize = 0;
        if(hdr_should_split(used_hdr, size))
            hdr_split(used_hdr, size);

        /// Set new 'asize'
        used_hdr->asize = size;
        return ptr;
    }
    else if(size == used_hdr->size){ // 'size' is equal to already allocated size 
        return ptr;
    }
    else{ // 'size' is bigger than is allocated
        /// Check if current header has enough space
        if(used_hdr->size >= size){
            used_hdr->asize = size;
            return ptr;
        }

        /// Check if next header is free and has enough space
        size_t hdr_asize = used_hdr->asize;
        used_hdr->asize = 0;
        if( used_hdr->next->asize == 0
            && used_hdr->next->size+sizeof(Header) >= size-used_hdr->size
            && hdr_can_merge(used_hdr, used_hdr->next)){
            /// True: Merge headers
            hdr_merge(used_hdr, used_hdr->next);

            /// Split unused space
            if(hdr_should_split(used_hdr, size))
                hdr_split(used_hdr, size);

            /// Set new 'asize'
            used_hdr->asize = size;
            return ptr;
        }
        else{
            /// False: Find or allocate new space
            Header* new_hdr = &((Header*)mmalloc(size))[-1];

            /// Copy old data into new space
            memcpy(&new_hdr[1], &used_hdr[1], hdr_asize);

            /// Free old space
            mfree(&used_hdr[1]);
            return &new_hdr[1];
        }
    }
}
