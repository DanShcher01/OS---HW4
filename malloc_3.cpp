#include <cstddef>
#include <cstdint>
#include <cstring>
#include <unistd.h>
#include <sys/mman.h>

struct MallocMetadata {
    size_t size;
    bool is_free;
    bool is_mmap;
    MallocMetadata* next;
    MallocMetadata* prev;
};

MallocMetadata* free_lists[10 + 1] = {};
MallocMetadata* mmap_head = nullptr;
void* heap_base = nullptr;
bool initialized = false;

int order_for_block_size(size_t block_size)
{
    int order = 0;
    size_t current = 128;

    while (current < block_size && order < 10) {
        current <<= 1;
        ++order;
    }

    return order;
}

int order_for_request(size_t total_size)
{
    return order_for_block_size(total_size);
}

void clear_links(MallocMetadata* block)
{
    block->next = nullptr;
    block->prev = nullptr;
}

void insert_free_block(MallocMetadata* block)
{
    const int order = order_for_block_size(block->size);
    MallocMetadata*& head = free_lists[order];

    block->is_free = true;
    block->is_mmap = false;
    clear_links(block);

    if (head == nullptr) {
        head = block;
        return;
    }

    const uintptr_t block_address = reinterpret_cast<uintptr_t>(block);
    MallocMetadata* current = head;
    MallocMetadata* previous = nullptr;

    while (current != nullptr &&
           reinterpret_cast<uintptr_t>(current) < block_address) {
        previous = current;
        current = current->next;
    }

    block->prev = previous;
    block->next = current;

    if (previous != nullptr) {
        previous->next = block;
    } else {
        head = block;
    }

    if (current != nullptr) {
        current->prev = block;
    }
}

void remove_free_block(MallocMetadata* block)
{
    const int order = order_for_block_size(block->size);
    MallocMetadata*& head = free_lists[order];

    if (block->prev != nullptr) {
        block->prev->next = block->next;
    } else {
        head = block->next;
    }

    if (block->next != nullptr) {
        block->next->prev = block->prev;
    }

    clear_links(block);
}

void insert_mmap_block(MallocMetadata* block)
{
    block->prev = nullptr;
    block->next = mmap_head;

    if (mmap_head != nullptr) {
        mmap_head->prev = block;
    }

    mmap_head = block;
}

void remove_mmap_block(MallocMetadata* block)
{
    if (block->prev != nullptr) {
        block->prev->next = block->next;
    } else {
        mmap_head = block->next;
    }

    if (block->next != nullptr) {
        block->next->prev = block->prev;
    }

    clear_links(block);
}

bool initialize_allocator()
{
    if (initialized) {
        return true;
    }

    void* current_break = sbrk(0);
    if (current_break == reinterpret_cast<void*>(-1)) {
        return false;
    }

    const uintptr_t current_address = reinterpret_cast<uintptr_t>(current_break);
    const size_t padding =
        (4194304 - (current_address % 4194304)) %
        4194304;

    if (padding != 0 && sbrk(padding) == reinterpret_cast<void*>(-1)) {
        return false;
    }

    void* region = sbrk(4194304);
    if (region == reinterpret_cast<void*>(-1)) {
        return false;
    }

    heap_base = region;

    for (size_t i = 0; i < 32; ++i) {
        MallocMetadata* block = reinterpret_cast<MallocMetadata*>(
            static_cast<char*>(region) + i * 131072);

        block->size = 131072;
        block->is_free = true;
        block->is_mmap = false;
        clear_links(block);
        insert_free_block(block);
    }

    initialized = true;
    return true;
}

MallocMetadata* split_block(MallocMetadata* block, int target_order)
{
    int current_order = order_for_block_size(block->size);

    while (current_order > target_order) {
        const size_t half_size = block->size / 2;
        block->size = half_size;

        MallocMetadata* buddy = reinterpret_cast<MallocMetadata*>(
            reinterpret_cast<char*>(block) + half_size);

        buddy->size = half_size;
        buddy->is_free = true;
        buddy->is_mmap = false;
        clear_links(buddy);
        insert_free_block(buddy);

        --current_order;
    }

    block->is_free = false;
    block->is_mmap = false;
    clear_links(block);
    return block;
}

