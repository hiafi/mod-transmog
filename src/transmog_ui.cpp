/*
 * Wardrobe UI bridge: addon messages under the "CTMOG" prefix between the client's wardrobe window and
 * mod-transmog's rules.
 *
 * Client -> server: SendAddonMessage("CTMOG", body, "WHISPER", UnitName("player")). The whisper to self is caught in
 * OnPlayerCanUseChat and swallowed, so it never reaches real chat.
 * Server -> client: a LANG_ADDON whisper from the player to the player, "CTMOG\t<body>", read from CHAT_MSG_ADDON.
 *
 * Wire slots are the client's INVSLOT_* numbers (1-based); the server's EQUIPMENT_SLOT_* is wire - 1.
 *
 *   C->S HELLO                          -> CFG;<hiddenAllowed>;<hiddenFree>;<setsEnabled>;<maxSets>  then CUR
 *   S->C CUR;<slot>:<v>,...             v: 0 = none, H = hidden, - = slot empty, else the fake item entry
 *   C->S LIST;<slot>                    -> L;<slot>;<part>/<parts>;<entry>,...  (0..n chunks)  then
 *                                          LE;<slot>;<count>;<costCopper>
 *   C->S APPLY;<slot>:<entry|H|0>,...   -> R;<slot>;<code> per requested slot (0 = OK), then CUR
 *                                          (0 on an empty slot clears the slot's saved look, slot-bound mode)
 *   S->C NEW;<entry>                    pushed when an appearance is added to the account collection
 *
 * LIST and APPLY are rate limited per type (200 ms); HELLO is not.
 * Bodies are at most 240 bytes (the 3.3.5 client caps addon messages near 255 bytes including the prefix).
 */

#include "Chat.h"
#include "Item.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "Tokenize.h"
#include "Transmogrification.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <tuple>

using namespace Acore::ChatCommands;

#define sT sTransmogrification

namespace
{
    std::string const WardrobePrefix = "CTMOG";
    std::size_t constexpr MaxBodyLength = 240;
    std::chrono::milliseconds constexpr MinRequestInterval{ 200 };

    // Server EQUIPMENT_SLOT_* values mod-transmog lets players transmogrify (GetSlotName's list).
    std::array<uint8, 14> constexpr TransmogSlots =
    {
        EQUIPMENT_SLOT_HEAD, EQUIPMENT_SLOT_SHOULDERS, EQUIPMENT_SLOT_BODY, EQUIPMENT_SLOT_CHEST,
        EQUIPMENT_SLOT_WAIST, EQUIPMENT_SLOT_LEGS, EQUIPMENT_SLOT_FEET, EQUIPMENT_SLOT_WRISTS,
        EQUIPMENT_SLOT_HANDS, EQUIPMENT_SLOT_BACK, EQUIPMENT_SLOT_MAINHAND, EQUIPMENT_SLOT_OFFHAND,
        EQUIPMENT_SLOT_RANGED, EQUIPMENT_SLOT_TABARD
    };

    // HELLO is not rate limited: it only sends CFG and CUR.
    enum class WardrobeRequest : uint8
    {
        List,
        Apply,
        Max
    };

    struct WardrobeList
    {
        uint32 equippedEntry = 0;
        std::vector<uint32> entries;
    };

    struct WardrobeState
    {
        std::array<std::chrono::steady_clock::time_point, size_t(WardrobeRequest::Max)> lastRequest{};
        std::unordered_set<uint32> sentItemQueries; // item entries this session has been sent a query response for
        std::map<uint8, WardrobeList> lists;        // server slot -> list built for the item equipped then

        // Slot-bound looks (Transmogrification.WardrobeUI.SlotBound).
        std::map<uint8, uint32> slotChoices;                  // server slot -> entry, HIDDEN_ITEM_ID = hidden
        bool slotChoicesLoaded = false;                       // false while the character is still loading
        std::array<ObjectGuid, EQUIPMENT_SLOT_END> equipped{}; // last item seen in each slot, to see one leave
    };

    bool WardrobeEnabled = true;
    bool SlotBound = true;

    // Guards States only: chat (world thread) and the item hooks (map threads) both touch it. Recursive because
    // SetFakeEntry re-enters through the visible-item hook; nothing below holds it across a call into mod-transmog.
    // It does NOT guard mod-transmog's own maps (entryMap, dataMap, collectionCache): the slot-bound hooks write those
    // from map threads, which assumes MapUpdate.Threads = 1 (see TryApplySlotChoice and OnSlotItemChanged).
    std::recursive_mutex StateLock;
    using StateGuard = std::lock_guard<std::recursive_mutex>;
    std::unordered_map<ObjectGuid, WardrobeState> States;

