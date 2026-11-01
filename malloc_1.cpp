#include <iostream>
#include <cassert>
#include <unistd.h>

void* smalloc(size_t size){
    if(size == 0 || size > 100000000){
        return nullptr;
    }
    void* request = sbrk(size);
    if(request == (void*) -1){
        return nullptr;
    }
    return request;

}
