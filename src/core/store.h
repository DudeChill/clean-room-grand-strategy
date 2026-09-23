#pragma once
// Deterministic entity storage.
//
// Iteration is always in ascending slot index, which is creation order. Slots of
// destroyed entities are reused through an explicit free list, so iteration order
// never depends on hash-table layout, allocator behaviour or pointer values.
//
// Store<Payload> addresses entities through the strongly-typed id declared for that
// payload (ProvinceId, CountryId, ...) via the EntityTag trait below.

#include <cstdint>
#include <utility>
#include <vector>

#include "core/types.h"

namespace hoi {

// Forward declarations of payload types; the trait only needs the tag mapping.
struct Province;
struct State;
struct Region;
struct Country;
struct Division;
struct Army;
struct Battle;
struct War;
struct Character;

template <typename T>
struct EntityTag {
    using Type = Id<T>;
};

template <>
struct EntityTag<Province> {
    using Type = ProvinceId;
};
template <>
struct EntityTag<State> {
    using Type = StateId;
};
template <>
struct EntityTag<Region> {
    using Type = RegionId;
};
template <>
struct EntityTag<Country> {
    using Type = CountryId;
};
template <>
struct EntityTag<Division> {
    using Type = DivisionId;
};
template <>
struct EntityTag<Army> {
    using Type = ArmyId;
};
template <>
struct EntityTag<Battle> {
    using Type = BattleId;
};
template <>
struct EntityTag<War> {
    using Type = WarId;
};
template <>
struct EntityTag<Character> {
    using Type = CharacterId;
};

template <typename T>
class Store {
  public:
    using IdT = typename EntityTag<T>::Type;

    template <typename... Args>
    IdT create(Args&&... args) {
        uint32_t slot;
        if (!free_.empty()) {
            slot = free_.back();
            free_.pop_back();
            items_[slot] = T(std::forward<Args>(args)...);
        } else {
            slot = static_cast<uint32_t>(items_.size());
            items_.emplace_back(std::forward<Args>(args)...);
        }
        if (alive_.size() <= slot) {
            alive_.resize(slot + 1, 0);
        }
        alive_[slot] = 1;
        ++size_;
        return IdT(slot);
    }

    void destroy(IdT id) {
        if (!alive(id)) return;
        alive_[id.v] = 0;
        free_.push_back(id.v);
        --size_;
    }

    [[nodiscard]] bool alive(IdT id) const {
        return id.valid() && id.v < alive_.size() && alive_[id.v] != 0;
    }

    [[nodiscard]] T* try_get(IdT id) { return alive(id) ? &items_[id.v] : nullptr; }
    [[nodiscard]] const T* try_get(IdT id) const { return alive(id) ? &items_[id.v] : nullptr; }

    T& operator[](IdT id) { return items_[id.v]; }
    const T& operator[](IdT id) const { return items_[id.v]; }

    [[nodiscard]] size_t size() const { return size_; }
    [[nodiscard]] size_t capacity() const { return items_.size(); }
    [[nodiscard]] const std::vector<uint8_t>& alive_flags() const { return alive_; }

    template <typename F>
    void for_each(F&& f) {
        for (uint32_t i = 0; i < items_.size(); ++i) {
            if (alive_[i]) f(IdT(i), items_[i]);
        }
    }

    template <typename F>
    void for_each(F&& f) const {
        for (uint32_t i = 0; i < items_.size(); ++i) {
            if (alive_[i]) f(IdT(i), items_[i]);
        }
    }

    void clear() {
        items_.clear();
        alive_.clear();
        free_.clear();
        size_ = 0;
    }

    // Raw access for serialization only.
    std::vector<T>& raw_items() { return items_; }
    const std::vector<T>& raw_items() const { return items_; }
    std::vector<uint32_t>& raw_free_list() { return free_; }
    const std::vector<uint32_t>& raw_free_list() const { return free_; }
    void set_alive_flags(std::vector<uint8_t> flags) {
        alive_ = std::move(flags);
        size_ = 0;
        for (uint8_t a : alive_) size_ += a ? 1 : 0;
    }

  private:
    std::vector<T> items_;
    std::vector<uint8_t> alive_;
    std::vector<uint32_t> free_;
    size_t size_ = 0;
};

}  // namespace hoi