    // Read at boot, on `.reload config` and on `.transmog reload`. A reload drops every cached list, because the rules
    // (or the collections) a list was built with may have changed.
    void LoadWardrobeConfig(bool reload)
    {
        WardrobeEnabled = sConfigMgr->GetOption<bool>("Transmogrification.WardrobeUI.Enable", true);
        SlotBound = sConfigMgr->GetOption<bool>("Transmogrification.WardrobeUI.SlotBound", true);

        if (reload)
        {
            StateGuard guard(StateLock);
            for (auto& state : States)
                state.second.lists.clear();
        }
    }

    bool IsWardrobeActive()
    {
        return WardrobeEnabled && sT->IsEnabled();
    }

    bool IsSlotBoundActive()
    {
        return SlotBound && sT->IsEnabled();
    }

    bool IsTransmogSlot(uint8 slot)
    {
        return std::find(TransmogSlots.begin(), TransmogSlots.end(), slot) != TransmogSlots.end();
    }

    // Wire slot (INVSLOT_*, 1-based) to server EQUIPMENT_SLOT_*; false if not a transmoggable slot.
    bool ParseWireSlot(std::string_view token, uint8& slot)
    {
        Optional<uint32> wire = Acore::StringTo<uint32>(token);
        if (!wire || *wire < 1 || *wire > EQUIPMENT_SLOT_END)
            return false;

        slot = uint8(*wire - 1);
        return IsTransmogSlot(slot);
    }

    // The price Transmogrification::Transmogrify charges for changing the look of `dest` (also hidden when not free).
    // A negative result is not charged there, so it is 0 here.
    uint32 SlotPrice(ItemTemplate const* dest)
    {
        int32 cost = sT->GetSpecialPrice(dest);
        cost *= sT->GetScaledCostModifier();
        cost += sT->GetCopperCost();
        return cost > 0 ? uint32(cost) : 0;
    }

    uint32 ResultCode(TransmogStrings result)
    {
        return result == LANG_TRANSMOG_OK ? 0 : uint32(result);
    }