MallocMetadata* buddy_of(MallocMetadata* block, size_t block_size)
{
    const uintptr_t base = reinterpret_cast<uintptr_t>(heap_base);
    const uintptr_t address = reinterpret_cast<uintptr_t>(block);
    const uintptr_t offset = address - base;
    const uintptr_t buddy_offset = offset ^ block_size;

    if (buddy_offset >= 4194304) {
        return nullptr;
    }

    return reinterpret_cast<MallocMetadata*>(base + buddy_offset);
}

void* allocate_mmap(size_t requested_size)
{
    const size_t total_size = requested_size + sizeof(MallocMetadata);

    void* region = mmap(nullptr,
                        total_size,
                        PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS,
                        -1,
                        0);

    if (region == MAP_FAILED) {
        return nullptr;
    }

    MallocMetadata* block = static_cast<MallocMetadata*>(region);
    block->size = total_size;
    block->is_free = false;
    block->is_mmap = true;
    clear_links(block);
    insert_mmap_block(block);

    return reinterpret_cast<char*>(block) + sizeof(MallocMetadata);
}

size_t regular_block_count()
{
    if (!initialized) {
        return 0;
    }

    size_t count = 0;
    size_t offset = 0;

    while (offset < 4194304) {
        MallocMetadata* block = reinterpret_cast<MallocMetadata*>(
            static_cast<char*>(heap_base) + offset);
        ++count;
        offset += block->size;
    }

    return count;
}

size_t mmap_block_count()
{
    size_t count = 0;
    for (MallocMetadata* block = mmap_head;
         block != nullptr;
         block = block->next) {
        ++count;
    }
    return count;
}

void* smalloc(size_t size)
{
    if (size == 0 || size > 100000000) {
        return nullptr;
    }

    if (!initialize_allocator()) {
        return nullptr;
    }

    const size_t total_size = size + sizeof(MallocMetadata);

    if (total_size > 131072) {
        return allocate_mmap(size);
    }

    const int requested_order = order_for_request(total_size);
    int available_order = requested_order;

    while (available_order <= 10 &&
           free_lists[available_order] == nullptr) {
        ++available_order;
    }

    if (available_order > 10) {
        return nullptr;
    }

    MallocMetadata* block = free_lists[available_order];
    remove_free_block(block);
    block = split_block(block, requested_order);

    return reinterpret_cast<char*>(block) + sizeof(MallocMetadata);
}

void* scalloc(size_t num, size_t size)
{
    if (num == 0 || size == 0 || size > 100000000 / num) {
        return nullptr;
    }

    const size_t total_size = num * size;
    void* allocation = smalloc(total_size);

    if (allocation != nullptr) {
        std::memset(allocation, 0, total_size);
    }

    return allocation;
}

void sfree(void* p)
{
    if (p == nullptr) {
        return;
    }

    MallocMetadata* block = reinterpret_cast<MallocMetadata*>(
        static_cast<char*>(p) - sizeof(MallocMetadata));

    if (block->is_mmap) {
        const size_t mapping_size = block->size;
        remove_mmap_block(block);
        munmap(block, mapping_size);
        return;
    }

    if (block->is_free) {
        return;
    }

    block->is_free = true;
    clear_links(block);

    while (block->size < 131072) {
        MallocMetadata* buddy = buddy_of(block, block->size);

        if (buddy == nullptr || buddy->is_mmap || !buddy->is_free ||
            buddy->size != block->size) {
            break;
        }

        remove_free_block(buddy);

        MallocMetadata* merged =
            reinterpret_cast<uintptr_t>(block) < reinterpret_cast<uintptr_t>(buddy)
                ? block
                : buddy;

        merged->size = block->size * 2;
        merged->is_free = true;
        merged->is_mmap = false;
        clear_links(merged);
        block = merged;
    }

    insert_free_block(block);
}

