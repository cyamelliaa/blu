#pragma once
/**
 * mcbe_sdk.h
 * Lightweight helpers for calling into libminecraftpe.so
 * via vtable or resolved symbol, without linking against it.
 *
 * Update symbol names / offsets per target MCBE version.
 * Reference: https://github.com/mcbedrock/bedrock-headers
 */

#include <dlfcn.h>
#include <cstdint>
#include <cstring>

// ----------------------------------------------------------------
//  Generic vtable call
// ----------------------------------------------------------------
template<typename Ret, typename T, typename... Args>
inline Ret callVtable(T* obj, int index, Args... args) {
    using FnType = Ret(*)(T*, Args...);
    void** vtable = *(void***)obj;
    return reinterpret_cast<FnType>(vtable[index])(obj, args...);
}

// ----------------------------------------------------------------
//  Symbol resolver — finds a mangled symbol in libminecraftpe.so
// ----------------------------------------------------------------
inline void* resolveSymbol(const char* sym) {
    static void* mcpe = nullptr;
    if (!mcpe) {
        mcpe = dlopen("libminecraftpe.so", RTLD_NOW | RTLD_NOLOAD);
    }
    return mcpe ? dlsym(mcpe, sym) : nullptr;
}

// ----------------------------------------------------------------
//  Vtable indices — UPDATE FOR YOUR MCBE VERSION
//  Decompile with Ghidra/IDA and cross-reference bedrock-headers.
// ----------------------------------------------------------------
namespace VtableIdx {
    // Actor (base class of all entities/players)
    constexpr int getHealth          = 41;  // example — verify!
    constexpr int getMaxHealth       = 42;
    constexpr int isAlive            = 5;

    // Player
    constexpr int getFoodLevel       = 0x120 / 8;  // offset in bytes -> index
    constexpr int getSaturation      = 0x124 / 8;
    constexpr int getXpLevel         = 0x150 / 8;
    constexpr int getInventory       = 0x180 / 8;
}

// ----------------------------------------------------------------
//  ItemStack helpers (resolved at runtime)
// ----------------------------------------------------------------
namespace ItemHelper {
    // int ItemStack::getDamageValue() const
    inline int getDamage(void* item) {
        if (!item) return 0;
        using Fn = int(*)(void*);
        static auto fn = (Fn)resolveSymbol(
            "_ZNK9ItemStack14getDamageValueEv");
        return fn ? fn(item) : 0;
    }

    // int ItemStack::getMaxDamage() const  (via Item*)
    inline int getMaxDamage(void* item) {
        if (!item) return 1;
        using Fn = int(*)(void*);
        static auto fn = (Fn)resolveSymbol(
            "_ZNK9ItemStack12getMaxDamageEv");
        return fn ? fn(item) : 1;
    }

    // bool ItemStack::isNull() const
    inline bool isNull(void* item) {
        if (!item) return true;
        using Fn = bool(*)(void*);
        static auto fn = (Fn)resolveSymbol(
            "_ZNK9ItemStack6isNullEv");
        return fn ? fn(item) : true;
    }

    inline float getDurability(void* item) {
        int dmg = getDamage(item);
        int max = getMaxDamage(item);
        if (max <= 0) return 1.f;  // unbreakable or stackable
        return 1.f - (float)dmg / (float)max;
    }
}

// ----------------------------------------------------------------
//  PlayerInventory helpers
// ----------------------------------------------------------------
namespace InventoryHelper {
    // ItemStack* SimpleContainer::getItem(int slot) const
    inline void* getSlot(void* container, int slot) {
        using Fn = void*(*)(void*, int);
        static auto fn = (Fn)resolveSymbol(
            "_ZNK15SimpleContainer7getItemEi");
        return fn ? fn(container, slot) : nullptr;
    }

    // Armor slots in MCBE inventory: 0=head 1=chest 2=legs 3=feet
    // These are inside PlayerInventory, offset from the base
    // Adjust the slot numbers if needed for your version
    inline void* getArmorSlot(void* playerInventory, int armorIndex) {
        // PlayerInventory stores armor in a separate sub-container
        // Typically accessed via: playerInventory->getArmor().getItem(index)
        // This is a simplified stand-in — check headers for your version
        return getSlot(playerInventory, 36 + armorIndex);
    }

    inline void* getOffhandSlot(void* playerInventory) {
        return getSlot(playerInventory, 40);
    }
}

// ----------------------------------------------------------------
//  ClientInstance / LocalPlayer resolution
// ----------------------------------------------------------------
namespace MCBE {
    // ClientInstance::getInstance()
    inline void* getClientInstance() {
        using Fn = void*(*)();
        static auto fn = (Fn)resolveSymbol(
            "_ZN14ClientInstance11getInstanceEv");
        return fn ? fn() : nullptr;
    }

    // ClientInstance::getLocalPlayer()
    inline void* getLocalPlayer(void* clientInstance) {
        using Fn = void*(*)(void*);
        static auto fn = (Fn)resolveSymbol(
            "_ZN14ClientInstance14getLocalPlayerEv");
        return fn ? fn(clientInstance) : nullptr;
    }

    // Player::getSupplies() -> PlayerInventory*
    inline void* getInventory(void* player) {
        using Fn = void*(*)(void*);
        static auto fn = (Fn)resolveSymbol(
            "_ZN6Player11getSuppliesEv");
        return fn ? fn(player) : nullptr;
    }

    // Actor::getHealth() via attribute component
    inline float getHealth(void* actor) {
        using Fn = float(*)(void*);
        static auto fn = (Fn)resolveSymbol(
            "_ZNK5Actor9getHealthEv");
        return fn ? fn(actor) : 20.f;
    }

    inline float getMaxHealth(void* actor) {
        using Fn = float(*)(void*);
        static auto fn = (Fn)resolveSymbol(
            "_ZNK5Actor12getMaxHealthEv");
        return fn ? fn(actor) : 20.f;
    }
}