    void SendWardrobeMessage(Player* player, std::string const& body)
    {
        WorldSession* session = player->GetSession();
        if (!session || session->IsSocketClosed())
            return;

        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player, WardrobePrefix + '\t' + body);
        player->SendDirectMessage(&data);
    }

    // Per message type, so HELLO followed at once by the first LIST is not dropped.
    bool AcceptRequest(Player* player, WardrobeRequest type)
    {
        auto const now = std::chrono::steady_clock::now();
        StateGuard guard(StateLock);
        auto& last = States[player->GetGUID()].lastRequest[size_t(type)];
        if (last.time_since_epoch().count() && now - last < MinRequestInterval)
            return false;

        last = now;
        return true;
    }

    std::string BuildCfgBody()
    {
        bool setsEnabled = false;
        uint32 maxSets = 0;
#ifdef PRESETS
        setsEnabled = sT->GetEnableSets();
        maxSets = sT->GetMaxSets();
#endif
        return Acore::StringFormat("CFG;{};{};{};{}", sT->GetAllowHiddenTransmog() ? 1 : 0,
            sT->GetHiddenTransmogIsFree() ? 1 : 0, setsEnabled ? 1 : 0, maxSets);
    }

    std::string BuildCurBody(Player* player)
    {
        std::string body = "CUR;";
        bool first = true;
        for (uint8 slot : TransmogSlots)
        {
            std::string value = "-";
            if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            {
                uint32 fake = sT->GetFakeEntry(item->GetGUID());
                value = fake == HIDDEN_ITEM_ID ? "H" : std::to_string(fake);
            }

            if (!first)
                body += ',';
            body += Acore::StringFormat("{}:{}", slot + 1, value);
            first = false;
        }
        return body;
    }

    // Every collected appearance that fits `dest`, one per (display, class, subclass), sorted like the gossip menu's
    // CmpTmog: quality high to low, then name. Template checks only: no Item is created.
    std::vector<uint32> BuildList(Player* player, ItemTemplate const* dest)
    {
        std::vector<uint32> result;
        auto collection = sT->collectionCache.find(player->GetSession()->GetAccountId());
        if (collection == sT->collectionCache.end())
            return result;

        std::map<std::tuple<uint32, uint32, uint32>, ItemTemplate const*> byDisplay;
        for (uint32 entry : collection->second)
        {
            ItemTemplate const* source = sObjectMgr->GetItemTemplate(entry);
            if (!source || !sT->CanTransmogrifyItemWithItem(player, dest, source))
                continue;

            auto key = std::make_tuple(source->DisplayInfoID, source->Class, source->SubClass);
            auto [itr, inserted] = byDisplay.emplace(key, source);
            if (inserted)
                continue;

            ItemTemplate const* kept = itr->second;
            if (source->Quality > kept->Quality || (source->Quality == kept->Quality && source->ItemId < kept->ItemId))
                itr->second = source;
        }

        std::vector<ItemTemplate const*> sorted;
        sorted.reserve(byDisplay.size());
        for (auto const& group : byDisplay)
            sorted.push_back(group.second);

        std::sort(sorted.begin(), sorted.end(), [](ItemTemplate const* a, ItemTemplate const* b)
        {
            return std::make_tuple(7 - int32(a->Quality), std::cref(a->Name1), a->ItemId) <
                std::make_tuple(7 - int32(b->Quality), std::cref(b->Name1), b->ItemId);
        });

        result.reserve(sorted.size());
        for (ItemTemplate const* source : sorted)
            result.push_back(source->ItemId);
        return result;
    }

    // The list for the item equipped in `slot`, from the cache when it was built for the same item entry.
    std::vector<uint32> GetList(Player* player, uint8 slot, Item* equipped)
    {
        std::vector<uint32> entries;
        {
            StateGuard guard(StateLock);
            WardrobeList& list = States[player->GetGUID()].lists[slot];
            if (list.equippedEntry != equipped->GetEntry())
            {
                list.equippedEntry = equipped->GetEntry();
                list.entries = BuildList(player, equipped->GetTemplate());
            }
            entries = list.entries;
        }

        // The representative of a display group may not be the entry the slot currently wears. Swap the current one
        // in, so the client can mark the cell as the current look.
        uint32 fake = sT->GetFakeEntry(equipped->GetGUID());
        if (!fake || fake == HIDDEN_ITEM_ID || std::find(entries.begin(), entries.end(), fake) != entries.end())
            return entries;

        ItemTemplate const* fakeTemplate = sObjectMgr->GetItemTemplate(fake);
        if (!fakeTemplate)
            return entries;

        for (uint32& entry : entries)
        {
            ItemTemplate const* source = sObjectMgr->GetItemTemplate(entry);
            if (source && source->DisplayInfoID == fakeTemplate->DisplayInfoID &&
                source->Class == fakeTemplate->Class && source->SubClass == fakeTemplate->SubClass)
            {
                entry = fake;
                break;
            }
        }
        return entries;
    }

    // L chunks: "L;<slot>;<part>/<parts>;<entry>,<entry>,...", each at most MaxBodyLength. None for an empty list.
    std::vector<std::string> BuildListBodies(uint8 wireSlot, std::vector<uint32> const& entries)
    {
        std::vector<std::string> bodies;
        if (entries.empty())
            return bodies;

        std::string const slotText = std::to_string(wireSlot);
        for (std::size_t partDigits = 1; ; ++partDigits)
        {
            // "L;" + slot + ";" + part + "/" + parts + ";"
            std::size_t const header = 2 + slotText.size() + 1 + partDigits + 1 + partDigits + 1;
            std::size_t const room = MaxBodyLength - header;

            std::vector<std::string> parts;
            std::string current;
            for (uint32 entry : entries)
            {
                std::string const text = std::to_string(entry);
                if (!current.empty() && current.size() + 1 + text.size() > room)
                {
                    parts.push_back(std::move(current));
                    current.clear();
                }

                if (!current.empty())
                    current += ',';
                current += text;
            }
            parts.push_back(std::move(current));

            if (std::to_string(parts.size()).size() > partDigits)
                continue;

            for (std::size_t i = 0; i < parts.size(); ++i)
                bodies.push_back(Acore::StringFormat("L;{};{}/{};{}", slotText, i + 1, parts.size(), parts[i]));
            return bodies;
        }
    }

    // Everything LIST answers with, in order (L chunks, then LE). Shared with `.transmog wardrobe list`.
    std::vector<std::string> BuildListResponse(Player* player, uint8 slot, std::vector<uint32>* entriesOut = nullptr)
    {
        uint8 const wireSlot = slot + 1;
        Item* equipped = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!equipped)
            return { Acore::StringFormat("LE;{};0;0", wireSlot) };

        std::vector<uint32> entries = GetList(player, slot, equipped);
        std::vector<std::string> bodies = BuildListBodies(wireSlot, entries);
        bodies.push_back(Acore::StringFormat("LE;{};{};{}", wireSlot, entries.size(),
            SlotPrice(equipped->GetTemplate())));

        if (entriesOut)
            *entriesOut = std::move(entries);
        return bodies;
    }

    // Item query responses for entries this session has not been sent yet, so names, icons and quality are in the
    // client's cache before the grid renders (3.3.5 GetItemInfo returns nil for uncached items and does not query).
    // Fed through the core's own CMSG_ITEM_QUERY_SINGLE handler: identical packets, localized names, no core edit.
    void PushItemQueries(Player* player, std::vector<uint32> const& entries)
    {
        std::vector<uint32> toSend;
        {
            StateGuard guard(StateLock);
            auto& sent = States[player->GetGUID()].sentItemQueries;
            for (uint32 entry : entries)
                if (sent.insert(entry).second)
                    toSend.push_back(entry);
        }

        WorldSession* session = player->GetSession();
        for (uint32 entry : toSend)
        {
            WorldPacket query(CMSG_ITEM_QUERY_SINGLE, 4);
            query << uint32(entry);
            session->HandleItemQuerySingleOpcode(query);
        }
    }

    // ---- Slot-bound looks -------------------------------------------------------------------------------------------
    // The look a player picks belongs to the equipment slot (custom_transmogrification_slots). mod-transmog's per-item
    // storage still carries the visual: the choice is copied onto whatever item is equipped in the slot (if that item
    // can take it), and taken off the item again when it leaves the slot.

    uint32 GetSlotChoice(Player* player, uint8 slot)
    {
        StateGuard guard(StateLock);
        auto itr = States.find(player->GetGUID());
        if (itr == States.end() || !itr->second.slotChoicesLoaded)
            return 0;

        auto choice = itr->second.slotChoices.find(slot);
        return choice != itr->second.slotChoices.end() ? choice->second : 0;
    }

    // entry 0 clears the slot's choice.
    void SetSlotChoice(Player* player, uint8 slot, uint32 entry)
    {
        {
            StateGuard guard(StateLock);
            auto& choices = States[player->GetGUID()].slotChoices;
            auto itr = choices.find(slot);
            if (!entry)
            {
                if (itr == choices.end())
                    return;
                choices.erase(itr);
            }
            else
            {
                if (itr != choices.end() && itr->second == entry)
                    return;
                choices[slot] = entry;
            }
        }

        if (entry)
            CharacterDatabase.Execute("REPLACE INTO custom_transmogrification_slots (Owner, Slot, FakeEntry) "
                "VALUES ({}, {}, {})", player->GetGUID().GetCounter(), slot, entry);
        else
            CharacterDatabase.Execute("DELETE FROM custom_transmogrification_slots WHERE Owner = {} AND Slot = {}",
                player->GetGUID().GetCounter(), slot);
    }

    // Puts the slot's choice on `item` (equipped in `slot`) for free, if the item can take it and the look is still
    // in the account collection. Otherwise the choice stays stored, dormant, and the item keeps its own look.
    void TryApplySlotChoice(Player* player, uint8 slot, Item* item)
    {
        uint32 const choice = GetSlotChoice(player, slot);
        if (!choice || sT->GetFakeEntry(item->GetGUID()) == choice)
            return;

        if (choice == HIDDEN_ITEM_ID)
        {
            // Map thread, no lock: assumes MapUpdate.Threads = 1 (see the SetFakeEntry call below).
            if (sT->GetAllowHiddenTransmog())
                sT->SetFakeEntry(player, HIDDEN_ITEM_ID, slot, item);
            return;
        }

        ItemTemplate const* source = sObjectMgr->GetItemTemplate(choice);
        if (!source || !sT->GetUseCollectionSystem())
            return;

        auto collection = sT->collectionCache.find(player->GetSession()->GetAccountId());
        if (collection == sT->collectionCache.end() || !collection->second.contains(choice))
            return;

        if (!sT->CanTransmogrifyItemWithItem(player, item->GetTemplate(), source))
            return;

        // Only the fake entry: unlike a paid transmog, an automatic one does not bind the item or end its refund or
        // trade window.
        // Thread safety: this runs from OnPlayerEquip on a map thread and writes mod-transmog's shared entryMap and
        // dataMap without a lock. That assumes MapUpdate.Threads = 1 (the live value), the same assumption stock
        // mod-transmog makes for its own map-thread writes. No lock is taken here.
        sT->SetFakeEntry(player, choice, slot, item);
    }

    // Login: load the choices, then bring the equipped items in line. An item that already carries a look in a slot
    // without a choice (looks made before slot binding) makes that look the slot's choice.
    void LoadSlotChoices(Player* player)
    {
        std::map<uint8, uint32> choices;
        if (QueryResult result = CharacterDatabase.Query("SELECT Slot, FakeEntry FROM custom_transmogrification_slots "
            "WHERE Owner = {}", player->GetGUID().GetCounter()))
        {
            do
            {
                uint8 slot = (*result)[0].Get<uint8>();
                uint32 entry = (*result)[1].Get<uint32>();
                if (IsTransmogSlot(slot) && (entry == HIDDEN_ITEM_ID || sObjectMgr->GetItemTemplate(entry)))
                    choices[slot] = entry;
            } while (result->NextRow());
        }

        {
            StateGuard guard(StateLock);
            WardrobeState& state = States[player->GetGUID()];
            state.slotChoices = std::move(choices);
            state.slotChoicesLoaded = true;
        }

        if (!IsSlotBoundActive())
            return;

        for (uint8 slot : TransmogSlots)
        {
            Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!item)
                continue;

            uint32 const fake = sT->GetFakeEntry(item->GetGUID());
            if (!GetSlotChoice(player, slot))
            {
                if (fake)
                    SetSlotChoice(player, slot, fake);
            }
            else
                TryApplySlotChoice(player, slot, item);
        }
    }

    // An item left `slot` (unequipped, swapped, destroyed, moved to another slot): if it wears the slot's choice, the
    // look stays with the slot, so it comes off the item.
    void OnSlotItemChanged(Player* player, uint8 slot, Item* item)
    {
        ObjectGuid const current = item ? item->GetGUID() : ObjectGuid::Empty;
        ObjectGuid previous;
        uint32 choice = 0;
        {
            StateGuard guard(StateLock);
            WardrobeState& state = States[player->GetGUID()];
            previous = state.equipped[slot];
            if (previous == current)
                return;

            state.equipped[slot] = current;
            if (state.slotChoicesLoaded)
            {
                auto itr = state.slotChoices.find(slot);
                if (itr != state.slotChoices.end())
                    choice = itr->second;
            }
        }

        if (!previous || !choice || !IsSlotBoundActive())
            return;

        // DeleteFakeFromDB, not DeleteFakeEntry: the item is no longer in the slot (nothing to redraw), and this is
        // not the player removing the look, so the slot keeps its choice.
        // Thread safety: this runs from OnPlayerAfterSetVisibleItemSlot on a map thread and erases from mod-transmog's
        // shared entryMap and dataMap without a lock. That assumes MapUpdate.Threads = 1 (the live value), the same
        // assumption stock mod-transmog makes for its own map-thread writes. No lock is taken here.
        if (sT->GetFakeEntry(previous) == choice)
            sT->DeleteFakeFromDB(previous.GetCounter());
    }

    void HandleHello(Player* player)
    {
        SendWardrobeMessage(player, BuildCfgBody());
        SendWardrobeMessage(player, BuildCurBody(player));
    }

    void HandleList(Player* player, std::vector<std::string_view> const& fields)
    {
        uint8 slot = 0;
        if (fields.size() < 2 || !ParseWireSlot(fields[1], slot))
        {
            // Answer a bad slot with an empty list, so the client does not wait for one.
            if (fields.size() >= 2)
                if (Optional<uint32> wire = Acore::StringTo<uint32>(fields[1]))
                    if (*wire >= 1 && *wire <= EQUIPMENT_SLOT_END)
                        SendWardrobeMessage(player, Acore::StringFormat("LE;{};0;0", *wire));
            return;
        }

        std::vector<uint32> entries;
        std::vector<std::string> bodies = BuildListResponse(player, slot, &entries);
        PushItemQueries(player, entries);
        for (std::string const& body : bodies)
            SendWardrobeMessage(player, body);
    }

    enum class ApplyKind : uint8
    {
        Entry,
        Hide,
        Remove
    };

    struct ApplyChange
    {
        uint8 wireSlot = 0;
        uint8 slot = 0;
        ApplyKind kind = ApplyKind::Remove;
        uint32 entry = 0;
        Item* item = nullptr;
        uint32 cost = 0;
        bool noop = false;
        uint32 code = 0; // TransmogStrings value of a failed check, 0 = passed
    };

    void ValidateChange(Player* player, ApplyChange& change)
    {
        change.item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, change.slot);
        if (!change.item)
        {
            // "0" on an empty slot only clears the slot's saved choice (done after the batch is applied).
            if (change.kind == ApplyKind::Remove)
            {
                change.noop = true;
                return;
            }

            change.code = LANG_TRANSMOG_MISSING_DEST_ITEM;
            return;
        }

        uint32 const current = sT->GetFakeEntry(change.item->GetGUID());
        ItemTemplate const* dest = change.item->GetTemplate();

        switch (change.kind)
        {
            case ApplyKind::Remove:
                change.noop = !current;
                return;
            case ApplyKind::Hide:
                if (!sT->GetAllowHiddenTransmog())
                {
                    change.code = LANG_TRANSMOG_INVALID_SRC_ENTRY;
                    return;
                }
                change.noop = current == HIDDEN_ITEM_ID;
                if (!change.noop && !sT->GetHiddenTransmogIsFree())
                    change.cost = SlotPrice(dest);
                return;
            case ApplyKind::Entry:
            {
                ItemTemplate const* source = sObjectMgr->GetItemTemplate(change.entry);
                if (!source)
                {
                    change.code = LANG_TRANSMOG_MISSING_SRC_ITEM;
                    return;
                }

                // Transmogrify(uint32) does not check the collection; the wardrobe only offers collected looks.
                auto collection = sT->collectionCache.find(player->GetSession()->GetAccountId());
                if (!sT->GetUseCollectionSystem() || collection == sT->collectionCache.end() ||
                    !collection->second.contains(change.entry))
                {
                    change.code = LANG_TRANSMOG_INVALID_SRC_ENTRY;
                    return;
                }

                if (!sT->CanTransmogrifyItemWithItem(player, dest, source))
                {
                    change.code = LANG_TRANSMOG_INVALID_ITEMS;
                    return;
                }

                change.noop = current == change.entry;
                if (!change.noop)
                    change.cost = SlotPrice(dest);
                return;
            }
        }
    }

    // The source item is a temporary copy of a collected entry. Transmogrify only reads it and sets flags on it, it
    // keeps no pointer to it, so it is deleted when this returns.
    TransmogStrings ApplyEntry(Player* player, ApplyChange const& change)
    {
        std::unique_ptr<Item> source(Item::CreateItem(change.entry, 1, nullptr));
        if (!source)
            return LANG_TRANSMOG_MISSING_SRC_ITEM;

        // Cost and tokens were charged once for the whole APPLY.
        return sT->Transmogrify(player, source.get(), change.slot, true, false);
    }

    void HandleApply(Player* player, std::vector<std::string_view> const& fields)
    {
        std::vector<ApplyChange> changes;
        uint32 firstError = 0;
        std::unordered_set<uint8> seenSlots;

        std::vector<std::string_view> items;
        if (fields.size() >= 2)
            items = Acore::Tokenize(fields[1], ',', false);

        for (std::string_view itemText : items)
        {
            if (changes.size() >= EQUIPMENT_SLOT_END)
                break;

            ApplyChange change;
            std::vector<std::string_view> pair = Acore::Tokenize(itemText, ':', true);
            Optional<uint32> wire = pair.empty() ? Optional<uint32>() : Acore::StringTo<uint32>(pair[0]);
            if (wire && *wire >= 1 && *wire <= EQUIPMENT_SLOT_END)
                change.wireSlot = uint8(*wire);

            if (pair.size() != 2 || !ParseWireSlot(pair[0], change.slot) || !seenSlots.insert(change.slot).second)
                change.code = LANG_TRANSMOG_INVALID_SLOT;
            else if (pair[1] == "H")
                change.kind = ApplyKind::Hide;
            else if (pair[1] == "0")
                change.kind = ApplyKind::Remove;
            else if (Optional<uint32> entry = Acore::StringTo<uint32>(pair[1]); entry && *entry != HIDDEN_ITEM_ID)
            {
                change.kind = ApplyKind::Entry;
                change.entry = *entry;
            }
            else
                change.code = LANG_TRANSMOG_INVALID_SRC_ENTRY;

            if (!change.code)
                ValidateChange(player, change);

            if (change.code && !firstError)
                firstError = change.code;
            changes.push_back(change);
        }

        // Cost and tokens for the whole batch: all of it is affordable, or nothing is applied.
        uint64 totalCost = 0;
        uint32 tokens = 0;
        bool showSetDisclaimer = false;
        for (ApplyChange const& change : changes)
        {
            if (change.code || change.noop)
                continue;

            totalCost += change.cost;
            if (change.kind == ApplyKind::Entry && sT->GetRequireToken())
                tokens += sT->GetTokenAmount();
            if (change.kind == ApplyKind::Entry && change.item->GetTemplate()->ItemSet)
                showSetDisclaimer = true;
        }

        if (!firstError && totalCost > uint64(MAX_MONEY_AMOUNT))
            firstError = LANG_TRANSMOG_NOT_ENOUGH_MONEY;
        if (!firstError && !player->HasEnoughMoney(uint32(totalCost)))
            firstError = LANG_TRANSMOG_NOT_ENOUGH_MONEY;
        if (!firstError && tokens && !player->HasItemCount(sT->GetTokenEntry(), tokens))
            firstError = LANG_TRANSMOG_NOT_ENOUGH_TOKENS;

        if (!firstError)
        {
            if (tokens)
                player->DestroyItemCount(sT->GetTokenEntry(), tokens, true);
            if (totalCost)
                player->ModifyMoney(-int32(totalCost), false);

            for (ApplyChange& change : changes)
            {
                if (!change.noop)
                {
                    switch (change.kind)
                    {
                        case ApplyKind::Remove:
                            // Same as the gossip menu's single-slot "Remove transmogrification".
                            sT->DeleteFakeEntry(player, change.slot, change.item);
                            break;
                        case ApplyKind::Hide:
                            // Transmogrify(player, nullptr, slot, _, true) minus its charge, which was taken above.
                            sT->SetFakeEntry(player, HIDDEN_ITEM_ID, change.slot, change.item);
                            break;
                        case ApplyKind::Entry:
                            change.code = ResultCode(ApplyEntry(player, change));
                            break;
                    }
                }

                // SetFakeEntry/DeleteFakeEntry already moved the slot's choice; this also covers a no-op item change
                // (the item already wore the look), "0" on a slot whose choice is dormant and "0" on an empty slot.
                if (!change.code && IsSlotBoundActive())
                {
                    uint32 const choice = change.kind == ApplyKind::Entry ? change.entry :
                        change.kind == ApplyKind::Hide ? HIDDEN_ITEM_ID : 0;
                    SetSlotChoice(player, change.slot, choice);
                }
            }

            WorldSession* session = player->GetSession();
            if (showSetDisclaimer && sT->ShowSetDisclaimer &&
                !player->GetPlayerSetting("mod-transmog", SETTING_HIDE_SET_DISCLAIMER).value)
                ChatHandler(session).PSendModuleSysMessage("mod-transmog", LANG_TRANSMOG_SET_DISCLAIMER);
        }

        for (ApplyChange const& change : changes)
        {
            if (!change.wireSlot)
                continue;

            // A slot that passed its own checks but was not applied because of another slot (or the total cost)
            // reports the batch's first failure.
            uint32 code = change.code;
            if (firstError && !code)
                code = firstError;
            SendWardrobeMessage(player, Acore::StringFormat("R;{};{}", change.wireSlot, code));
        }

        SendWardrobeMessage(player, BuildCurBody(player));
    }

    void HandleWardrobeMessage(Player* player, std::string_view body)
    {
        std::vector<std::string_view> fields = Acore::Tokenize(body, ';', true);
        if (fields.empty())
            return;

        std::string_view const command = fields[0];
        if (command == "HELLO")
            HandleHello(player);
        else if (command == "LIST")
        {
            if (AcceptRequest(player, WardrobeRequest::List))
                HandleList(player, fields);
        }
        else if (command == "APPLY")
        {
            if (AcceptRequest(player, WardrobeRequest::Apply))
                HandleApply(player, fields);
        }
    }
}

