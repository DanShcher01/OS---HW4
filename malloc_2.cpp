#include <iostream>
#include <cassert>
#include <unistd.h>
#include <cstring>

struct MallocMetadata{
    size_t size;
    bool is_free;
    MallocMetadata* next;
    MallocMetadata* prev;
};


MallocMetadata* list_head = nullptr;
MallocMetadata* list_tail = nullptr;

size_t _num_free_blocks() {
    size_t count = 0;
    MallocMetadata* curr = list_head;
    while (curr != nullptr) {
        if (curr->is_free) {
            count++;
        }
        curr = curr->next;
    }
    return count;
}

size_t _num_free_bytes() {
    size_t total_free_bytes = 0;
    MallocMetadata* curr = list_head;
    while (curr != nullptr) {
        if (curr->is_free) {
            total_free_bytes += curr->size;
        }
        curr = curr->next;
    }
    return total_free_bytes;
}

size_t _num_allocated_blocks() {
    size_t count = 0;
    MallocMetadata* curr = list_head;
    while (curr != nullptr) {
        count++;
        curr = curr->next;
    }
    return count;
}

size_t _num_allocated_bytes() {
    size_t total_allocated_bytes = 0;
    MallocMetadata* curr = list_head;
    while (curr != nullptr) {
        total_allocated_bytes += curr->size;
        curr = curr->next;
    }
    return total_allocated_bytes;
}

size_t _num_meta_data_bytes() {

    return _num_allocated_blocks() * sizeof(MallocMetadata);
}

size_t _size_meta_data() {
    return sizeof(MallocMetadata);
}

void* smalloc(size_t size) {
    if (size == 0 || size > 100000000) {
        return nullptr;
    }
    MallocMetadata* curr = list_head;
    //check if exists
    while (curr != nullptr) {
        if (curr->is_free && curr->size >= size) {
            curr->is_free = false;
            return (void *) ((char *) curr + sizeof(MallocMetadata));
        }
        curr = curr->next;
    }
    //if block doesnt exist
    size_t total_size = sizeof(MallocMetadata) + size;
    void *request = sbrk(total_size);

    if (request == (void *) (-1)) {
        return nullptr;
    }
    MallocMetadata *new_meta = (MallocMetadata *) request;
    new_meta->size = size; // Note: size field in metadata is just the requested size
    new_meta->is_free = false;
    new_meta->next = nullptr;
    new_meta->prev = list_tail;

    if (list_head == nullptr) {
        list_head = new_meta;
    } else {
        list_tail->next = new_meta;
    }
    list_tail = new_meta;
    return (void *) ((char *) new_meta + sizeof(MallocMetadata));

}

void sfree(void* p){
    if(p == nullptr){
        return;
    }
    MallocMetadata* meta = (MallocMetadata*)((char*)p - sizeof(MallocMetadata));
    if(!meta->is_free){
        meta->is_free = true;
    }
}

void* scalloc(size_t num, size_t size) {
    if(num == 0 || size == 0 || size > 100000000/num){
        return nullptr;
    }
    size_t total_size = num * size;
    void* p = smalloc(total_size);
    if(p != nullptr){
        std::memset(p,0,total_size);
    }
    return p;
}

void* srealloc(void* oldp, size_t size){
    if(size == 0 || size > 100000000){
        return nullptr;
    }
    if(oldp == nullptr){
        return smalloc(size);
    }
    //get old block
    MallocMetadata* meta = (MallocMetadata*)((char*)oldp - sizeof(MallocMetadata));
    if(size <= meta->size){
        return oldp;
    }
    void* p = smalloc(size);
    if(p == nullptr){
        return nullptr;
    }
    //update block
    std::memmove(p,oldp,meta->size);
    sfree(oldp);

    return p;
}
