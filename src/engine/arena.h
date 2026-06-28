#pragma once

#include "engine/base.h"

#include <array>

namespace ol {

template <u32 CAP, typename T>
struct Arena {
    std::array<T, CAP> data{};
    std::array<u32, CAP> slot_of_id{};
    std::array<u32, CAP> id_of_slot{};
    std::array<u32, CAP> free_ids{};
    u32 count = 0;
    u32 next_id = 0;
    u32 free_count = 0;

    Arena() {
        count = 0;
        next_id = 0;
        free_count = 0;
        slot_of_id.fill(invalid_id);
        id_of_slot.fill(invalid_id);
    }
};

template <u32 CAP, typename T>
void arena_clear(Arena<CAP, T>* arena) {
    arena->count = 0;
    arena->next_id = 0;
    arena->free_count = 0;
    arena->slot_of_id.fill(invalid_id);
    arena->id_of_slot.fill(invalid_id);
}

template <u32 CAP, typename T>
u32 arena_acquire(Arena<CAP, T>* arena, const T& value) {
    if (arena->count >= CAP) return invalid_id;

    u32 id = invalid_id;
    if (arena->free_count > 0) {
        id = arena->free_ids[--arena->free_count];
    } else {
        id = arena->next_id++;
        if (id >= CAP) return invalid_id;
    }

    const u32 slot = arena->count++;
    arena->data[slot] = value;
    arena->id_of_slot[slot] = id;
    arena->slot_of_id[id] = slot;
    return id;
}

template <u32 CAP, typename T>
u32 arena_reserve(Arena<CAP, T>* arena) {
    if (arena->count >= CAP) return invalid_id;

    u32 id = invalid_id;
    if (arena->free_count > 0) {
        id = arena->free_ids[--arena->free_count];
    } else {
        id = arena->next_id++;
        if (id >= CAP) return invalid_id;
    }

    const u32 slot = arena->count++;
    arena->id_of_slot[slot] = id;
    arena->slot_of_id[id] = slot;
    return id;
}

template <u32 CAP, typename T>
bool arena_has(const Arena<CAP, T>* arena, u32 id) {
    return id < CAP && arena->slot_of_id[id] != invalid_id;
}

template <u32 CAP, typename T>
T* arena_get(Arena<CAP, T>* arena, u32 id) {
    if (!arena_has(arena, id)) return nullptr;
    return &arena->data[arena->slot_of_id[id]];
}

template <u32 CAP, typename T>
const T* arena_get(const Arena<CAP, T>* arena, u32 id) {
    if (!arena_has(arena, id)) return nullptr;
    return &arena->data[arena->slot_of_id[id]];
}

template <u32 CAP, typename T>
u32 arena_id_at_slot(const Arena<CAP, T>* arena, u32 slot) {
    return slot < arena->count ? arena->id_of_slot[slot] : invalid_id;
}

template <u32 CAP, typename T>
bool arena_remove(Arena<CAP, T>* arena, u32 id) {
    if (!arena_has(arena, id)) return false;

    const u32 slot = arena->slot_of_id[id];
    const u32 last_slot = arena->count - 1;

    if (slot != last_slot) {
        arena->data[slot] = arena->data[last_slot];
        const u32 moved_id = arena->id_of_slot[last_slot];
        arena->id_of_slot[slot] = moved_id;
        arena->slot_of_id[moved_id] = slot;
    }

    arena->slot_of_id[id] = invalid_id;
    arena->id_of_slot[last_slot] = invalid_id;
    arena->free_ids[arena->free_count++] = id;
    arena->count--;
    return true;
}

} // namespace ol