// Called by Transmogrification::AddCollectedAppearance when an entry is new to the account's collection.
void TransmogUI_OnAppearanceCollected(uint32 accountId, uint32 itemId)
{
    WorldSession* session = sWorldSessionMgr->FindSession(accountId);
    Player* player = session ? session->GetPlayer() : nullptr;
    if (!player)
        return;

    {
        StateGuard guard(StateLock);
        auto itr = States.find(player->GetGUID());
        if (itr != States.end())
            itr->second.lists.clear();
    }

    if (!IsWardrobeActive() || !player->IsInWorld())
        return;

    // Rings, trinkets and the like are collected but never offered.
    ItemTemplate const* itemTemplate = sObjectMgr->GetItemTemplate(itemId);
    if (!itemTemplate || sT->CanNeverTransmog(itemTemplate))
        return;

    SendWardrobeMessage(player, Acore::StringFormat("NEW;{}", itemId));
}

// Called by Transmogrification::SetFakeEntry (newEntry = the look) and DeleteFakeEntry (newEntry = 0): every route that
// changes an equipped item's look (gossip, fake vendor, presets, the wardrobe) moves that slot's choice with it.
void TransmogUI_OnFakeEntryChanged(Player* player, Item* item, uint32 newEntry)
{
    if (!player || !item || !IsSlotBoundActive() || !item->IsEquipped() || !IsTransmogSlot(item->GetSlot()))
        return;

    SetSlotChoice(player, item->GetSlot(), newEntry);
}