void* srealloc(void* oldp, size_t size)
{
    if (size == 0 || size > 100000000) {
        return nullptr;
    }

    if (oldp == nullptr) {
        return smalloc(size);
    }

    MallocMetadata* old_block = reinterpret_cast<MallocMetadata*>(
        static_cast<char*>(oldp) - sizeof(MallocMetadata));
    const size_t old_payload_size = old_block->size - sizeof(MallocMetadata);

    if (size <= old_payload_size) {
        return oldp;
    }

    if (!old_block->is_mmap) {
        const size_t required_total = size + sizeof(MallocMetadata);
        MallocMetadata* simulated_base = old_block;
        size_t simulated_size = old_block->size;

        while (simulated_size < required_total &&
               simulated_size < 131072) {
            MallocMetadata* buddy = buddy_of(simulated_base, simulated_size);

            if (buddy == nullptr || buddy->is_mmap || !buddy->is_free ||
                buddy->size != simulated_size) {
                break;
            }

            if (reinterpret_cast<uintptr_t>(buddy) <
                reinterpret_cast<uintptr_t>(simulated_base)) {
                simulated_base = buddy;
            }

            simulated_size *= 2;
        }

        if (simulated_size >= required_total) {
            MallocMetadata* merged_base = old_block;
            size_t merged_size = old_block->size;

            while (merged_size < simulated_size) {
                MallocMetadata* buddy = buddy_of(merged_base, merged_size);
                remove_free_block(buddy);

                if (reinterpret_cast<uintptr_t>(buddy) <
                    reinterpret_cast<uintptr_t>(merged_base)) {
                    merged_base = buddy;
                }

                merged_size *= 2;
                merged_base->size = merged_size;
                merged_base->is_free = false;
                merged_base->is_mmap = false;
                clear_links(merged_base);
            }

            void* new_payload =
                reinterpret_cast<char*>(merged_base) + sizeof(MallocMetadata);
            std::memmove(new_payload, oldp, old_payload_size);
            return new_payload;
        }
    }

    void* newp = smalloc(size);
    if (newp == nullptr) {
        return nullptr;
    }

    std::memmove(newp, oldp, old_payload_size);
    sfree(oldp);
    return newp;
}

size_t _num_free_blocks()
{
    if (!initialized) {
        return 0;
    }

    size_t count = 0;
    size_t offset = 0;

    while (offset < 4194304) {
        MallocMetadata* block = reinterpret_cast<MallocMetadata*>(
            static_cast<char*>(heap_base) + offset);

        if (block->is_free) {
            ++count;
        }

        offset += block->size;
    }

    return count;
}

size_t _num_free_bytes()
{
    if (!initialized) {
        return 0;
    }

    size_t bytes = 0;
    size_t offset = 0;

    while (offset < 4194304) {
        MallocMetadata* block = reinterpret_cast<MallocMetadata*>(
            static_cast<char*>(heap_base) + offset);

        if (block->is_free) {
            bytes += block->size - sizeof(MallocMetadata);
        }

        offset += block->size;
    }

    return bytes;
}

size_t _num_allocated_blocks()
{
    return regular_block_count() + mmap_block_count();
}

size_t _num_allocated_bytes()
{
    size_t bytes = 0;

    if (initialized) {
        size_t offset = 0;
        while (offset < 4194304) {
            MallocMetadata* block = reinterpret_cast<MallocMetadata*>(
                static_cast<char*>(heap_base) + offset);
            bytes += block->size - sizeof(MallocMetadata);
            offset += block->size;
        }
    }

    for (MallocMetadata* block = mmap_head;
         block != nullptr;
         block = block->next) {
        bytes += block->size - sizeof(MallocMetadata);
    }

    return bytes;
}

size_t _num_meta_data_bytes()
{
    return _num_allocated_blocks() * sizeof(MallocMetadata);
}

size_t _size_meta_data()
{
    return sizeof(MallocMetadata);
}