// Called by `.transmog reload` (HandleReloadTransmogConfig), which does not fire OnAfterConfigLoad.
void TransmogUI_OnConfigReload()
{
    LoadWardrobeConfig(true);
}

// Called by the gossip menu's "Remove all transmogrifications": dormant choices go too.
void TransmogUI_ClearSlotChoices(Player* player)
{
    if (!player || !IsSlotBoundActive())
        return;

    for (uint8 slot : TransmogSlots)
        SetSlotChoice(player, slot, 0);
}

class PS_TransmogWardrobe : public PlayerScript
{
public:
    PS_TransmogWardrobe() : PlayerScript("PS_TransmogWardrobe", {
        PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT,
        PLAYERHOOK_ON_LOGIN,
        PLAYERHOOK_ON_LOGOUT,
        PLAYERHOOK_ON_LEVEL_CHANGED,
        PLAYERHOOK_ON_EQUIP,
        PLAYERHOOK_ON_AFTER_SET_VISIBLE_ITEM_SLOT,
        PLAYERHOOK_ON_DELETE_FROM_DB
    }) { }

    using PlayerScript::OnPlayerCanUseChat;

    // Every whisper to self under the prefix is swallowed, whether the wardrobe is enabled or not.
    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 language, std::string& msg, Player* receiver) override
    {
        if (language != LANG_ADDON || type != CHAT_MSG_WHISPER || receiver != player ||
            msg.compare(0, WardrobePrefix.size() + 1, WardrobePrefix + '\t') != 0)
            return true;

        if (IsWardrobeActive())
            HandleWardrobeMessage(player, std::string_view(msg).substr(WardrobePrefix.size() + 1));
        return false;
    }

    // Runs after PS_Transmogrification::OnPlayerLogin (registered first), so the items' fake entries are loaded.
    void OnPlayerLogin(Player* player) override
    {
        LoadSlotChoices(player);
    }

    void OnPlayerLogout(Player* player) override
    {
        StateGuard guard(StateLock);
        States.erase(player->GetGUID());
    }

    // Required levels change which looks fit.
    void OnPlayerLevelChanged(Player* player, uint8 /*oldLevel*/) override
    {
        StateGuard guard(StateLock);
        auto itr = States.find(player->GetGUID());
        if (itr != States.end())
            itr->second.lists.clear();
    }

    // Any route into an equipment slot: equip from bags, swaps, the equipment manager, AutoEquip, loot/vendor equip.
    void OnPlayerEquip(Player* player, Item* item, uint8 bag, uint8 slot, bool /*update*/) override
    {
        if (!item || bag != INVENTORY_SLOT_BAG_0 || !IsTransmogSlot(slot) || !IsSlotBoundActive())
            return;

        if (player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot) == item)
            TryApplySlotChoice(player, slot, item);
    }

    // Fires on every change of a visible equipment slot, including the item leaving it (item = nullptr) and the
    // character loading its gear.
    void OnPlayerAfterSetVisibleItemSlot(Player* player, uint8 slot, Item* item) override
    {
        if (slot < EQUIPMENT_SLOT_END)
            OnSlotItemChanged(player, slot, item);
    }

    void OnPlayerDeleteFromDB(CharacterDatabaseTransaction trans, uint32 guid) override
    {
        trans->Append("DELETE FROM custom_transmogrification_slots WHERE Owner = {}", guid);
    }
};

class WS_TransmogWardrobe : public WorldScript
{
public:
    WS_TransmogWardrobe() : WorldScript("WS_TransmogWardrobe", {
        WORLDHOOK_ON_AFTER_CONFIG_LOAD,
        WORLDHOOK_ON_STARTUP
    }) { }

    // Same clean-up mod-transmog runs for its presets table.
    void OnStartup() override
    {
        CharacterDatabase.Execute("DELETE FROM `custom_transmogrification_slots` WHERE NOT EXISTS "
            "(SELECT 1 FROM characters WHERE characters.guid = custom_transmogrification_slots.Owner)");
    }

    void OnAfterConfigLoad(bool reload) override
    {
        LoadWardrobeConfig(reload);
    }
};

class transmog_wardrobe_commandscript : public CommandScript
{
public:
    transmog_wardrobe_commandscript() : CommandScript("transmog_wardrobe_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable wardrobeTable =
        {
            { "list", HandleWardrobeListCommand, SEC_GAMEMASTER, Console::No },
            { "cur",  HandleWardrobeCurCommand,  SEC_GAMEMASTER, Console::No }
        };

        static ChatCommandTable transmogTable =
        {
            { "wardrobe", wardrobeTable }
        };

        static ChatCommandTable commandTable =
        {
            { "transmog", transmogTable }
        };

        return commandTable;
    }

    // Prints the bodies LIST would send for the selected player (or self). Sends nothing to the client.
    static bool HandleWardrobeListCommand(ChatHandler* handler, uint8 wireSlot)
    {
        Player* target = handler->getSelectedPlayerOrSelf();
        if (!target)
            return false;

        uint8 slot = 0;
        if (!ParseWireSlot(std::to_string(wireSlot), slot))
        {
            handler->SendErrorMessage("Slot {} is not a transmoggable slot (client INVSLOT_* numbering, 1 = head ... "
                "19 = tabard).", wireSlot);
            return false;
        }

        for (std::string const& body : BuildListResponse(target, slot))
            handler->SendSysMessage(WardrobePrefix + " " + body);
        return true;
    }

    // Prints the CUR body for the selected player (or self).
    static bool HandleWardrobeCurCommand(ChatHandler* handler)
    {
        Player* target = handler->getSelectedPlayerOrSelf();
        if (!target)
            return false;

        handler->SendSysMessage(WardrobePrefix + " " + BuildCurBody(target));
        return true;
    }
};

void AddSC_transmog_ui()
{
    new PS_TransmogWardrobe();
    new WS_TransmogWardrobe();
    new transmog_wardrobe_commandscript();
}
