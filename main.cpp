#include <Windows.h>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>
#include <atomic>
#include <array>
#include <mutex>
#include <system_error>

#include "sdk/inc/main.h"
#include "natives.h"

namespace fs = std::filesystem;

HMODULE g_module = nullptr;

// ============================================================
// Ultimate Outlaw Satchel
// Version 2.15.1
//
// EARLY-BOOTSTRAP 500K SATCHEL PATCH + AMMO -1 + "HOARDING X" + FIRST-RUN WELCOME + RETURNING HARIS STARTUP
//
// v1.76 mapped every stackable AMMO-group base-capacity record in the verified
// 3,801,088-byte ItemDatabase region: 43/43 mapped, 0 unresolved,
// 0 inactive/unavailable, and 0 catalog mismatches.
//
// v1.81 keeps the proven v1.77 capacity/persistence behavior and the proven
// Satchel DataBinding bridge from v1.48. v1.78 proved the final wording and
// quantity source, but its 50 ms polling interval allowed Rockstar's vanilla
// "Carrying X of Y" string to flash briefly when the highlight changed.
//
// v1.81 keeps the no-gap DATABINDING_WRITE_DATA_STRING interception, but the
// presentation rule is now deliberately narrow: only Rockstar's normal
// positive-quantity "Carrying <quantity>..." leaf-item Tip is rewritten to
// "Hoarding <quantity>". Unique-item text, submenu/group headers, documents,
// and every other non-Carrying Tip are left exactly as Rockstar supplied them.
// The per-frame watcher is read-only and exists only to discover the live Tip
// handle/selection; it never manufactures Hoarding text itself. The underlying
// 500000+ capacity remains unchanged.
//
// v2.03 keeps the proven 500000 Satchel/card/watch capacities and final
// "Hoarding <quantity>" Satchel presentation. The 43 mapped ammo base-capacity
// records are set to Rockstar's -1 sentinel, which makes the vanilla weapon
// wheel present the current ammo quantity without the slash/maximum.
// The vanilla top and bottom ammo numbers are left completely untouched.
// No custom weapon-wheel text is drawn; the lower display is simply Rockstar's
// current ammo quantity with the slash/maximum naturally suppressed by -1.
//
// Existing proven behavior retained:
//   - 517 positive records in the real 518-record SATCHEL pointer group -> 500000
//   - the one negative SATCHEL record remains untouched
//   - exactly 144 cigarette-card records -> 500000
//   - all 4 stackable loot-watch records -> 500000
//   - personal/unique quantity-1 watches remain untouched
//
// No F7. No ledger. No inventory hooks. No replacement catalog.
// The Win32-only bootstrap begins at DLL_PROCESS_ATTACH so capacities are
// changed before Rockstar restores/clamps saved player inventory.
// ============================================================

static constexpr std::uint32_t V181_WILD_MINT = 0xE61255CEu;
static constexpr std::uint32_t V181_SLOT_ZERO = 0xD7E2D44Au;
static constexpr std::uint32_t V181_SLOT_SATCHEL = 0x409F50CBu;
static constexpr std::uint32_t V181_SLOT_CAPACITY_A = 0xD5C803CEu;
static constexpr std::uint32_t V181_SLOT_CAPACITY_B = 0x04718245u;
static constexpr std::uint32_t V181_SLOT_WATCH = 0xBAB76133u;

// RDO 1.33 catalog proof for the two special buckets:
//   148 DOCUMENT records have quantity 5 on 0xD7E2D44A; 144 are cigarette cards.
//   4 PROVISION watch records have quantity 5 on 0xBAB76133; all four are stackable loot watches.
static constexpr int V181_EXPECTED_CARD_STYLE_RECORDS = 148;
static constexpr int V181_EXPECTED_CIGARETTE_CARD_RECORDS = 144;
static constexpr int V181_EXPECTED_NONCARD_DOCUMENT_RECORDS = 4;
static constexpr int V181_EXPECTED_STACKABLE_WATCH_RECORDS = 4;
static constexpr std::int64_t V181_PROBE_MAX = 499321;


static constexpr std::int64_t V181_QTY_ZERO = 0;
static constexpr std::int64_t V181_QTY_BASE = 10;
static constexpr std::int64_t V181_QTY_CAPACITY_A = 5;
static constexpr std::int64_t V181_QTY_CAPACITY_B = 84;
static constexpr std::int64_t V181_TARGET_MAX = 500000;
// v2.03 ammo rule: all 43 mapped AMMO base capacities -> -1.
// Satchel/card/watch targets remain 500000.
static constexpr std::int64_t V181_AMMO_TEST_MAX = -1;

static constexpr SIZE_T V181_RECORD_SIZE = 24u;
static constexpr SIZE_T V181_BLOCK_SIZE = V181_RECORD_SIZE * 4u;
static constexpr SIZE_T V181_REGION_SIZE = 3801088u;
static constexpr SIZE_T V181_MINT_OFFSET = 0x2E5C20u;
static constexpr SIZE_T V181_GUARD14_OFFSET = 0x2E4CC0u;
static constexpr SIZE_T V181_GUARD16_OFFSET = 0x2E6298u;

static constexpr int V181_EXPECTED_REAL_SATCHEL_RECORDS = 518;
static constexpr int V181_EXPECTED_POSITIVE_RECORDS = 517;
static constexpr int V181_EXPECTED_NEGATIVE_RECORDS = 1;
static constexpr int V181_EXPECTED_ZERO_RECORDS = 0;
static constexpr int V181_EXPECTED_MINT_ORDINAL = 471;

static constexpr std::uint64_t N_GET_INVENTORY_ID_FROM_PED = 0x13D234A2A3F66E63ULL;
static constexpr std::uint64_t N_COUNT_INVENTORY_ITEMS = 0xE787F05DFC977BDEULL;
static constexpr std::uint64_t N_GET_ITEM_SLOT_MAX_COUNT = 0xE80E50BEE276A54AULL;
// Proven v1.48 Satchel DataBinding bridge.
static constexpr std::uint64_t N_DATABINDING_GET_DATA_CONTAINER_FROM_PATH =
    0x0C827D175F1292F2ULL;
static constexpr std::uint64_t N_DATABINDING_READ_HASH =
    0x81D7183E7A8ECA72ULL;
static constexpr std::uint64_t N_DATABINDING_WRITE_DATA_STRING =
    0xE1BD342F2872AEE9ULL;

struct V181NamedItemHash
{
    std::uint32_t hash;
    const char* name;
};

// These are the four non-cigarette-card DOCUMENT records that share the exact
// quantity-5 / 0xD7E2D44A multiplicity signature in RDO 1.33. Two are already
// raw hash keys in the catalog; DOCUMENT_BOUNTY_POSTER_CHAIN_GANG hashes to
// 0x9A0F33FA. ScriptMain identifies their four live multiplicity addresses by
// temporarily probing one candidate at a time, then restores only those four to 5.
static constexpr std::array<V181NamedItemHash, V181_EXPECTED_NONCARD_DOCUMENT_RECORDS>
V181_NONCARD_DOCUMENTS = {{
    {0x4A0E890Du, "0x4A0E890D"},
    {0x7976694Au, "0x7976694A"},
    {0x9A0F33FAu, "DOCUMENT_BOUNTY_POSTER_CHAIN_GANG"},
    {0xBDC993D8u, "0xBDC993D8"}
}};

static constexpr std::array<V181NamedItemHash, 3> V181_CARD_VERIFY_SAMPLES = {{
    {0x023615C0u, "DOCUMENT_CIG_CARD_ART_4"},
    {0xD7DDF9F7u, "DOCUMENT_CIG_CARD_AML_1"},
    {0x4B63F889u, "DOCUMENT_CIG_CARD_VEH_12"}
}};

static constexpr std::array<V181NamedItemHash, 4> V181_WATCH_VERIFY_ITEMS = {{
    {0x20711C1Eu, "PROVISION_POCKET_WATCH_PLATINUM"},
    {0xC68E33F4u, "PROVISION_POCKET_WATCH_SILVER"},
    {0xC881E25Cu, "PROVISION_POCKET_WATCH_GOLD"},
    {0xEE03DD5Au, "PROVISION_WATCH"}
}};


static constexpr std::uint32_t V181_PERSONAL_WATCH = 0xD177CF30u; // KIT_PLAYER_POCKETWATCH

// v2.12: exact 144 vanilla/CI cigarette-card item identities.
// These are identity hashes only; starting multiplicity values are NOT trusted.
// Any replacement catalog may change the starting capacity. The live record
// found through Rockstar's own ItemDatabase structure is authoritative.
static constexpr std::array<std::uint32_t, 144> V212_CIGARETTE_CARD_HASHES = {
    0x023615C0u, // DOCUMENT_CIG_CARD_ART_4,
    0x0515E5D7u, // DOCUMENT_CIG_CARD_LND_3,
    0x05BA79FFu, // DOCUMENT_CIG_CARD_ACT_11,
    0x08EA4A4Fu, // DOCUMENT_CIG_CARD_GRL_9,
    0x0AD60BB8u, // DOCUMENT_CIG_CARD_GRL_10,
    0x0C328F0Eu, // DOCUMENT_CIG_CARD_SPT_5,
    0x0D66CF1Bu, // DOCUMENT_CIG_CARD_GUN_7,
    0x0DFDDBB7u, // DOCUMENT_CIG_CARD_SPT_11,
    0x0F7DB04Fu, // DOCUMENT_CIG_CARD_ART_5,
    0x10C70350u, // DOCUMENT_CIG_CARD_VEH_10,
    0x12000475u, // DOCUMENT_CIG_CARD_ACT_9,
    0x122FC698u, // DOCUMENT_CIG_CARD_PAM_2,
    0x127B55F8u, // DOCUMENT_CIG_CARD_PLT_12,
    0x16731B6Cu, // DOCUMENT_CIG_CARD_ACT_12,
    0x16A85371u, // DOCUMENT_CIG_CARD_ART_12,
    0x16F86D2Bu, // DOCUMENT_CIG_CARD_VEH_6,
    0x180D2626u, // DOCUMENT_CIG_CARD_GRL_11,
    0x1A752B93u, // DOCUMENT_CIG_CARD_SPT_6,
    0x1ADD6A08u, // DOCUMENT_CIG_CARD_GUN_6,
    0x1FB24793u, // DOCUMENT_CIG_CARD_GUN_12,
    0x1FF66225u, // DOCUMENT_CIG_CARD_PAM_3,
    0x2104DD33u, // DOCUMENT_CIG_CARD_INV_5,
    0x22B1976Cu, // DOCUMENT_CIG_CARD_AML_12,
    0x23366C8Du, // DOCUMENT_CIG_CARD_ART_11,
    0x23CA7896u, // DOCUMENT_CIG_CARD_PLT_11,
    0x24B9DAC7u, // DOCUMENT_CIG_CARD_ART_2,
    0x254709C8u, // DOCUMENT_CIG_CARD_VEH_7,
    0x260E965Bu, // DOCUMENT_CIG_CARD_AML_9,
    0x264BC2A3u, // DOCUMENT_CIG_CARD_GRL_12,
    0x2AE69F9Fu, // DOCUMENT_CIG_CARD_LND_12,
    0x2B320EE2u, // DOCUMENT_CIG_CARD_GRL_4,
    0x30617BECu, // DOCUMENT_CIG_CARD_INV_6,
    0x30A4B1D8u, // DOCUMENT_CIG_CARD_PLT_1,
    0x3177BE9Au, // DOCUMENT_CIG_CARD_LND_1,
    0x32732420u, // DOCUMENT_CIG_CARD_VEH_4,
    0x34007954u, // DOCUMENT_CIG_CARD_ART_3,
    0x381E4BE7u, // DOCUMENT_CIG_CARD_LND_6,
    0x38EFAA5Du, // DOCUMENT_CIG_CARD_GRL_3,
    0x3B5187F6u, // DOCUMENT_CIG_CARD_ART_8,
    0x3C902D6Du, // DOCUMENT_CIG_CARD_GUN_1,
    0x3CB77017u, // DOCUMENT_CIG_CARD_SPT_7,
    0x3F085FD2u, // DOCUMENT_CIG_CARD_VEH_11,
    0x40CEC0D7u, // DOCUMENT_CIG_CARD_VEH_5,
    0x425F554Du, // DOCUMENT_CIG_CARD_PLT_2,
    0x44872B4Eu, // DOCUMENT_CIG_CARD_PAM_1,
    0x46946D9Du, // DOCUMENT_CIG_CARD_ACT_4,
    0x48932279u, // DOCUMENT_CIG_CARD_ART_9,
    0x4A6391C6u, // DOCUMENT_CIG_CARD_INV_11,
    0x4B313FFDu, // DOCUMENT_CIG_CARD_HOR_6,
    0x4B63F889u, // DOCUMENT_CIG_CARD_VEH_12,
    0x4BFCD077u, // DOCUMENT_CIG_CARD_GRL_6,
    0x53561D58u, // DOCUMENT_CIG_CARD_SPT_9,
    0x5380509Bu, // DOCUMENT_CIG_CARD_HOR_7,
    0x543FFA88u, // DOCUMENT_CIG_CARD_AML_11,
    0x54AE09D8u, // DOCUMENT_CIG_CARD_ACT_5,
    0x54C45DD5u, // DOCUMENT_CIG_CARD_GUN_3,
    0x57A4E7C3u, // DOCUMENT_CIG_CARD_GRL_8,
    0x58ADCC84u, // DOCUMENT_CIG_CARD_INV_8,
    0x59B355A6u, // DOCUMENT_CIG_CARD_PAM_6,
    0x5D17846Cu, // DOCUMENT_CIG_CARD_AML_5,
    0x5DCB4CE9u, // DOCUMENT_CIG_CARD_ART_6,
    0x6035FFA5u, // DOCUMENT_CIG_CARD_VEH_2,
    0x6273FD65u, // DOCUMENT_CIG_CARD_GRL_5,
    0x62B8F9BEu, // DOCUMENT_CIG_CARD_GUN_2,
    0x66A5F6E6u, // DOCUMENT_CIG_CARD_HOR_8,
    0x6745F0CBu, // DOCUMENT_CIG_CARD_PAM_7,
    0x692E32D8u, // DOCUMENT_CIG_CARD_ACT_6,
    0x69F86F19u, // DOCUMENT_CIG_CARD_INV_9,
    0x6B24A086u, // DOCUMENT_CIG_CARD_AML_6,
    0x6D07EB62u, // DOCUMENT_CIG_CARD_ART_7,
    0x6E629BFEu, // DOCUMENT_CIG_CARD_VEH_3,
    0x70252A1Bu, // DOCUMENT_CIG_CARD_LND_10,
    0x70448A2Fu, // DOCUMENT_CIG_CARD_HOR_2,
    0x712FDF56u, // DOCUMENT_CIG_CARD_INV_12,
    0x790CC2A3u, // DOCUMENT_CIG_CARD_PLT_5,
    0x7BEDA181u, // DOCUMENT_CIG_CARD_HOR_3,
    0x7C26421Du, // DOCUMENT_CIG_CARD_LND_11,
    0x7CA17641u, // DOCUMENT_CIG_CARD_INV_10,
    0x7E271E8Du, // DOCUMENT_CIG_CARD_PAM_4,
    0x7F005E7Cu, // DOCUMENT_CIG_CARD_ACT_7,
    0x8127DDF9u, // DOCUMENT_CIG_CARD_LND_2,
    0x817ACD32u, // DOCUMENT_CIG_CARD_AML_7,
    0x84E64249u, // DOCUMENT_CIG_CARD_GRL_2,
    0x8B185569u, // DOCUMENT_CIG_CARD_VEH_1,
    0x8B52672Eu, // DOCUMENT_CIG_CARD_PLT_6,
    0x8B70B920u, // DOCUMENT_CIG_CARD_PAM_5,
    0x8C6733FAu, // DOCUMENT_CIG_CARD_INV_1,
    0x8C9F0FE5u, // DOCUMENT_CIG_CARD_SPT_4,
    0x8CC74334u, // DOCUMENT_CIG_CARD_HOR_4,
    0x8E0FE65Cu, // DOCUMENT_CIG_CARD_AML_8,
    0x8F4840CBu, // DOCUMENT_CIG_CARD_PAM_9,
    0x907EB97Au, // DOCUMENT_CIG_CARD_PAM_10,
    0x97968AD6u, // DOCUMENT_CIG_CARD_LND_7,
    0x991E634Cu, // DOCUMENT_CIG_CARD_PLT_10,
    0x9A51852Cu, // DOCUMENT_CIG_CARD_PLT_3,
    0x9BAB6FD3u, // DOCUMENT_CIG_CARD_GRL_1,
    0x9E9CE6DFu, // DOCUMENT_CIG_CARD_HOR_5,
    0xA01720A9u, // DOCUMENT_CIG_CARD_ACT_1,
    0xA467995Cu, // DOCUMENT_CIG_CARD_PLT_9,
    0xA4CEEFC4u, // DOCUMENT_CIG_CARD_ART_10,
    0xA4ED9417u, // DOCUMENT_CIG_CARD_AML_4,
    0xA6917E7Fu, // DOCUMENT_CIG_CARD_HOR_12,
    0xA9028650u, // DOCUMENT_CIG_CARD_GUN_9,
    0xAD68B67Au, // DOCUMENT_CIG_CARD_LND_8,
    0xB35630E8u, // DOCUMENT_CIG_CARD_AML_3,
    0xB3E4B852u, // DOCUMENT_CIG_CARD_PLT_4,
    0xB489843Eu, // DOCUMENT_CIG_CARD_INV_3,
    0xB6684D4Bu, // DOCUMENT_CIG_CARD_ACT_2,
    0xB9E92532u, // DOCUMENT_CIG_CARD_HOR_10,
    0xBC71AD2Eu, // DOCUMENT_CIG_CARD_GUN_8,
    0xBFCB5023u, // DOCUMENT_CIG_CARD_PLT_7,
    0xC0F2241Eu, // DOCUMENT_CIG_CARD_PAM_8,
    0xC3AD2285u, // DOCUMENT_CIG_CARD_INV_4,
    0xC3E29916u, // DOCUMENT_CIG_CARD_ART_1,
    0xC4ADE9D6u, // DOCUMENT_CIG_CARD_ACT_3,
    0xC828C1B1u, // DOCUMENT_CIG_CARD_HOR_11,
    0xC9985D6Cu, // DOCUMENT_CIG_CARD_AML_2,
    0xD210F4AEu, // DOCUMENT_CIG_CARD_PLT_8,
    0xD2BA0120u, // DOCUMENT_CIG_CARD_LND_4,
    0xD3649D73u, // DOCUMENT_CIG_CARD_SPT_8,
    0xD7DDF9F7u, // DOCUMENT_CIG_CARD_AML_1,
    0xDB6E7699u, // DOCUMENT_CIG_CARD_SPT_10,
    0xDF3D56DEu, // DOCUMENT_CIG_CARD_PAM_11,
    0xE17D39A4u, // DOCUMENT_CIG_CARD_SPT_1,
    0xE27A8430u, // DOCUMENT_CIG_CARD_VEH_8,
    0xE54C8314u, // DOCUMENT_CIG_CARD_GRL_7,
    0xE5A274E9u, // DOCUMENT_CIG_CARD_HOR_1,
    0xE747E9BAu, // DOCUMENT_CIG_CARD_INV_2,
    0xE7BC4622u, // DOCUMENT_CIG_CARD_SPT_2,
    0xE8802CACu, // DOCUMENT_CIG_CARD_LND_5,
    0xE8A75926u, // DOCUMENT_CIG_CARD_GUN_11,
    0xEC76F151u, // DOCUMENT_CIG_CARD_PAM_12,
    0xF04814DEu, // DOCUMENT_CIG_CARD_GUN_5,
    0xF0AA208Fu, // DOCUMENT_CIG_CARD_VEH_9,
    0xF0C90B2Eu, // DOCUMENT_CIG_CARD_HOR_9,
    0xF1FC5283u, // DOCUMENT_CIG_CARD_ACT_10,
    0xF5FBE2A1u, // DOCUMENT_CIG_CARD_SPT_3,
    0xF83BB033u, // DOCUMENT_CIG_CARD_SPT_12,
    0xFA56FC85u, // DOCUMENT_CIG_CARD_GUN_10,
    0xFD27D5FFu, // DOCUMENT_CIG_CARD_LND_9,
    0xFD2BCC61u, // DOCUMENT_CIG_CARD_AML_10,
    0xFD48DB07u, // DOCUMENT_CIG_CARD_ACT_8,
    0xFD9F1668u, // DOCUMENT_CIG_CARD_INV_7,
    0xFF91B371u, // DOCUMENT_CIG_CARD_GUN_4
};

struct V181AmmoMapItem
{
    const char* name;
    std::uint32_t hash;
    int catalogBase;
    SIZE_T offset;
};

// v1.76 mapped all 43 stackable AMMO-group base-capacity records.
// These offsets are relative to the same verified 3,801,088-byte ItemDatabase
// region used by the early satchel patch. Each mapped record used the same
// validated multiplicity pointer as the proven satchel records.
static constexpr std::array<V181AmmoMapItem, 43> V181_AMMO_MAP_ITEMS = {{
    { "AMMO_REVOLVER_EXPRESS_EXPLOSIVE", 0x04A8EFBBu, 10, 0x002BD810u },
    { "AMMO_ARROW_POISON", 0x07865A92u, 8, 0x002BE0C8u },
    { "AMMO_RIFLE_SPLIT_POINT", 0x0BEFA5B2u, 50, 0x002BEEF0u },
    { "AMMO_RIFLE", 0x0D05319Fu, 100, 0x002BF250u },
    { "AMMO_REPEATER_HIGH_VELOCITY", 0x0DCBE210u, 200, 0x002BF4C0u },
    { "AMMO_PISTOL_SPLIT_POINT", 0x0E163B80u, 50, 0x002BF598u },
    { "AMMO_ARROW_FIRE", 0x11B25B49u, 8, 0x002BFD48u },
    { "AMMO_SHOTGUN_SLUG", 0x12C60041u, 60, 0x002C0048u },
    { "AMMO_DYNAMITE", 0x1C9D6E9Du, 8, 0x002C1F98u },
    { "AMMO_SHOTGUN_SLUG_EXPLOSIVE", 0x2314B32Au, 10, 0x002C3468u },
    { "AMMO_PISTOL_EXPRESS", 0x31E2AD5Bu, 100, 0x002C5A30u },
    { "AMMO_DYNAMITE_VOLATILE", 0x321BA159u, 8, 0x002C5A90u },
    { "AMMO_ARROW", 0x38E6F55Fu, 40, 0x002C6BA0u },
    { "AMMO_REPEATER_SPLIT_POINT", 0x44750C88u, 100, 0x002C87A8u },
    { "AMMO_PISTOL_EXPRESS_EXPLOSIVE", 0x46A648C2u, 10, 0x002C8CB8u },
    { "AMMO_THROWING_KNIVES_IMPROVED", 0x48DC05F6u, 8, 0x002C93C0u },
    { "AMMO_REVOLVER_EXPRESS", 0x4970588Du, 200, 0x002C9468u },
    { "AMMO_TOMAHAWK", 0x49A985D7u, 3, 0x002C9510u },
    { "AMMO_REVOLVER_SPLIT_POINT", 0x4A25B008u, 100, 0x002C9708u },
    { "AMMO_MOLOTOV", 0x5633F9D5u, 8, 0x002CB628u },
    { "AMMO_RIFLE_EXPRESS", 0x62A11A4Bu, 100, 0x002CDAA0u },
    { "AMMO_MOONSHINEJUG", 0x631C84FCu, 100, 0x002CDB90u },
    { "AMMO_REVOLVER", 0x64356159u, 200, 0x002CDF80u },
    { "AMMO_RIFLE_EXPRESS_EXPLOSIVE", 0x6D926443u, 10, 0x002CF918u },
    { "AMMO_RIFLE_HIGH_VELOCITY", 0x6ECB67F9u, 100, 0x002CFF00u },
    { "AMMO_PISTOL", 0x743D4F54u, 100, 0x002D0DE8u },
    { "AMMO_THROWING_KNIVES_POISON", 0x7BA5E56Eu, 8, 0x002D23A8u },
    { "AMMO_22", 0x7DF4D025u, 100, 0x002D2AC8u },
    { "AMMO_REVOLVER_HIGH_VELOCITY", 0x83C5E860u, 200, 0x002D3788u },
    { "AMMO_MOLOTOV_VOLATILE", 0x886C55D7u, 8, 0x002D46D0u },
    { "AMMO_SHOTGUN", 0x90083D3Bu, 60, 0x002D5E88u },
    { "AMMO_ARROW_IMPROVED", 0x9238061Fu, 40, 0x002D6428u },
    { "AMMO_REPEATER_EXPRESS_EXPLOSIVE", 0x9C8B6796u, 10, 0x002D8570u },
    { "AMMO_THROWING_KNIVES", 0x9E4AD291u, 8, 0x002D8B88u },
    { "AMMO_TOMAHAWK_HOMING", 0xABD7C401u, 3, 0x002DB348u },
    { "AMMO_PISTOL_HIGH_VELOCITY", 0xABD96830u, 100, 0x002DB360u },
    { "AMMO_ARROW_SMALL_GAME", 0xAE6E2B0Eu, 40, 0x002DBB58u },
    { "AMMO_REPEATER", 0xB0B80B9Au, 200, 0x002DC158u },
    { "AMMO_CANNON", 0xB6976AA1u, 100, 0x002DD0E8u },
    { "AMMO_SHOTGUN_BUCKSHOT_INCENDIARY", 0xBFCB2621u, 14, 0x002DE750u },
    { "AMMO_ARROW_DYNAMITE", 0xC1F57A79u, 8, 0x002DF080u },
    { "AMMO_TOMAHAWK_IMPROVED", 0xCE489834u, 3, 0x002E17B0u },
    { "AMMO_REPEATER_EXPRESS", 0xDD871DC8u, 200, 0x002E43C0u },
}};



struct V181MintBlock
{
    std::uintptr_t block = 0;
    std::uintptr_t quantityBase = 0;
    std::uintptr_t quantity5 = 0;
    std::uintptr_t quantity84 = 0;
    std::uint64_t sharedPointer = 0;
};

struct V181SatchelRecord
{
    std::uintptr_t quantityAddress = 0;
    std::int64_t quantity = 0;
};

struct V181PatchStats
{
    int total = 0;
    int positive = 0;
    int negative = 0;
    int zero = 0;
    int mintOrdinal = 0;
    int writesApplied = 0;
    int alreadyAtTarget = 0;
    int aboveTargetPreserved = 0;
    std::uintptr_t negativeAddress = 0;
    std::int64_t negativeValue = 0;
};

struct V181SpecialPatchStats
{
    int cardStyleRecords = 0;
    int cardStyleWrites = 0;
    int watchRecords = 0;
    int watchWrites = 0;
};

enum class V181BootstrapState : int
{
    NotStarted = 0,
    Running = 1,
    Patched = 2,
    Stopped = 3
};

static std::atomic<int> g_v181State{static_cast<int>(V181BootstrapState::NotStarted)};
static std::atomic<bool> g_v181Stop{false};
static std::atomic<bool> g_v181ForceRescan{false};
static HANDLE g_v181Thread = nullptr;

static ULONGLONG g_v181DllAttachTick = 0;
static std::atomic<ULONGLONG> g_v181WorkerTick{0};
static std::atomic<ULONGLONG> g_v181PoolSeenTick{0};
static std::atomic<ULONGLONG> g_v181PatchTick{0};
static std::atomic<unsigned int> g_v181Attempts{0};
static std::atomic<unsigned int> g_v181ExactSizeVisits{0};
static std::atomic<unsigned int> g_v181ReapplyCount{0};

static std::atomic<std::uintptr_t> g_v181RegionBase{0};
static V181MintBlock g_v181Mint{};

// Written by the bootstrap worker. ScriptMain only uses these as diagnostics.
static std::atomic<int> g_v181StatTotal{0};
static std::atomic<int> g_v181StatPositive{0};
static std::atomic<int> g_v181StatNegative{0};
static std::atomic<int> g_v181StatZero{0};
static std::atomic<int> g_v181StatMintOrdinal{0};
static std::atomic<int> g_v181StatWrites{0};
static std::atomic<int> g_v181StatAlready{0};
static std::atomic<int> g_v181StatAbove{0};


static std::atomic<int> g_v181SpecialCardStyleRecords{0};
static std::atomic<int> g_v181SpecialCardStyleWrites{0};
static std::atomic<int> g_v181SpecialWatchRecords{0};
static std::atomic<int> g_v181SpecialWatchWrites{0};
static std::atomic<int> g_v181SpecialGeneration{0};

static std::atomic<int> g_v181AmmoRecords{0};
static std::atomic<int> g_v181AmmoWrites{0};
static std::atomic<int> g_v181AmmoAlreadyAtTarget{0};
static std::atomic<int> g_v181AmmoAboveTargetPreserved{0};
static std::atomic<int> g_v181AmmoValidationFailures{0};

static std::mutex g_v181SpecialMutex;
static std::vector<std::uintptr_t> g_v181CardStyleAddresses;
static std::vector<std::uintptr_t> g_v181WatchAddresses;

static ULONGLONG g_v181ScriptMainTick = 0;
static ULONGLONG g_v181MessageUntil = 0;
static std::string g_v181Message;
static ULONGLONG g_v181NextWatchTick = 0;
static int g_v181LastMintCount = -999999;
static int g_v181LastMintMax = -999999;
static bool g_v181PersistenceLogged = false;
static bool g_v181GeneralSuccessLogged = false;
static bool g_v181RescanRequestedLogged = false;
static int g_v181PruneAttemptedGeneration = 0;
static int g_v181PrunedGeneration = 0;
static bool g_v181SpecialPruneSucceeded = false;
static int g_v181LastCardSampleMax = -999999;
static int g_v181LastWatchSampleMax = -999999;

// v1.81 satchel presentation state.
static constexpr const char* V181_SATCHEL_PATH = "Satchel";
static constexpr const char* V181_SELECTED_PATH = "Satchel.Selected";
static constexpr const char* V181_SELECTED_NAME_PATH = "Satchel.Selected.Name";
static constexpr const char* V181_SELECTED_TIP_PATH = "Satchel.Selected.Tip";
static bool g_v181SatchelUiOpen = false;
static bool g_v181SatchelUiUnavailableLogged = false;
static std::uint32_t g_v181LastUiItem = 0;
static int g_v181LastUiCount = -999999;
static ULONGLONG g_v181LastUiTick = 0;

// v1.81 immediate Tip-write interception state. The current Tip handle is
// discovered by the normal Satchel DataBinding watcher and cached here. Once
// known, Rockstar's own write is rewritten before the renderer can ever see
// the vanilla "Carrying X of Y" text.
using V181GetCommandFromHashFn = void* (__cdecl*)(std::uint64_t);

struct V181NativeCallContextPrefix
{
    void* returnValue;
    std::uint32_t argumentCount;
    std::uint32_t padding0;
    std::uint64_t* arguments;
};

static void* g_v181TipWriteCommand = nullptr;
static BYTE g_v181TipWriteOriginalByte = 0;
static BYTE g_v181TipWriteOriginalBytes[16]{};
static std::size_t g_v181TipWriteFirstInstructionLength = 0;
static void* g_v181TipWriteTrampoline = nullptr;
static DWORD g_v181TipWriteOldProtect = 0;
static PVOID g_v181TipWriteVeh = nullptr;
static std::atomic<int> g_v181CachedTipHandle{0};
static std::atomic<bool> g_v181TipWriteHookInstalled{false};
static std::atomic<bool> g_v181SatchelOpenForHook{false};
static LONG64 g_v181TipWriteIntercepts = 0;
static thread_local char g_v181TipReplacement[128]{};


static bool ReadI64Address(std::uintptr_t address, std::int64_t& value)
{
    SIZE_T bytesRead = 0;
    value = 0;
    return ReadProcessMemory(
        GetCurrentProcess(),
        reinterpret_cast<LPCVOID>(address),
        &value,
        sizeof(value),
        &bytesRead
    ) && bytesRead == sizeof(value);
}

static bool WriteI64Address(std::uintptr_t address, std::int64_t value)
{
    SIZE_T bytesWritten = 0;
    return WriteProcessMemory(
        GetCurrentProcess(),
        reinterpret_cast<LPVOID>(address),
        &value,
        sizeof(value),
        &bytesWritten
    ) && bytesWritten == sizeof(value);
}

static fs::path GetModDataDirectory()
{
    wchar_t modulePath[MAX_PATH]{};
    GetModuleFileNameW(g_module, modulePath, MAX_PATH);
    return fs::path(modulePath).parent_path() / L"UltimateOutlawSatchel";
}

static fs::path GetLogPath()
{
    return GetModDataDirectory() / L"UltimateOutlawSatchel.log";
}

static void MigrateLegacySatchelDataNamesIfNeeded()
{
    // v2.15.24 rename-only compatibility:
    // preserve the existing Welcome-seen marker when moving from the old
    // UltimateTravelersSatchel data folder to UltimateOutlawSatchel.
    // The old folder is deliberately left untouched as a backup.
    try
    {
        wchar_t modulePath[MAX_PATH]{};
        GetModuleFileNameW(g_module, modulePath, MAX_PATH);

        const fs::path moduleDirectory =
            fs::path(modulePath).parent_path();

        const fs::path legacyDirectory =
            moduleDirectory / L"UltimateTravelersSatchel";

        const fs::path newDirectory =
            moduleDirectory / L"UltimateOutlawSatchel";

        const fs::path legacyWelcomeDat =
            legacyDirectory / L"UltimateOutlawSatchel.dat";

        const fs::path newWelcomeDat =
            newDirectory / L"UltimateOutlawSatchel.dat";

        std::error_code ec;

        if (!fs::exists(newWelcomeDat, ec) &&
            fs::exists(legacyWelcomeDat, ec))
        {
            fs::create_directories(newDirectory, ec);
            ec.clear();

            fs::copy_file(
                legacyWelcomeDat,
                newWelcomeDat,
                fs::copy_options::none,
                ec);
        }
    }
    catch (...)
    {
        // Migration failure is non-fatal. The normal Welcome logic will
        // simply behave as a first run if the new marker is unavailable.
    }
}

static void ResetLog()
{
    fs::create_directories(GetModDataDirectory());
    std::ofstream log(GetLogPath(), std::ios::trunc);
}

static void WriteLog(const std::string& s)
{
    fs::create_directories(GetModDataDirectory());
    std::ofstream log(GetLogPath(), std::ios::app);
    log << s << '\n';
}

static std::string Hex(std::uint64_t value, int width = 8)
{
    std::ostringstream out;
    out << "0x" << std::uppercase << std::hex
        << std::setfill('0') << std::setw(width) << value;
    return out.str();
}

static void SetScreenMessage(const std::string& message, ULONGLONG durationMs)
{
    g_v181Message = message;
    g_v181MessageUntil = GetTickCount64() + durationMs;
}

static void DrawScreenMessage()
{
    if (g_v181Message.empty() || GetTickCount64() > g_v181MessageUntil)
        return;

    const char* text = invoke<const char*>(
        0xFA925AC00EB830B9ULL,
        10,
        "LITERAL_STRING",
        g_v181Message.c_str()
    );

    if (!text)
        return;

    invoke<Void>(0xA1253A3C870B6843ULL, 0.38f, 0.38f);
    invoke<Void>(0x16FA5CE47F184F1EULL, 255, 255, 255, 255);
    invoke<Void>(0xBE5261939FBECB8CULL, true);
    invoke<Void>(0x16794E044C9EFB58ULL, text, 0.5f, 0.87f);
    invoke<Void>(0xBE5261939FBECB8CULL, false);
}

static bool IsTargetProtection(DWORD protect)
{
    if ((protect & PAGE_GUARD) != 0 || (protect & PAGE_NOACCESS) != 0)
        return false;

    const DWORD basic = protect & 0xFFu;
    return basic == PAGE_READWRITE || basic == PAGE_WRITECOPY;
}

static std::uint64_t RawU64(const std::uint8_t* b, SIZE_T offset)
{
    std::uint64_t value = 0;
    std::memcpy(&value, b + offset, sizeof(value));
    return value;
}

static std::int64_t RawI64(const std::uint8_t* b, SIZE_T offset)
{
    std::int64_t value = 0;
    std::memcpy(&value, b + offset, sizeof(value));
    return value;
}

// Verifies one of the known 4-record herb blocks.
// After the broad pass, the SATCHEL base quantity can be either 10 or 500000.
static bool BootstrapBlockMatches(
    std::uintptr_t blockAddress,
    std::uint64_t* sharedPointerOut)
{
    std::uint8_t b[V181_BLOCK_SIZE]{};
    SIZE_T bytesRead = 0;

    if (!ReadProcessMemory(
            GetCurrentProcess(),
            reinterpret_cast<LPCVOID>(blockAddress),
            b,
            sizeof(b),
            &bytesRead) ||
        bytesRead != sizeof(b))
    {
        return false;
    }

    const std::int64_t q0 = RawI64(b, V181_RECORD_SIZE * 0u + 0u);
    const std::int64_t q1 = RawI64(b, V181_RECORD_SIZE * 1u + 0u);
    const std::int64_t q2 = RawI64(b, V181_RECORD_SIZE * 2u + 0u);
    const std::int64_t q3 = RawI64(b, V181_RECORD_SIZE * 3u + 0u);

    const std::uint64_t s0 = RawU64(b, V181_RECORD_SIZE * 0u + 8u);
    const std::uint64_t s1 = RawU64(b, V181_RECORD_SIZE * 1u + 8u);
    const std::uint64_t s2 = RawU64(b, V181_RECORD_SIZE * 2u + 8u);
    const std::uint64_t s3 = RawU64(b, V181_RECORD_SIZE * 3u + 8u);

    const std::uint64_t p0 = RawU64(b, V181_RECORD_SIZE * 0u + 16u);
    const std::uint64_t p1 = RawU64(b, V181_RECORD_SIZE * 1u + 16u);
    const std::uint64_t p2 = RawU64(b, V181_RECORD_SIZE * 2u + 16u);
    const std::uint64_t p3 = RawU64(b, V181_RECORD_SIZE * 3u + 16u);

    if (q0 != V181_QTY_ZERO ||
        (q1 != V181_QTY_BASE && q1 != V181_TARGET_MAX) ||
        q2 != V181_QTY_CAPACITY_A ||
        q3 != V181_QTY_CAPACITY_B)
    {
        return false;
    }

    if (s0 != static_cast<std::uint64_t>(V181_SLOT_ZERO) ||
        s1 != static_cast<std::uint64_t>(V181_SLOT_SATCHEL) ||
        s2 != static_cast<std::uint64_t>(V181_SLOT_CAPACITY_A) ||
        s3 != static_cast<std::uint64_t>(V181_SLOT_CAPACITY_B))
    {
        return false;
    }

    if (p0 == 0 || !(p0 == p1 && p1 == p2 && p2 == p3))
        return false;

    if (sharedPointerOut)
        *sharedPointerOut = p0;

    return true;
}

static bool CollectRealSatchelRecords(
    std::uintptr_t regionBase,
    std::uint64_t realSharedPointer,
    const V181MintBlock& mint,
    std::vector<V181SatchelRecord>& out,
    V181PatchStats& stats)
{
    out.clear();
    stats = V181PatchStats{};

    std::vector<std::uint8_t> buffer(V181_REGION_SIZE);
    SIZE_T bytesRead = 0;

    if (!ReadProcessMemory(
            GetCurrentProcess(),
            reinterpret_cast<LPCVOID>(regionBase),
            buffer.data(),
            buffer.size(),
            &bytesRead) ||
        bytesRead < V181_RECORD_SIZE)
    {
        return false;
    }

    if (bytesRead < buffer.size())
        buffer.resize(bytesRead);

    int ordinal = 0;

    // v1.70 proved that the real records are the SATCHEL-slot records that
    // share Wild Mint's record pointer. The 29 bogus hits use another pointer.
    for (SIZE_T pos = 0; pos + V181_RECORD_SIZE <= buffer.size(); pos += 8u)
    {
        const std::uint64_t slotRaw = RawU64(buffer.data(), pos + 8u);
        const std::uint64_t pointer = RawU64(buffer.data(), pos + 16u);

        if (slotRaw != static_cast<std::uint64_t>(V181_SLOT_SATCHEL))
            continue;
        if (pointer != realSharedPointer)
            continue;

        ++ordinal;

        V181SatchelRecord record{};
        record.quantityAddress = regionBase + pos;
        record.quantity = RawI64(buffer.data(), pos);
        out.push_back(record);

        if (record.quantity > 0)
            ++stats.positive;
        else if (record.quantity == 0)
            ++stats.zero;
        else
        {
            ++stats.negative;
            stats.negativeAddress = record.quantityAddress;
            stats.negativeValue = record.quantity;
        }

        if (record.quantityAddress == mint.quantityBase)
            stats.mintOrdinal = ordinal;
    }

    stats.total = ordinal;
    return true;
}

static bool ValidateRecordShape(const V181PatchStats& stats)
{
    return
        stats.total == V181_EXPECTED_REAL_SATCHEL_RECORDS &&
        stats.positive == V181_EXPECTED_POSITIVE_RECORDS &&
        stats.negative == V181_EXPECTED_NEGATIVE_RECORDS &&
        stats.zero == V181_EXPECTED_ZERO_RECORDS &&
        stats.mintOrdinal == V181_EXPECTED_MINT_ORDINAL;
}

static bool PatchRealSatchelGroup(
    std::uintptr_t regionBase,
    std::uint64_t realSharedPointer,
    const V181MintBlock& mint,
    V181PatchStats& statsOut)
{
    std::vector<V181SatchelRecord> records;
    V181PatchStats before{};

    if (!CollectRealSatchelRecords(regionBase, realSharedPointer, mint, records, before))
        return false;

    if (!ValidateRecordShape(before))
    {
        statsOut = before;
        return false;
    }

    struct Change
    {
        std::uintptr_t address = 0;
        std::int64_t oldValue = 0;
    };

    std::vector<Change> changes;
    changes.reserve(V181_EXPECTED_POSITIVE_RECORDS);

    for (const auto& record : records)
    {
        if (record.quantity <= 0)
            continue;

        if (record.quantity == V181_TARGET_MAX)
        {
            ++before.alreadyAtTarget;
            continue;
        }

        if (record.quantity > V181_TARGET_MAX)
        {
            // Do not reduce a capacity supplied by another mod.
            ++before.aboveTargetPreserved;
            continue;
        }

        changes.push_back({record.quantityAddress, record.quantity});
    }

    SIZE_T completed = 0;
    for (; completed < changes.size(); ++completed)
    {
        if (!WriteI64Address(changes[completed].address, V181_TARGET_MAX))
            break;

        std::int64_t readBack = 0;
        if (!ReadI64Address(changes[completed].address, readBack) ||
            readBack != V181_TARGET_MAX)
        {
            break;
        }
    }

    if (completed != changes.size())
    {
        // Best-effort rollback of every write that may have completed.
        const SIZE_T rollbackCount =
            (completed < changes.size()) ? completed + 1u : completed;

        for (SIZE_T i = 0; i < rollbackCount && i < changes.size(); ++i)
            WriteI64Address(changes[i].address, changes[i].oldValue);

        statsOut = before;
        return false;
    }

    before.writesApplied = static_cast<int>(changes.size());

    // Re-enumerate and prove the structure stayed intact.
    std::vector<V181SatchelRecord> verifyRecords;
    V181PatchStats verify{};

    if (!CollectRealSatchelRecords(
            regionBase,
            realSharedPointer,
            mint,
            verifyRecords,
            verify) ||
        !ValidateRecordShape(verify))
    {
        for (const auto& change : changes)
            WriteI64Address(change.address, change.oldValue);

        statsOut = before;
        return false;
    }

    // Every positive legitimate SATCHEL capacity must now be >= 500000.
    for (const auto& record : verifyRecords)
    {
        if (record.quantity > 0 && record.quantity < V181_TARGET_MAX)
        {
            for (const auto& change : changes)
                WriteI64Address(change.address, change.oldValue);

            statsOut = before;
            return false;
        }
    }

    // The single special negative record must be exactly untouched.
    if (verify.negativeAddress != before.negativeAddress ||
        verify.negativeValue != before.negativeValue)
    {
        for (const auto& change : changes)
            WriteI64Address(change.address, change.oldValue);

        statsOut = before;
        return false;
    }

    statsOut = before;
    return true;
}

static void StoreStats(const V181PatchStats& stats)
{
    g_v181StatTotal.store(stats.total, std::memory_order_relaxed);
    g_v181StatPositive.store(stats.positive, std::memory_order_relaxed);
    g_v181StatNegative.store(stats.negative, std::memory_order_relaxed);
    g_v181StatZero.store(stats.zero, std::memory_order_relaxed);
    g_v181StatMintOrdinal.store(stats.mintOrdinal, std::memory_order_relaxed);
    g_v181StatWrites.store(stats.writesApplied, std::memory_order_relaxed);
    g_v181StatAlready.store(stats.alreadyAtTarget, std::memory_order_relaxed);
    g_v181StatAbove.store(stats.aboveTargetPreserved, std::memory_order_relaxed);
}


static int GetItemSlotMax(std::uint32_t itemHash, std::uint32_t slotHash)
{
    return invoke<int>(
        N_GET_ITEM_SLOT_MAX_COUNT,
        static_cast<int>(itemHash),
        static_cast<int>(slotHash));
}


// ============================================================
// v2.12 UNIVERSAL LIVE ITEMDATABASE RESOLVER / PATCH
// ============================================================
// v2.11 proved the real 1491.50 path:
//   item-table descriptor -> 0xD8-byte item records
//   item record + 8       -> live item object
//   item object + 0x98    -> multiplicity-array pointer
//   item object + 0xA0    -> multiplicity record count (u16)
//   each multiplicity     -> 24 bytes:
//       +0x00 internal pointer/type
//       +0x08 signed 64-bit quantity/max
//       +0x10 slot hash
//
// The item-table descriptor itself was observed at RDI when execution reaches
// RDR2.exe + 0x0077CAC6. That instruction is:
//   0F B7 4F 10    movzx ecx, word ptr [rdi+10h]
//
// v2.12 captures that descriptor with a one-instruction no-gap trampoline,
// validates Wild Mint and AMMO_REVOLVER inside the table, then walks Rockstar's
// live objects directly. It does NOT care what numeric values catalog_sp.ymt
// supplied. This is the catalog-independent path.
//
// Build-specific code RVA: RDR2.exe 1.0.1491.50 only.

static constexpr std::uintptr_t V212_ITEM_TABLE_LOOKUP_RVA = 0x0077CAC6u;
static constexpr std::size_t V212_LOOKUP_INSTRUCTION_LENGTH = 4u;
static constexpr std::size_t V212_ITEM_RECORD_STRIDE = 0xD8u;
static constexpr std::size_t V212_ITEM_OBJECT_OFFSET = 0x08u;
static constexpr std::size_t V212_MULTIPLICITY_PTR_OFFSET = 0x98u;
static constexpr std::size_t V212_MULTIPLICITY_COUNT_OFFSET = 0xA0u;
static constexpr std::size_t V212_MULTIPLICITY_RECORD_STRIDE = 24u;
static constexpr std::size_t V212_MULTIPLICITY_QUANTITY_OFFSET = 0x08u;
static constexpr std::size_t V212_MULTIPLICITY_SLOT_OFFSET = 0x10u;

static constexpr std::uint8_t V212_EXPECTED_LOOKUP_BYTES[12] = {
    0x0F, 0xB7, 0x4F, 0x10, 0x48, 0x69, 0xD1, 0xD8, 0x00, 0x00, 0x00, 0x48
};

static std::atomic<std::uintptr_t> g_v212LatestDescriptor{0};
static std::atomic<std::uintptr_t> g_v212ValidatedDescriptor{0};
static std::atomic<int> g_v212ItemCount{0};
static std::atomic<int> g_v212SatchelTotal{0};
static std::atomic<int> g_v212SatchelPositive{0};
static std::atomic<int> g_v212SatchelNegative{0};
static std::atomic<int> g_v212SatchelZero{0};
static std::atomic<int> g_v212CardsFound{0};
static std::atomic<int> g_v212WatchesFound{0};
static std::atomic<int> g_v212AmmoFound{0};
static std::atomic<int> g_v212Writes{0};
static std::atomic<int> g_v212AlreadyAtTarget{0};
static std::atomic<int> g_v212DirectVerifyFailures{0};
static std::atomic<int> g_v212SpecificDuplicates{0};
static std::atomic<bool> g_v212UniversalPatched{false};
static std::atomic<bool> g_v212NativeVerified{false};
static std::atomic<ULONGLONG> g_v212CaptureTick{0};
static std::atomic<ULONGLONG> g_v212PatchTick{0};

static std::uintptr_t g_v212GameBase = 0;
static std::uintptr_t g_v212LookupAddress = 0;
static BYTE g_v212LookupOriginalBytes[16]{};
static void* g_v212LookupTrampoline = nullptr;
static DWORD g_v212LookupOldProtect = 0;
static PVOID g_v212LookupVeh = nullptr;
static std::atomic<bool> g_v212LookupHookInstalled{false};
static std::mutex g_v212HookMutex;

static bool V212ReadBytes(
    std::uintptr_t address,
    void* output,
    SIZE_T size)
{
    if (address == 0 || output == nullptr || size == 0)
        return false;

    SIZE_T got = 0;
    return ReadProcessMemory(
               GetCurrentProcess(),
               reinterpret_cast<LPCVOID>(address),
               output,
               size,
               &got) != FALSE &&
           got == size;
}

static bool V212ReadU16(std::uintptr_t address, std::uint16_t& value)
{
    value = 0;
    return V212ReadBytes(address, &value, sizeof(value));
}

static bool V212ReadU32(std::uintptr_t address, std::uint32_t& value)
{
    value = 0;
    return V212ReadBytes(address, &value, sizeof(value));
}

static bool V212ReadU64(std::uintptr_t address, std::uint64_t& value)
{
    value = 0;
    return V212ReadBytes(address, &value, sizeof(value));
}

static int V212FindCardIndex(std::uint32_t hash)
{
    for (SIZE_T i = 0; i < V212_CIGARETTE_CARD_HASHES.size(); ++i)
    {
        if (V212_CIGARETTE_CARD_HASHES[i] == hash)
            return static_cast<int>(i);
    }
    return -1;
}

static int V212FindWatchIndex(std::uint32_t hash)
{
    for (SIZE_T i = 0; i < V181_WATCH_VERIFY_ITEMS.size(); ++i)
    {
        if (V181_WATCH_VERIFY_ITEMS[i].hash == hash)
            return static_cast<int>(i);
    }
    return -1;
}

static int V212FindAmmoIndex(std::uint32_t hash)
{
    for (SIZE_T i = 0; i < V181_AMMO_MAP_ITEMS.size(); ++i)
    {
        if (V181_AMMO_MAP_ITEMS[i].hash == hash)
            return static_cast<int>(i);
    }
    return -1;
}

static LONG CALLBACK V212LookupCaptureVeh(PEXCEPTION_POINTERS exceptionInfo)
{
    if (exceptionInfo == nullptr ||
        exceptionInfo->ExceptionRecord == nullptr ||
        exceptionInfo->ContextRecord == nullptr)
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    if (exceptionInfo->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT ||
        !g_v212LookupHookInstalled.load(std::memory_order_acquire) ||
        exceptionInfo->ExceptionRecord->ExceptionAddress !=
            reinterpret_cast<void*>(g_v212LookupAddress) ||
        g_v212LookupTrampoline == nullptr)
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    CONTEXT* cpu = exceptionInfo->ContextRecord;

    // RDI is the item-table descriptor at this exact 1491.50 instruction.
    // Keep the newest non-null candidate. The worker validates it before any
    // write occurs, so an early/unrelated candidate cannot be patched blindly.
    if (cpu->Rdi != 0)
    {
        g_v212LatestDescriptor.store(
            static_cast<std::uintptr_t>(cpu->Rdi),
            std::memory_order_release);

        if (g_v212CaptureTick.load(std::memory_order_relaxed) == 0)
        {
            g_v212CaptureTick.store(
                GetTickCount64(),
                std::memory_order_relaxed);
        }
    }

    // Execute the copied original instruction and jump back to +4 without
    // ever exposing an unhooked window to another game thread.
    cpu->Rip = reinterpret_cast<DWORD64>(g_v212LookupTrampoline);
    return EXCEPTION_CONTINUE_EXECUTION;
}

static bool V212InstallLookupCaptureHook()
{
    std::lock_guard<std::mutex> lock(g_v212HookMutex);

    if (g_v212LookupHookInstalled.load(std::memory_order_acquire))
        return true;

    g_v212GameBase = reinterpret_cast<std::uintptr_t>(
        GetModuleHandleW(nullptr));
    if (g_v212GameBase == 0)
        return false;

    g_v212LookupAddress =
        g_v212GameBase + V212_ITEM_TABLE_LOOKUP_RVA;

    BYTE actual[sizeof(V212_EXPECTED_LOOKUP_BYTES)]{};
    if (!V212ReadBytes(
            g_v212LookupAddress,
            actual,
            sizeof(actual)) ||
        std::memcmp(
            actual,
            V212_EXPECTED_LOOKUP_BYTES,
            sizeof(actual)) != 0)
    {
        return false;
    }

    std::memcpy(
        g_v212LookupOriginalBytes,
        actual,
        V212_LOOKUP_INSTRUCTION_LENGTH);

    // copied 4-byte instruction + 14-byte RIP-indirect absolute jump
    BYTE trampoline[32]{};
    std::memcpy(
        trampoline,
        g_v212LookupOriginalBytes,
        V212_LOOKUP_INSTRUCTION_LENGTH);

    SIZE_T p = V212_LOOKUP_INSTRUCTION_LENGTH;
    trampoline[p + 0] = 0xFF;
    trampoline[p + 1] = 0x25;
    trampoline[p + 2] = 0x00;
    trampoline[p + 3] = 0x00;
    trampoline[p + 4] = 0x00;
    trampoline[p + 5] = 0x00;

    const std::uint64_t returnAddress =
        static_cast<std::uint64_t>(
            g_v212LookupAddress +
            V212_LOOKUP_INSTRUCTION_LENGTH);
    std::memcpy(
        trampoline + p + 6,
        &returnAddress,
        sizeof(returnAddress));

    g_v212LookupTrampoline = VirtualAlloc(
        nullptr,
        sizeof(trampoline),
        MEM_COMMIT | MEM_RESERVE,
        PAGE_EXECUTE_READWRITE);
    if (g_v212LookupTrampoline == nullptr)
        return false;

    std::memcpy(
        g_v212LookupTrampoline,
        trampoline,
        sizeof(trampoline));
    FlushInstructionCache(
        GetCurrentProcess(),
        g_v212LookupTrampoline,
        sizeof(trampoline));

    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const std::uintptr_t pageSize =
        static_cast<std::uintptr_t>(si.dwPageSize);
    const std::uintptr_t pageBase =
        g_v212LookupAddress & ~(pageSize - 1u);

    DWORD oldProtect = 0;
    if (!VirtualProtect(
            reinterpret_cast<LPVOID>(pageBase),
            static_cast<SIZE_T>(pageSize),
            PAGE_EXECUTE_READWRITE,
            &oldProtect))
    {
        VirtualFree(g_v212LookupTrampoline, 0, MEM_RELEASE);
        g_v212LookupTrampoline = nullptr;
        return false;
    }

    g_v212LookupOldProtect = oldProtect;

    g_v212LookupVeh =
        AddVectoredExceptionHandler(1, V212LookupCaptureVeh);
    if (g_v212LookupVeh == nullptr)
    {
        DWORD ignored = 0;
        VirtualProtect(
            reinterpret_cast<LPVOID>(pageBase),
            static_cast<SIZE_T>(pageSize),
            oldProtect,
            &ignored);
        VirtualFree(g_v212LookupTrampoline, 0, MEM_RELEASE);
        g_v212LookupTrampoline = nullptr;
        g_v212LookupOldProtect = 0;
        return false;
    }

    *reinterpret_cast<volatile BYTE*>(g_v212LookupAddress) = 0xCC;
    FlushInstructionCache(
        GetCurrentProcess(),
        reinterpret_cast<LPCVOID>(g_v212LookupAddress),
        1u);

    g_v212LookupHookInstalled.store(
        true,
        std::memory_order_release);
    return true;
}

static void V212RemoveLookupCaptureHook()
{
    std::lock_guard<std::mutex> lock(g_v212HookMutex);

    if (g_v212LookupAddress != 0 &&
        g_v212LookupHookInstalled.load(std::memory_order_acquire))
    {
        *reinterpret_cast<volatile BYTE*>(g_v212LookupAddress) =
            g_v212LookupOriginalBytes[0];
        FlushInstructionCache(
            GetCurrentProcess(),
            reinterpret_cast<LPCVOID>(g_v212LookupAddress),
            1u);
    }

    g_v212LookupHookInstalled.store(
        false,
        std::memory_order_release);

    if (g_v212LookupVeh != nullptr)
    {
        RemoveVectoredExceptionHandler(g_v212LookupVeh);
        g_v212LookupVeh = nullptr;
    }

    if (g_v212LookupAddress != 0 &&
        g_v212LookupOldProtect != 0)
    {
        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        const std::uintptr_t pageSize =
            static_cast<std::uintptr_t>(si.dwPageSize);
        const std::uintptr_t pageBase =
            g_v212LookupAddress & ~(pageSize - 1u);

        DWORD ignored = 0;
        VirtualProtect(
            reinterpret_cast<LPVOID>(pageBase),
            static_cast<SIZE_T>(pageSize),
            g_v212LookupOldProtect,
            &ignored);
        g_v212LookupOldProtect = 0;
    }

    if (g_v212LookupTrampoline != nullptr)
    {
        VirtualFree(g_v212LookupTrampoline, 0, MEM_RELEASE);
        g_v212LookupTrampoline = nullptr;
    }
}

struct V212TableView
{
    std::uintptr_t descriptor = 0;
    std::uintptr_t itemBase = 0;
    std::uint16_t itemCount = 0;
};

static bool V212GetTableView(
    std::uintptr_t descriptor,
    V212TableView& view)
{
    view = V212TableView{};

    std::uint64_t itemBaseRaw = 0;
    std::uint16_t itemCount = 0;

    if (!V212ReadU64(descriptor + 0x08u, itemBaseRaw) ||
        !V212ReadU16(descriptor + 0x10u, itemCount))
    {
        return false;
    }

    const std::uintptr_t itemBase =
        static_cast<std::uintptr_t>(itemBaseRaw);

    // Known 1491.50 Story Mode catalogs are around five thousand items.
    // Keep the guard deliberately broad enough for overhauls/merges.
    if (itemBase == 0 ||
        itemCount < 1000u ||
        itemCount > 30000u)
    {
        return false;
    }

    const std::uint64_t span =
        static_cast<std::uint64_t>(itemCount) *
        static_cast<std::uint64_t>(V212_ITEM_RECORD_STRIDE);

    const std::uintptr_t maxAddress =
        static_cast<std::uintptr_t>(~std::uintptr_t{0});
    if (span >
        static_cast<std::uint64_t>(maxAddress - itemBase))
    {
        return false;
    }

    std::uint32_t firstHash = 0;
    std::uint32_t lastHash = 0;
    if (!V212ReadU32(itemBase, firstHash) ||
        !V212ReadU32(
            itemBase +
                (static_cast<std::uintptr_t>(itemCount) - 1u) *
                    V212_ITEM_RECORD_STRIDE,
            lastHash))
    {
        return false;
    }

    // Zero is allowed as data in many places, but an item table whose first
    // AND last keys are both zero is not credible enough to patch.
    if (firstHash == 0 && lastHash == 0)
        return false;

    view.descriptor = descriptor;
    view.itemBase = itemBase;
    view.itemCount = itemCount;
    return true;
}

struct V212ResolvedMultiplicity
{
    bool itemFound = false;
    bool slotFound = false;
    std::uintptr_t quantityAddress = 0;
    std::int64_t quantity = 0;
    std::uint16_t multiplicityCount = 0;
};

static V212ResolvedMultiplicity V212ResolveItemSlot(
    const V212TableView& view,
    std::uint32_t itemHash,
    std::uint32_t slotHash)
{
    V212ResolvedMultiplicity result{};

    for (std::uint16_t i = 0; i < view.itemCount; ++i)
    {
        const std::uintptr_t record =
            view.itemBase +
            static_cast<std::uintptr_t>(i) *
                V212_ITEM_RECORD_STRIDE;

        std::uint32_t hash = 0;
        if (!V212ReadU32(record, hash))
            return result;

        if (hash != itemHash)
            continue;

        result.itemFound = true;

        const std::uintptr_t object =
            record + V212_ITEM_OBJECT_OFFSET;

        std::uint64_t multiplicityRaw = 0;
        std::uint16_t multiplicityCount = 0;

        if (!V212ReadU64(
                object + V212_MULTIPLICITY_PTR_OFFSET,
                multiplicityRaw) ||
            !V212ReadU16(
                object + V212_MULTIPLICITY_COUNT_OFFSET,
                multiplicityCount))
        {
            return result;
        }

        result.multiplicityCount = multiplicityCount;

        if (multiplicityCount == 0 ||
            multiplicityCount > 64u ||
            multiplicityRaw == 0)
        {
            return result;
        }

        const std::uintptr_t multiplicity =
            static_cast<std::uintptr_t>(multiplicityRaw);

        for (std::uint16_t m = 0;
             m < multiplicityCount;
             ++m)
        {
            const std::uintptr_t entry =
                multiplicity +
                static_cast<std::uintptr_t>(m) *
                    V212_MULTIPLICITY_RECORD_STRIDE;

            std::uint32_t slot = 0;
            std::int64_t quantity = 0;

            if (!V212ReadU32(
                    entry + V212_MULTIPLICITY_SLOT_OFFSET,
                    slot) ||
                !ReadI64Address(
                    entry + V212_MULTIPLICITY_QUANTITY_OFFSET,
                    quantity))
            {
                return result;
            }

            if (slot == slotHash)
            {
                if (result.slotFound)
                {
                    // Ambiguous duplicate item+slot: refuse to resolve.
                    result.quantityAddress = 0;
                    return result;
                }

                result.slotFound = true;
                result.quantityAddress =
                    entry + V212_MULTIPLICITY_QUANTITY_OFFSET;
                result.quantity = quantity;
            }
        }

        return result;
    }

    return result;
}

static bool V212ValidateDescriptor(
    std::uintptr_t descriptor,
    V212TableView& view)
{
    if (!V212GetTableView(descriptor, view))
        return false;

    const V212ResolvedMultiplicity mint =
        V212ResolveItemSlot(
            view,
            V181_WILD_MINT,
            V181_SLOT_SATCHEL);

    const V212ResolvedMultiplicity ammo =
        V212ResolveItemSlot(
            view,
            0x64356159u, // AMMO_REVOLVER
            V181_SLOT_ZERO);

    if (!mint.itemFound ||
        !mint.slotFound ||
        mint.quantityAddress == 0 ||
        !ammo.itemFound ||
        !ammo.slotFound ||
        ammo.quantityAddress == 0)
    {
        return false;
    }

    // Quantities may be changed by any replacement catalog, so do not require
    // vanilla/CI starting values. Only reject absurd values that strongly
    // suggest this is not the live multiplicity table.
    if (mint.quantity < -1 ||
        mint.quantity > 1000000000LL ||
        ammo.quantity < -1 ||
        ammo.quantity > 1000000000LL)
    {
        return false;
    }

    return true;
}

enum class V212TargetKind : std::uint8_t
{
    Satchel,
    Card,
    Watch,
    Ammo
};

struct V212Target
{
    std::uintptr_t address = 0;
    std::int64_t original = 0;
    std::int64_t desired = 0;
    V212TargetKind kind = V212TargetKind::Satchel;
    std::uint32_t itemHash = 0;
};

static bool V212AddTarget(
    std::vector<V212Target>& targets,
    std::uintptr_t address,
    std::int64_t original,
    std::int64_t desired,
    V212TargetKind kind,
    std::uint32_t itemHash)
{
    for (const auto& existing : targets)
    {
        if (existing.address == address)
            return existing.desired == desired;
    }

    targets.push_back(
        {address, original, desired, kind, itemHash});
    return true;
}

static bool V212PatchValidatedDescriptor(
    const V212TableView& view,
    bool writeDetailedLog)
{
    std::array<bool, V212_CIGARETTE_CARD_HASHES.size()> cardFound{};
    std::array<bool, V181_WATCH_VERIFY_ITEMS.size()> watchFound{};
    std::array<bool, V181_AMMO_MAP_ITEMS.size()> ammoFound{};

    std::vector<V212Target> targets;
    targets.reserve(800u);

    int satchelTotal = 0;
    int satchelPositive = 0;
    int satchelNegative = 0;
    int satchelZero = 0;
    int specificDuplicates = 0;

    for (std::uint16_t i = 0; i < view.itemCount; ++i)
    {
        const std::uintptr_t record =
            view.itemBase +
            static_cast<std::uintptr_t>(i) *
                V212_ITEM_RECORD_STRIDE;

        std::uint32_t itemHash = 0;
        if (!V212ReadU32(record, itemHash))
            return false;

        const std::uintptr_t object =
            record + V212_ITEM_OBJECT_OFFSET;

        std::uint64_t multiplicityRaw = 0;
        std::uint16_t multiplicityCount = 0;

        if (!V212ReadU64(
                object + V212_MULTIPLICITY_PTR_OFFSET,
                multiplicityRaw) ||
            !V212ReadU16(
                object + V212_MULTIPLICITY_COUNT_OFFSET,
                multiplicityCount))
        {
            return false;
        }

        if (multiplicityCount == 0)
            continue;

        if (multiplicityCount > 64u ||
            multiplicityRaw == 0)
        {
            return false;
        }

        const int cardIndex = V212FindCardIndex(itemHash);
        const int watchIndex = V212FindWatchIndex(itemHash);
        const int ammoIndex = V212FindAmmoIndex(itemHash);

        int cardSlotMatches = 0;
        int watchSlotMatches = 0;
        int ammoSlotMatches = 0;

        const std::uintptr_t multiplicity =
            static_cast<std::uintptr_t>(multiplicityRaw);

        for (std::uint16_t m = 0;
             m < multiplicityCount;
             ++m)
        {
            const std::uintptr_t entry =
                multiplicity +
                static_cast<std::uintptr_t>(m) *
                    V212_MULTIPLICITY_RECORD_STRIDE;

            std::uint32_t slot = 0;
            std::int64_t quantity = 0;

            if (!V212ReadU32(
                    entry + V212_MULTIPLICITY_SLOT_OFFSET,
                    slot) ||
                !ReadI64Address(
                    entry + V212_MULTIPLICITY_QUANTITY_OFFSET,
                    quantity))
            {
                return false;
            }

            const std::uintptr_t quantityAddress =
                entry + V212_MULTIPLICITY_QUANTITY_OFFSET;

            if (slot == V181_SLOT_SATCHEL)
            {
                ++satchelTotal;

                if (quantity > 0)
                {
                    ++satchelPositive;
                    const std::int64_t desired =
                        (quantity >= V181_TARGET_MAX)
                            ? quantity
                            : V181_TARGET_MAX;

                    if (!V212AddTarget(
                            targets,
                            quantityAddress,
                            quantity,
                            desired,
                            V212TargetKind::Satchel,
                            itemHash))
                    {
                        return false;
                    }
                }
                else if (quantity < 0)
                {
                    ++satchelNegative;
                }
                else
                {
                    ++satchelZero;
                }
            }

            if (cardIndex >= 0 &&
                slot == V181_SLOT_ZERO)
            {
                ++cardSlotMatches;
                cardFound[static_cast<SIZE_T>(cardIndex)] = true;

                if (quantity > 0)
                {
                    const std::int64_t desired =
                        (quantity >= V181_TARGET_MAX)
                            ? quantity
                            : V181_TARGET_MAX;

                    if (!V212AddTarget(
                            targets,
                            quantityAddress,
                            quantity,
                            desired,
                            V212TargetKind::Card,
                            itemHash))
                    {
                        return false;
                    }
                }
            }

            if (watchIndex >= 0 &&
                slot == V181_SLOT_WATCH)
            {
                ++watchSlotMatches;
                watchFound[static_cast<SIZE_T>(watchIndex)] = true;

                if (quantity > 0)
                {
                    const std::int64_t desired =
                        (quantity >= V181_TARGET_MAX)
                            ? quantity
                            : V181_TARGET_MAX;

                    if (!V212AddTarget(
                            targets,
                            quantityAddress,
                            quantity,
                            desired,
                            V212TargetKind::Watch,
                            itemHash))
                    {
                        return false;
                    }
                }
            }

            if (ammoIndex >= 0 &&
                slot == V181_SLOT_ZERO)
            {
                ++ammoSlotMatches;
                ammoFound[static_cast<SIZE_T>(ammoIndex)] = true;

                if (!V212AddTarget(
                        targets,
                        quantityAddress,
                        quantity,
                        V181_AMMO_TEST_MAX,
                        V212TargetKind::Ammo,
                        itemHash))
                {
                    return false;
                }
            }
        }

        if (cardSlotMatches > 1 ||
            watchSlotMatches > 1 ||
            ammoSlotMatches > 1)
        {
            ++specificDuplicates;
        }
    }

    int cardsFound = 0;
    int watchesFound = 0;
    int ammoFoundCount = 0;

    for (bool value : cardFound)
        if (value) ++cardsFound;
    for (bool value : watchFound)
        if (value) ++watchesFound;
    for (bool value : ammoFound)
        if (value) ++ammoFoundCount;

    g_v212ItemCount.store(
        static_cast<int>(view.itemCount),
        std::memory_order_relaxed);
    g_v212SatchelTotal.store(
        satchelTotal,
        std::memory_order_relaxed);
    g_v212SatchelPositive.store(
        satchelPositive,
        std::memory_order_relaxed);
    g_v212SatchelNegative.store(
        satchelNegative,
        std::memory_order_relaxed);
    g_v212SatchelZero.store(
        satchelZero,
        std::memory_order_relaxed);
    g_v212CardsFound.store(
        cardsFound,
        std::memory_order_relaxed);
    g_v212WatchesFound.store(
        watchesFound,
        std::memory_order_relaxed);
    g_v212AmmoFound.store(
        ammoFoundCount,
        std::memory_order_relaxed);
    g_v212SpecificDuplicates.store(
        specificDuplicates,
        std::memory_order_relaxed);

    // These are the known Rockstar core identities we promise to patch.
    // Starting VALUES are deliberately not validated.
    if (satchelPositive <= 0 ||
        cardsFound !=
            static_cast<int>(V212_CIGARETTE_CARD_HASHES.size()) ||
        watchesFound !=
            static_cast<int>(V181_WATCH_VERIFY_ITEMS.size()) ||
        ammoFoundCount !=
            static_cast<int>(V181_AMMO_MAP_ITEMS.size()) ||
        specificDuplicates != 0)
    {
        if (writeDetailedLog)
        {
            WriteLog(
                "[v2.12 UNIVERSAL] REFUSED before writes: "
                "required core identities/slots were not uniquely resolved.");
        }
        return false;
    }

    struct Change
    {
        std::uintptr_t address = 0;
        std::int64_t oldValue = 0;
        std::int64_t newValue = 0;
    };

    std::vector<Change> changes;
    changes.reserve(targets.size());

    int already = 0;
    for (const auto& target : targets)
    {
        if (target.original == target.desired)
        {
            ++already;
            continue;
        }

        changes.push_back(
            {target.address, target.original, target.desired});
    }

    SIZE_T completed = 0;
    for (; completed < changes.size(); ++completed)
    {
        if (!WriteI64Address(
                changes[completed].address,
                changes[completed].newValue))
        {
            break;
        }

        std::int64_t readBack = 0;
        if (!ReadI64Address(
                changes[completed].address,
                readBack) ||
            readBack != changes[completed].newValue)
        {
            break;
        }
    }

    if (completed != changes.size())
    {
        const SIZE_T rollbackCount =
            (completed < changes.size())
                ? completed + 1u
                : completed;

        for (SIZE_T i = 0;
             i < rollbackCount && i < changes.size();
             ++i)
        {
            WriteI64Address(
                changes[i].address,
                changes[i].oldValue);
        }

        g_v212Writes.store(0, std::memory_order_relaxed);
        g_v212DirectVerifyFailures.store(1, std::memory_order_relaxed);
        return false;
    }

    int verifyFailures = 0;
    for (const auto& target : targets)
    {
        std::int64_t current = 0;
        if (!ReadI64Address(target.address, current))
        {
            ++verifyFailures;
            continue;
        }

        if (target.kind == V212TargetKind::Ammo)
        {
            if (current != V181_AMMO_TEST_MAX)
                ++verifyFailures;
        }
        else
        {
            if (current < V181_TARGET_MAX)
                ++verifyFailures;
        }
    }

    if (verifyFailures != 0)
    {
        for (const auto& change : changes)
        {
            WriteI64Address(
                change.address,
                change.oldValue);
        }

        g_v212Writes.store(0, std::memory_order_relaxed);
        g_v212DirectVerifyFailures.store(
            verifyFailures,
            std::memory_order_relaxed);
        return false;
    }

    g_v212Writes.store(
        static_cast<int>(changes.size()),
        std::memory_order_relaxed);
    g_v212AlreadyAtTarget.store(
        already,
        std::memory_order_relaxed);
    g_v212DirectVerifyFailures.store(
        0,
        std::memory_order_relaxed);
    g_v212ValidatedDescriptor.store(
        view.descriptor,
        std::memory_order_release);
    g_v212UniversalPatched.store(
        true,
        std::memory_order_release);

    if (g_v212PatchTick.load(std::memory_order_relaxed) == 0)
    {
        g_v212PatchTick.store(
            GetTickCount64(),
            std::memory_order_relaxed);
    }

    if (writeDetailedLog)
    {
        WriteLog(
            "[v2.12 UNIVERSAL] Direct live ItemDatabase patch completed. "
            "Starting catalog values were ignored.");
    }

    return true;
}

static bool V212TryPatchDescriptor(
    std::uintptr_t descriptor,
    bool writeDetailedLog)
{
    if (descriptor == 0)
        return false;

    V212TableView view{};
    if (!V212ValidateDescriptor(descriptor, view))
        return false;

    if (g_v181PoolSeenTick.load(std::memory_order_relaxed) == 0)
    {
        g_v181PoolSeenTick.store(
            GetTickCount64(),
            std::memory_order_relaxed);
    }

    const bool ok =
        V212PatchValidatedDescriptor(
            view,
            writeDetailedLog);

    if (ok)
    {
        g_v181PatchTick.store(
            g_v212PatchTick.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        g_v181State.store(
            static_cast<int>(V181BootstrapState::Patched),
            std::memory_order_release);
    }

    return ok;
}

static DWORD WINAPI V212UniversalBootstrapThread(LPVOID)
{
    g_v181WorkerTick.store(
        GetTickCount64(),
        std::memory_order_relaxed);

    g_v181State.store(
        static_cast<int>(V181BootstrapState::Running),
        std::memory_order_release);

    // Arm once and let Rockstar's own normal ItemDatabase traffic hand us the
    // descriptor. No process-memory catalog scan is performed.
    V212InstallLookupCaptureHook();

    while (!g_v181Stop.load(std::memory_order_relaxed) &&
           !g_v212UniversalPatched.load(std::memory_order_acquire))
    {
        const std::uintptr_t descriptor =
            g_v212LatestDescriptor.load(
                std::memory_order_acquire);

        if (descriptor != 0 &&
            V212TryPatchDescriptor(
                descriptor,
                false))
        {
            break;
        }

        Sleep(5u);
    }

    if (g_v212UniversalPatched.load(std::memory_order_acquire))
    {
        V212RemoveLookupCaptureHook();
    }

    return 0;
}

static bool V212EnsurePatchAtScriptMain()
{
    if (g_v212UniversalPatched.load(std::memory_order_acquire))
        return true;

    if (!g_v212LookupHookInstalled.load(std::memory_order_acquire))
    {
        if (!V212InstallLookupCaptureHook())
        {
            WriteLog(
                "[v2.12 UNIVERSAL] Could not install 1491.50 item-table capture hook.");
            return false;
        }
    }

    // Force one normal Rockstar lookup on the script thread. v2.11 proved this
    // path reaches +0x77CAC6 and places the item-table descriptor in RDI.
    const int before =
        GetItemSlotMax(
            V181_WILD_MINT,
            V181_SLOT_SATCHEL);

    const std::uintptr_t descriptor =
        g_v212LatestDescriptor.load(
            std::memory_order_acquire);

    WriteLog(
        "[v2.12 UNIVERSAL] ScriptMain fallback lookup beforePatch=" +
        std::to_string(before) +
        " descriptor=" +
        Hex(descriptor, 16));

    const bool ok =
        V212TryPatchDescriptor(
            descriptor,
            true);

    if (ok)
        V212RemoveLookupCaptureHook();

    return ok;
}

static void V212LogUniversalStats()
{
    WriteLog(
        "[v2.12 UNIVERSAL STATS] descriptor=" +
        Hex(
            g_v212ValidatedDescriptor.load(
                std::memory_order_acquire),
            16) +
        " itemCount=" +
        std::to_string(
            g_v212ItemCount.load(
                std::memory_order_relaxed)));

    WriteLog(
        "[v2.12 UNIVERSAL STATS] SATCHEL total=" +
        std::to_string(
            g_v212SatchelTotal.load(
                std::memory_order_relaxed)) +
        " positive=" +
        std::to_string(
            g_v212SatchelPositive.load(
                std::memory_order_relaxed)) +
        " negative=" +
        std::to_string(
            g_v212SatchelNegative.load(
                std::memory_order_relaxed)) +
        " zero=" +
        std::to_string(
            g_v212SatchelZero.load(
                std::memory_order_relaxed)));

    WriteLog(
        "[v2.12 UNIVERSAL STATS] cards=" +
        std::to_string(
            g_v212CardsFound.load(
                std::memory_order_relaxed)) +
        "/144 watches=" +
        std::to_string(
            g_v212WatchesFound.load(
                std::memory_order_relaxed)) +
        "/4 ammo=" +
        std::to_string(
            g_v212AmmoFound.load(
                std::memory_order_relaxed)) +
        "/43 duplicates=" +
        std::to_string(
            g_v212SpecificDuplicates.load(
                std::memory_order_relaxed)));

    WriteLog(
        "[v2.12 UNIVERSAL STATS] writes=" +
        std::to_string(
            g_v212Writes.load(
                std::memory_order_relaxed)) +
        " alreadyAtTarget=" +
        std::to_string(
            g_v212AlreadyAtTarget.load(
                std::memory_order_relaxed)) +
        " directVerifyFailures=" +
        std::to_string(
            g_v212DirectVerifyFailures.load(
                std::memory_order_relaxed)));

    const ULONGLONG captureTick =
        g_v212CaptureTick.load(
            std::memory_order_relaxed);
    const ULONGLONG patchTick =
        g_v212PatchTick.load(
            std::memory_order_relaxed);

    if (captureTick != 0)
    {
        WriteLog(
            "[v2.12 UNIVERSAL TIMING] descriptor first captured " +
            std::to_string(
                captureTick - g_v181DllAttachTick) +
            " ms after DLL attach.");
    }

    if (patchTick != 0)
    {
        WriteLog(
            "[v2.12 UNIVERSAL TIMING] live capacities first patched " +
            std::to_string(
                patchTick - g_v181DllAttachTick) +
            " ms after DLL attach (" +
            std::to_string(
                static_cast<long long>(
                    g_v181ScriptMainTick) -
                static_cast<long long>(
                    patchTick)) +
            " ms before ScriptMain; negative means ScriptMain fallback).");
    }
}


static bool PatchEarlyCardsAndWatches(
    std::uintptr_t regionBase,
    std::uint64_t realSharedPointer,
    V181SpecialPatchStats& statsOut)
{
    statsOut = V181SpecialPatchStats{};

    std::vector<std::uint8_t> buffer(V181_REGION_SIZE);
    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory(
            GetCurrentProcess(),
            reinterpret_cast<LPCVOID>(regionBase),
            buffer.data(),
            buffer.size(),
            &bytesRead) ||
        bytesRead < V181_RECORD_SIZE)
    {
        return false;
    }

    if (bytesRead < buffer.size())
        buffer.resize(bytesRead);

    std::vector<std::uintptr_t> cardStyle;
    std::vector<std::uintptr_t> watches;

    for (SIZE_T pos = 0; pos + V181_RECORD_SIZE <= buffer.size(); pos += 8u)
    {
        const std::int64_t quantity = RawI64(buffer.data(), pos + 0u);
        const std::uint64_t slotRaw = RawU64(buffer.data(), pos + 8u);
        const std::uint64_t pointer = RawU64(buffer.data(), pos + 16u);

        if (pointer != realSharedPointer || quantity != 5)
            continue;

        if (slotRaw == static_cast<std::uint64_t>(V181_SLOT_ZERO))
            cardStyle.push_back(regionBase + pos);
        else if (slotRaw == static_cast<std::uint64_t>(V181_SLOT_WATCH))
            watches.push_back(regionBase + pos);
    }

    statsOut.cardStyleRecords = static_cast<int>(cardStyle.size());
    statsOut.watchRecords = static_cast<int>(watches.size());

    if (statsOut.cardStyleRecords != V181_EXPECTED_CARD_STYLE_RECORDS ||
        statsOut.watchRecords != V181_EXPECTED_STACKABLE_WATCH_RECORDS)
    {
        return false;
    }

    struct Change
    {
        std::uintptr_t address = 0;
        std::int64_t oldValue = 0;
        bool watch = false;
    };

    std::vector<Change> changes;
    changes.reserve(cardStyle.size() + watches.size());

    for (const auto address : cardStyle)
        changes.push_back({address, 5, false});
    for (const auto address : watches)
        changes.push_back({address, 5, true});

    SIZE_T completed = 0;
    for (; completed < changes.size(); ++completed)
    {
        if (!WriteI64Address(changes[completed].address, V181_TARGET_MAX))
            break;

        std::int64_t readBack = 0;
        if (!ReadI64Address(changes[completed].address, readBack) ||
            readBack != V181_TARGET_MAX)
        {
            break;
        }

        if (changes[completed].watch)
            ++statsOut.watchWrites;
        else
            ++statsOut.cardStyleWrites;
    }

    if (completed != changes.size())
    {
        const SIZE_T rollbackCount =
            (completed < changes.size()) ? completed + 1u : completed;

        for (SIZE_T i = 0; i < rollbackCount && i < changes.size(); ++i)
            WriteI64Address(changes[i].address, changes[i].oldValue);

        return false;
    }

    // Publish the exact early candidate addresses only after every write and
    // readback succeeded. ScriptMain uses this list to identify and restore the
    // four unrelated DOCUMENT records, leaving exactly 144 cigarette cards high.
    {
        std::lock_guard<std::mutex> lock(g_v181SpecialMutex);
        g_v181CardStyleAddresses = cardStyle;
        g_v181WatchAddresses = watches;
    }

    g_v181SpecialCardStyleRecords.store(statsOut.cardStyleRecords, std::memory_order_relaxed);
    g_v181SpecialCardStyleWrites.store(statsOut.cardStyleWrites, std::memory_order_relaxed);
    g_v181SpecialWatchRecords.store(statsOut.watchRecords, std::memory_order_relaxed);
    g_v181SpecialWatchWrites.store(statsOut.watchWrites, std::memory_order_relaxed);
    g_v181SpecialGeneration.fetch_add(1, std::memory_order_release);

    return true;
}


static bool PatchMappedAmmoBaseRecords(
    std::uintptr_t regionBase,
    std::uint64_t expectedSharedPointer)
{
    struct Change
    {
        std::uintptr_t address = 0;
        std::int64_t oldValue = 0;
    };

    std::vector<Change> changes;
    changes.reserve(V181_AMMO_MAP_ITEMS.size());

    int records = 0;
    int already = 0;
    int unexpected = 0;
    int failures = 0;

    // TEMP TEST RULE:
    // Accept only the exact mapped vanilla base quantity for each ammo record,
    // or our already-applied -1 sentinel. This prevents the test from silently
    // overwriting an unrelated/modded capacity value.
    for (const auto& item : V181_AMMO_MAP_ITEMS)
    {
        const std::uintptr_t quantityAddress = regionBase + item.offset;

        std::int64_t quantity = 0;
        if (!ReadI64Address(quantityAddress, quantity))
        {
            ++failures;
            continue;
        }

        std::uint64_t pointer = 0;
        SIZE_T bytesRead = 0;
        if (!ReadProcessMemory(
                GetCurrentProcess(),
                reinterpret_cast<LPCVOID>(quantityAddress + 16u),
                &pointer,
                sizeof(pointer),
                &bytesRead) ||
            bytesRead != sizeof(pointer) ||
            pointer != expectedSharedPointer)
        {
            ++failures;
            continue;
        }

        if (quantity == V181_AMMO_TEST_MAX)
        {
            ++records;
            ++already;
            continue;
        }

        if (quantity != static_cast<std::int64_t>(item.catalogBase))
        {
            ++unexpected;
            ++failures;
            continue;
        }

        ++records;
        changes.push_back({quantityAddress, quantity});
    }

    if (records != static_cast<int>(V181_AMMO_MAP_ITEMS.size()) ||
        failures != 0)
    {
        g_v181AmmoRecords.store(records, std::memory_order_relaxed);
        g_v181AmmoWrites.store(0, std::memory_order_relaxed);
        g_v181AmmoAlreadyAtTarget.store(already, std::memory_order_relaxed);
        g_v181AmmoAboveTargetPreserved.store(unexpected, std::memory_order_relaxed);
        g_v181AmmoValidationFailures.store(failures, std::memory_order_relaxed);
        return false;
    }

    SIZE_T completed = 0;
    for (; completed < changes.size(); ++completed)
    {
        if (!WriteI64Address(
                changes[completed].address,
                V181_AMMO_TEST_MAX))
        {
            break;
        }

        std::int64_t readBack = 0;
        if (!ReadI64Address(
                changes[completed].address,
                readBack) ||
            readBack != V181_AMMO_TEST_MAX)
        {
            break;
        }
    }

    if (completed != changes.size())
    {
        const SIZE_T rollbackCount =
            (completed < changes.size()) ? completed + 1u : completed;

        for (SIZE_T i = 0;
             i < rollbackCount && i < changes.size();
             ++i)
        {
            WriteI64Address(
                changes[i].address,
                changes[i].oldValue);
        }

        g_v181AmmoRecords.store(records, std::memory_order_relaxed);
        g_v181AmmoWrites.store(0, std::memory_order_relaxed);
        g_v181AmmoAlreadyAtTarget.store(already, std::memory_order_relaxed);
        g_v181AmmoAboveTargetPreserved.store(unexpected, std::memory_order_relaxed);
        g_v181AmmoValidationFailures.store(1, std::memory_order_relaxed);
        return false;
    }

    // Verify every mapped record is now exactly -1 and still belongs to the
    // expected ItemDatabase record family.
    for (const auto& item : V181_AMMO_MAP_ITEMS)
    {
        const std::uintptr_t quantityAddress = regionBase + item.offset;

        std::int64_t quantity = 0;
        std::uint64_t pointer = 0;
        SIZE_T bytesRead = 0;

        if (!ReadI64Address(quantityAddress, quantity) ||
            !ReadProcessMemory(
                GetCurrentProcess(),
                reinterpret_cast<LPCVOID>(quantityAddress + 16u),
                &pointer,
                sizeof(pointer),
                &bytesRead) ||
            bytesRead != sizeof(pointer) ||
            pointer != expectedSharedPointer ||
            quantity != V181_AMMO_TEST_MAX)
        {
            for (const auto& change : changes)
                WriteI64Address(change.address, change.oldValue);

            g_v181AmmoRecords.store(records, std::memory_order_relaxed);
            g_v181AmmoWrites.store(0, std::memory_order_relaxed);
            g_v181AmmoAlreadyAtTarget.store(already, std::memory_order_relaxed);
            g_v181AmmoAboveTargetPreserved.store(unexpected, std::memory_order_relaxed);
            g_v181AmmoValidationFailures.store(1, std::memory_order_relaxed);
            return false;
        }
    }

    g_v181AmmoRecords.store(records, std::memory_order_relaxed);
    g_v181AmmoWrites.store(static_cast<int>(changes.size()), std::memory_order_relaxed);
    g_v181AmmoAlreadyAtTarget.store(already, std::memory_order_relaxed);
    g_v181AmmoAboveTargetPreserved.store(unexpected, std::memory_order_relaxed);
    g_v181AmmoValidationFailures.store(0, std::memory_order_relaxed);
    return true;
}

static bool PruneFourNonCardDocumentsFromCardBucket()
{
    const int generation = g_v181SpecialGeneration.load(std::memory_order_acquire);
    if (generation <= 0)
        return false;

    std::vector<std::uintptr_t> candidates;
    {
        std::lock_guard<std::mutex> lock(g_v181SpecialMutex);
        candidates = g_v181CardStyleAddresses;
    }

    if (static_cast<int>(candidates.size()) != V181_EXPECTED_CARD_STYLE_RECORDS)
    {
        WriteLog("[v1.81 SPECIAL PRUNE] Candidate address count is not 148; refusing post-native pruning.");
        g_v181PruneAttemptedGeneration = generation;
        return false;
    }

    std::array<std::uintptr_t, V181_EXPECTED_NONCARD_DOCUMENT_RECORDS> mapped{};
    std::vector<bool> used(candidates.size(), false);

    // All 148 were intentionally raised early. Now use the native as a live
    // oracle: perturb one multiplicity address, ask for one known non-card
    // document's slot max, and see whether that exact item follows the probe.
    for (SIZE_T docIndex = 0; docIndex < V181_NONCARD_DOCUMENTS.size(); ++docIndex)
    {
        const auto& doc = V181_NONCARD_DOCUMENTS[docIndex];
        const int beforeMax = GetItemSlotMax(doc.hash, V181_SLOT_ZERO);
        if (beforeMax != static_cast<int>(V181_TARGET_MAX))
        {
            WriteLog("[v1.81 SPECIAL PRUNE] " + std::string(doc.name) +
                     " did not report 500000 before mapping; got " +
                     std::to_string(beforeMax) + ". Aborting exact prune.");
            g_v181PruneAttemptedGeneration = generation;
            return false;
        }

        bool found = false;

        for (SIZE_T i = 0; i < candidates.size(); ++i)
        {
            if (used[i])
                continue;

            std::int64_t current = 0;
            if (!ReadI64Address(candidates[i], current) || current != V181_TARGET_MAX)
                continue;

            if (!WriteI64Address(candidates[i], V181_PROBE_MAX))
                continue;

            const int observed = GetItemSlotMax(doc.hash, V181_SLOT_ZERO);

            // Always restore the early target immediately after each probe.
            WriteI64Address(candidates[i], V181_TARGET_MAX);

            if (observed == static_cast<int>(V181_PROBE_MAX))
            {
                mapped[docIndex] = candidates[i];
                used[i] = true;
                found = true;
                WriteLog("[v1.81 SPECIAL PRUNE] mapped " + std::string(doc.name) +
                         " -> " + Hex(candidates[i], 16));
                break;
            }
        }

        if (!found)
        {
            WriteLog("[v1.81 SPECIAL PRUNE] Could not map " + std::string(doc.name) +
                     "; leaving all 148 early document records at 500000 rather than pruning partially.");
            g_v181PruneAttemptedGeneration = generation;
            return false;
        }
    }

    // Only after all four mappings are proven do we restore those four records.
    for (const auto address : mapped)
    {
        if (address == 0 || !WriteI64Address(address, 5))
        {
            // Best-effort rollback to the broad early state if a restore fails.
            for (const auto rollbackAddress : mapped)
            {
                if (rollbackAddress != 0)
                    WriteI64Address(rollbackAddress, V181_TARGET_MAX);
            }

            WriteLog("[v1.81 SPECIAL PRUNE] Failed restoring one non-card document to 5; rolled mapped records back to 500000.");
            g_v181PruneAttemptedGeneration = generation;
            return false;
        }
    }

    for (const auto& doc : V181_NONCARD_DOCUMENTS)
    {
        const int value = GetItemSlotMax(doc.hash, V181_SLOT_ZERO);
        if (value != 5)
        {
            for (const auto address : mapped)
                WriteI64Address(address, V181_TARGET_MAX);

            WriteLog("[v1.81 SPECIAL PRUNE] Native verification failed after restoring non-card documents; returned to broad 500000 state.");
            g_v181PruneAttemptedGeneration = generation;
            return false;
        }
    }

    for (const auto& card : V181_CARD_VERIFY_SAMPLES)
    {
        const int value = GetItemSlotMax(card.hash, V181_SLOT_ZERO);
        if (value < static_cast<int>(V181_TARGET_MAX))
        {
            for (const auto address : mapped)
                WriteI64Address(address, V181_TARGET_MAX);

            WriteLog("[v1.81 SPECIAL PRUNE] Card sample verification failed for " +
                     std::string(card.name) + "; returned to broad 500000 state.");
            g_v181PruneAttemptedGeneration = generation;
            return false;
        }
    }

    g_v181PruneAttemptedGeneration = generation;
    g_v181PrunedGeneration = generation;
    g_v181SpecialPruneSucceeded = true;

    WriteLog("[v1.81 SPECIAL PRUNE SUCCESS] Restored the 4 unrelated DOCUMENT records to 5; exactly 144 cigarette-card records remain targeted at 500000.");
    return true;
}

static bool FindVerifiedRegionAndPatchOnce()
{
    SYSTEM_INFO si{};
    GetSystemInfo(&si);

    std::uintptr_t address =
        reinterpret_cast<std::uintptr_t>(si.lpMinimumApplicationAddress);
    const std::uintptr_t maximum =
        reinterpret_cast<std::uintptr_t>(si.lpMaximumApplicationAddress);

    while (address < maximum)
    {
        if (g_v181Stop.load(std::memory_order_relaxed))
            return false;

        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(
                reinterpret_cast<LPCVOID>(address),
                &mbi,
                sizeof(mbi)) == 0)
        {
            break;
        }

        const std::uintptr_t base =
            reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        const SIZE_T size = mbi.RegionSize;

        if (mbi.State == MEM_COMMIT &&
            mbi.Type == MEM_PRIVATE &&
            IsTargetProtection(mbi.Protect) &&
            size == V181_REGION_SIZE)
        {
            ++g_v181ExactSizeVisits;

            std::uint64_t p14 = 0;
            std::uint64_t p15 = 0;
            std::uint64_t p16 = 0;

            const bool guard14 =
                BootstrapBlockMatches(base + V181_GUARD14_OFFSET, &p14);
            const bool mint15 =
                BootstrapBlockMatches(base + V181_MINT_OFFSET, &p15);
            const bool guard16 =
                BootstrapBlockMatches(base + V181_GUARD16_OFFSET, &p16);

            if (guard14 && mint15 && guard16 &&
                p14 == p15 && p15 == p16)
            {
                if (g_v181PoolSeenTick.load(std::memory_order_relaxed) == 0)
                {
                    g_v181PoolSeenTick.store(
                        GetTickCount64(),
                        std::memory_order_relaxed);
                }

                V181MintBlock mint{};
                mint.block = base + V181_MINT_OFFSET;
                mint.quantityBase = mint.block + V181_RECORD_SIZE;
                mint.quantity5 = mint.block + V181_RECORD_SIZE * 2u;
                mint.quantity84 = mint.block + V181_RECORD_SIZE * 3u;
                mint.sharedPointer = p15;

                V181PatchStats stats{};
                if (!PatchRealSatchelGroup(
                        base,
                        p15,
                        mint,
                        stats))
                {
                    StoreStats(stats);
                    return false;
                }

                V181SpecialPatchStats specialStats{};
                if (!PatchEarlyCardsAndWatches(base, p15, specialStats))
                {
                    // Roll the already-applied general pass back is intentionally not
                    // attempted here; the proven general 517-record patch is safe on
                    // its own. We simply refuse to claim the special buckets worked.
                    StoreStats(stats);
                    return false;
                }

                if (!PatchMappedAmmoBaseRecords(base, p15))
                {
                    StoreStats(stats);
                    return false;
                }

                g_v181Mint = mint;
                g_v181RegionBase.store(base, std::memory_order_release);
                StoreStats(stats);

                if (g_v181PatchTick.load(std::memory_order_relaxed) == 0)
                {
                    g_v181PatchTick.store(
                        GetTickCount64(),
                        std::memory_order_relaxed);
                }

                return true;
            }
        }

        if (size == 0 || base > maximum - size)
            break;

        address = base + size;
    }

    return false;
}

static bool ReapplyTrackedRegionIfNeeded()
{
    const std::uintptr_t base =
        g_v181RegionBase.load(std::memory_order_acquire);

    if (base == 0 || g_v181Mint.sharedPointer == 0)
        return false;

    std::int64_t mintBase = 0;
    if (!ReadI64Address(g_v181Mint.quantityBase, mintBase))
        return false;

    // Fast path: if Mint still reads 500000, do a full guarded group check only
    // once per worker cycle below. PatchRealSatchelGroup is idempotent and will
    // write only records that reverted.
    V181PatchStats stats{};
    if (!PatchRealSatchelGroup(
            base,
            g_v181Mint.sharedPointer,
            g_v181Mint,
            stats))
    {
        StoreStats(stats);
        return false;
    }

    if (!PatchMappedAmmoBaseRecords(base, g_v181Mint.sharedPointer))
    {
        StoreStats(stats);
        return false;
    }

    if (stats.writesApplied > 0 ||
        g_v181AmmoWrites.load(std::memory_order_relaxed) > 0)
    {
        ++g_v181ReapplyCount;
    }

    StoreStats(stats);
    return true;
}

static DWORD WINAPI V181BootstrapThread(LPVOID)
{
    g_v181WorkerTick.store(
        GetTickCount64(),
        std::memory_order_relaxed);

    g_v181State.store(
        static_cast<int>(V181BootstrapState::Running),
        std::memory_order_release);

    bool patchedAtLeastOnce = false;

    while (!g_v181Stop.load(std::memory_order_relaxed))
    {
        ++g_v181Attempts;

        if (g_v181ForceRescan.exchange(false, std::memory_order_acq_rel))
        {
            g_v181RegionBase.store(0, std::memory_order_release);
            g_v181Mint = V181MintBlock{};
            {
                std::lock_guard<std::mutex> lock(g_v181SpecialMutex);
                g_v181CardStyleAddresses.clear();
                g_v181WatchAddresses.clear();
            }
        }

        const std::uintptr_t trackedBase =
            g_v181RegionBase.load(std::memory_order_acquire);

        bool ok = false;

        if (trackedBase == 0)
            ok = FindVerifiedRegionAndPatchOnce();
        else
            ok = ReapplyTrackedRegionIfNeeded();

        if (ok)
        {
            patchedAtLeastOnce = true;
            g_v181State.store(
                static_cast<int>(V181BootstrapState::Patched),
                std::memory_order_release);
        }
        else if (trackedBase != 0)
        {
            // Tracked region became stale or failed validation. Re-find it.
            g_v181RegionBase.store(0, std::memory_order_release);
            g_v181Mint = V181MintBlock{};
            g_v181State.store(
                static_cast<int>(V181BootstrapState::Running),
                std::memory_order_release);
        }

        // Early startup still gets frequent checks, but once the region is
        // tracked this is only one guarded 3.8 MiB validation pass per 100 ms.
        Sleep(trackedBase == 0 ? 2u : 100u);
    }

    if (!patchedAtLeastOnce)
    {
        g_v181State.store(
            static_cast<int>(V181BootstrapState::Stopped),
            std::memory_order_release);
    }

    return 0;
}

static int GetPhysicalMintCount()
{
    const Ped player = PLAYER::PLAYER_PED_ID();
    if (player == 0 || !ENTITY::DOES_ENTITY_EXIST(player))
        return -1;

    const int inv = invoke<int>(
        N_GET_INVENTORY_ID_FROM_PED,
        player);

    return invoke<int>(
        N_COUNT_INVENTORY_ITEMS,
        inv,
        static_cast<int>(V181_WILD_MINT),
        true);
}

static int GetReportedMintMax()
{
    return invoke<int>(
        N_GET_ITEM_SLOT_MAX_COUNT,
        static_cast<int>(V181_WILD_MINT),
        static_cast<int>(V181_SLOT_SATCHEL));
}


static int VerifyMappedAmmoBaseNatives(bool logEach)
{
    int passed = 0;

    for (const auto& item : V181_AMMO_MAP_ITEMS)
    {
        const int reported =
            GetItemSlotMax(
                item.hash,
                V181_SLOT_ZERO);

        if (logEach)
        {
            WriteLog(
                "[v2.03 AMMO -1 NATIVE CHECK] " +
                std::string(item.name) +
                " baseMax=" +
                std::to_string(reported));
        }

        if (reported == static_cast<int>(V181_AMMO_TEST_MAX))
            ++passed;
    }

    return passed;
}

// ------------------------------------------------------------
// v1.81 NO-GAP DATABINDING WRITE INTERCEPT
// ------------------------------------------------------------
// v1.78 rewrote Satchel.Selected.Tip after Rockstar had already populated it.
// v1.79 moved to an entry breakpoint on DATABINDING_WRITE_DATA_STRING, but its
// restore/single-step/re-arm cycle left a tiny global gap where another UI
// thread could enter the same hot native untrapped. v1.81 keeps INT3 armed at
// all times and routes the trapped call through a copied first-instruction
// trampoline. There is no temporary unhook window.
//
// The hook does NOT invent the quantity. It extracts the exact current quantity
// from Rockstar's own incoming "Carrying <quantity>..." string and changes only
// the presentation to "Hoarding <quantity>".

static V181GetCommandFromHashFn V181ResolveGetCommandFromHash()
{
    HMODULE scriptHook = GetModuleHandleW(L"ScriptHookRDR2.dll");
    if (scriptHook == nullptr)
        return nullptr;

    FARPROC proc = GetProcAddress(
        scriptHook,
        "?getCommandFromHash@@YAPEAX_K@Z");

    if (proc == nullptr)
        proc = GetProcAddress(scriptHook, "getCommandFromHash");

    return reinterpret_cast<V181GetCommandFromHashFn>(proc);
}

static bool V181ReadCStringSafe(const char* source, char* output, std::size_t capacity)
{
    if (source == nullptr || output == nullptr || capacity < 2)
        return false;

    for (std::size_t i = 0; i + 1 < capacity; ++i)
    {
        char ch = 0;
        SIZE_T bytesRead = 0;
        if (!ReadProcessMemory(
                GetCurrentProcess(),
                source + i,
                &ch,
                sizeof(ch),
                &bytesRead) ||
            bytesRead != sizeof(ch))
        {
            output[0] = '\0';
            return false;
        }

        output[i] = ch;
        if (ch == '\0')
            return true;
    }

    output[capacity - 1] = '\0';
    return true;
}

static bool V181BuildHoardingTextFromRockstarTip(
    const char* source,
    char* output,
    std::size_t outputCapacity)
{
    char input[128]{};
    if (!V181ReadCStringSafe(source, input, sizeof(input)))
        return false;

    static constexpr char kCarrying[] = "Carrying ";
    static constexpr char kHoarding[] = "Hoarding ";

    const std::size_t carryingLen = sizeof(kCarrying) - 1;
    const std::size_t hoardingLen = sizeof(kHoarding) - 1;

    if (std::strncmp(input, kCarrying, carryingLen) != 0)
        return false;

    const char* quantityBegin = input + carryingLen;
    const char* quantityEnd = quantityBegin;

    if (*quantityEnd == '-')
        ++quantityEnd;

    const char* digitStart = quantityEnd;
    while ((*quantityEnd >= '0' && *quantityEnd <= '9') ||
           *quantityEnd == ',')
    {
        ++quantityEnd;
    }

    if (quantityEnd == digitStart)
        return false;

    // Only accept the normal satchel carrying formats: either "Carrying X"
    // or "Carrying X of Y". This prevents unrelated prose beginning with the
    // same word from being modified.
    if (*quantityEnd != '\0' && std::strncmp(quantityEnd, " of ", 4) != 0)
        return false;

    const std::size_t quantityLen =
        static_cast<std::size_t>(quantityEnd - quantityBegin);

    // Group/submenu rows in the Satchel are not real inventory leaves. Some
    // resolve to quantity zero while their children hold the actual items.
    // Never turn those into "Hoarding 0"; leave Rockstar's original Tip alone.
    long long parsedQuantity = 0;
    bool sawDigit = false;
    for (const char* p = quantityBegin; p < quantityEnd; ++p)
    {
        if (*p == ',')
            continue;
        if (*p < '0' || *p > '9')
            return false;
        sawDigit = true;
        parsedQuantity = parsedQuantity * 10 + (*p - '0');
        if (parsedQuantity > 2147483647LL)
            break;
    }

    if (!sawDigit || parsedQuantity <= 0)
        return false;

    if (hoardingLen + quantityLen + 1 > outputCapacity)
        return false;

    std::memcpy(output, kHoarding, hoardingLen);
    std::memcpy(output + hoardingLen, quantityBegin, quantityLen);
    output[hoardingLen + quantityLen] = '\0';
    return true;
}

static std::size_t V181DecodeRelocatableFirstInstruction(
    const BYTE* code,
    std::size_t available)
{
    if (code == nullptr || available < 2)
        return 0;

    std::size_t i = 0;

    // The live 1491.50 handler begins with a REX-prefixed x64 instruction.
    // Accept REX prefixes, but deliberately reject legacy prefixes/two-byte
    // opcodes here. If Rockstar changes the prologue, the hook fails open and
    // logs the bytes instead of guessing.
    if (code[i] >= 0x40 && code[i] <= 0x4F)
        ++i;

    if (i >= available)
        return 0;

    const BYTE op = code[i++];

    // Simple one-byte register push/pop/nop/ret forms.
    if ((op >= 0x50 && op <= 0x5F) || op == 0x90 || op == 0xC3)
        return i;

    bool hasModRm = false;
    std::size_t immediateBytes = 0;

    switch (op)
    {
        case 0x01: case 0x03: case 0x09: case 0x0B:
        case 0x21: case 0x23: case 0x29: case 0x2B:
        case 0x31: case 0x33: case 0x39: case 0x3B:
        case 0x63: case 0x85: case 0x87: case 0x89:
        case 0x8B: case 0x8D: case 0x8F: case 0xFF:
            hasModRm = true;
            break;
        case 0x69:
            hasModRm = true;
            immediateBytes = 4;
            break;
        case 0x6B:
            hasModRm = true;
            immediateBytes = 1;
            break;
        case 0x80: case 0x82: case 0x83:
            hasModRm = true;
            immediateBytes = 1;
            break;
        case 0x81: case 0xC7:
            hasModRm = true;
            immediateBytes = 4;
            break;
        case 0xC6:
            hasModRm = true;
            immediateBytes = 1;
            break;
        default:
            return 0;
    }

    if (!hasModRm || i >= available)
        return 0;

    const BYTE modrm = code[i++];
    const BYTE mod = (modrm >> 6) & 0x3;
    const BYTE rm = modrm & 0x7;

    if (mod != 3 && rm == 4)
    {
        if (i >= available)
            return 0;
        const BYTE sib = code[i++];
        const BYTE base = sib & 0x7;
        if (mod == 0 && base == 5)
        {
            if (i + 4 > available)
                return 0;
            i += 4;
        }
    }
    else if (mod == 0 && rm == 5)
    {
        // RIP-relative memory operand. A raw copy would point at the wrong
        // address from the trampoline, so reject rather than relocate blindly.
        return 0;
    }

    if (mod == 1)
    {
        if (i + 1 > available)
            return 0;
        i += 1;
    }
    else if (mod == 2)
    {
        if (i + 4 > available)
            return 0;
        i += 4;
    }

    if (i + immediateBytes > available)
        return 0;

    i += immediateBytes;
    return i;
}

static std::string V181BytesToHex(const BYTE* bytes, std::size_t count)
{
    std::ostringstream ss;
    ss << std::uppercase << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < count; ++i)
    {
        if (i != 0)
            ss << ' ';
        ss << std::setw(2) << static_cast<unsigned int>(bytes[i]);
    }
    return ss.str();
}

static bool V181BuildTipWriteTrampoline()
{
    if (g_v181TipWriteCommand == nullptr)
        return false;

    std::memcpy(
        g_v181TipWriteOriginalBytes,
        g_v181TipWriteCommand,
        sizeof(g_v181TipWriteOriginalBytes));

    g_v181TipWriteOriginalByte = g_v181TipWriteOriginalBytes[0];
    g_v181TipWriteFirstInstructionLength =
        V181DecodeRelocatableFirstInstruction(
            g_v181TipWriteOriginalBytes,
            sizeof(g_v181TipWriteOriginalBytes));

    if (g_v181TipWriteFirstInstructionLength == 0)
    {
        WriteLog(
            "[v1.81 UI HOOK] FAILED: unsupported first instruction. bytes=" +
            V181BytesToHex(g_v181TipWriteOriginalBytes, 16));
        return false;
    }

    g_v181TipWriteTrampoline = VirtualAlloc(
        nullptr,
        64,
        MEM_COMMIT | MEM_RESERVE,
        PAGE_EXECUTE_READWRITE);

    if (g_v181TipWriteTrampoline == nullptr)
    {
        WriteLog("[v1.81 UI HOOK] FAILED: VirtualAlloc trampoline failed.");
        return false;
    }

    BYTE* trampoline =
        reinterpret_cast<BYTE*>(g_v181TipWriteTrampoline);

    std::memcpy(
        trampoline,
        g_v181TipWriteOriginalBytes,
        g_v181TipWriteFirstInstructionLength);

    // Absolute indirect jump: FF 25 00 00 00 00 [target64]. This preserves
    // all general-purpose registers, unlike a MOV RAX/JMP RAX sequence.
    BYTE* jump = trampoline + g_v181TipWriteFirstInstructionLength;
    jump[0] = 0xFF;
    jump[1] = 0x25;
    jump[2] = 0x00;
    jump[3] = 0x00;
    jump[4] = 0x00;
    jump[5] = 0x00;

    const std::uint64_t target =
        reinterpret_cast<std::uint64_t>(g_v181TipWriteCommand) +
        g_v181TipWriteFirstInstructionLength;
    std::memcpy(jump + 6, &target, sizeof(target));

    FlushInstructionCache(
        GetCurrentProcess(),
        trampoline,
        g_v181TipWriteFirstInstructionLength + 14);

    WriteLog(
        "[v1.81 UI HOOK] no-gap trampoline ready. firstInstructionLength=" +
        std::to_string(g_v181TipWriteFirstInstructionLength) +
        " bytes=" +
        V181BytesToHex(
            g_v181TipWriteOriginalBytes,
            g_v181TipWriteFirstInstructionLength));

    return true;
}

static LONG CALLBACK V181TipWriteVectoredExceptionHandler(
    PEXCEPTION_POINTERS exceptionInfo)
{
    if (exceptionInfo == nullptr ||
        exceptionInfo->ExceptionRecord == nullptr ||
        exceptionInfo->ContextRecord == nullptr)
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    CONTEXT* cpu = exceptionInfo->ContextRecord;
    const DWORD code = exceptionInfo->ExceptionRecord->ExceptionCode;

    if (code != EXCEPTION_BREAKPOINT ||
        !g_v181TipWriteHookInstalled.load(std::memory_order_acquire) ||
        g_v181TipWriteCommand == nullptr ||
        g_v181TipWriteTrampoline == nullptr ||
        exceptionInfo->ExceptionRecord->ExceptionAddress != g_v181TipWriteCommand)
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    auto* nativeContext =
        reinterpret_cast<V181NativeCallContextPrefix*>(cpu->Rcx);

    if (nativeContext != nullptr &&
        nativeContext->arguments != nullptr &&
        nativeContext->argumentCount >= 2)
    {
        const int handle =
            static_cast<int>(nativeContext->arguments[0]);
        const int activeTipHandle =
            g_v181CachedTipHandle.load(std::memory_order_acquire);

        // The exact Tip handle remains the preferred gate. While the Satchel
        // is known open, also accept the extremely specific Rockstar numeric
        // "Carrying X" / "Carrying X of Y" shape. This covers a handle refresh
        // between menu rebuild and watcher discovery without touching general
        // prose or other arbitrary DataBinding strings.
        const bool possibleSatchelTip =
            (activeTipHandle != 0 && handle == activeTipHandle) ||
            g_v181SatchelOpenForHook.load(std::memory_order_acquire);

        if (possibleSatchelTip)
        {
            const char* source = reinterpret_cast<const char*>(
                static_cast<std::uintptr_t>(nativeContext->arguments[1]));

            if (V181BuildHoardingTextFromRockstarTip(
                    source,
                    g_v181TipReplacement,
                    sizeof(g_v181TipReplacement)))
            {
                nativeContext->arguments[1] =
                    static_cast<std::uint64_t>(
                        reinterpret_cast<std::uintptr_t>(g_v181TipReplacement));
                InterlockedIncrement64(&g_v181TipWriteIntercepts);
            }
        }
    }

    // Critical v1.81 change: INT3 NEVER comes off the live native entry.
    // Execute the copied first instruction in our private trampoline and jump
    // straight back to command+instructionLength. Other threads therefore
    // cannot slip through an unhooked one-instruction window.
    cpu->Rip = reinterpret_cast<DWORD64>(g_v181TipWriteTrampoline);
    return EXCEPTION_CONTINUE_EXECUTION;
}

static bool V181InstallImmediateTipWriteHook()
{
    if (g_v181TipWriteHookInstalled.load(std::memory_order_acquire))
        return true;

    const V181GetCommandFromHashFn getCommand =
        V181ResolveGetCommandFromHash();
    if (getCommand == nullptr)
    {
        WriteLog("[v1.81 UI HOOK] FAILED: ScriptHookRDR2 getCommandFromHash export unavailable.");
        return false;
    }

    g_v181TipWriteCommand =
        getCommand(N_DATABINDING_WRITE_DATA_STRING);
    if (g_v181TipWriteCommand == nullptr)
    {
        WriteLog("[v1.81 UI HOOK] FAILED: DATABINDING_WRITE_DATA_STRING command pointer is null.");
        return false;
    }

    if (!V181BuildTipWriteTrampoline())
    {
        g_v181TipWriteCommand = nullptr;
        return false;
    }

    g_v181TipWriteVeh = AddVectoredExceptionHandler(
        1,
        V181TipWriteVectoredExceptionHandler);
    if (g_v181TipWriteVeh == nullptr)
    {
        VirtualFree(g_v181TipWriteTrampoline, 0, MEM_RELEASE);
        g_v181TipWriteTrampoline = nullptr;
        g_v181TipWriteCommand = nullptr;
        WriteLog("[v1.81 UI HOOK] FAILED: AddVectoredExceptionHandler failed.");
        return false;
    }

    if (!VirtualProtect(
            g_v181TipWriteCommand,
            1,
            PAGE_EXECUTE_READWRITE,
            &g_v181TipWriteOldProtect))
    {
        RemoveVectoredExceptionHandler(g_v181TipWriteVeh);
        g_v181TipWriteVeh = nullptr;
        VirtualFree(g_v181TipWriteTrampoline, 0, MEM_RELEASE);
        g_v181TipWriteTrampoline = nullptr;
        g_v181TipWriteCommand = nullptr;
        WriteLog("[v1.81 UI HOOK] FAILED: VirtualProtect could not arm the native handler.");
        return false;
    }

    if (g_v181TipWriteOriginalByte == 0xCC)
    {
        DWORD ignored = 0;
        VirtualProtect(
            g_v181TipWriteCommand,
            1,
            g_v181TipWriteOldProtect,
            &ignored);
        RemoveVectoredExceptionHandler(g_v181TipWriteVeh);
        g_v181TipWriteVeh = nullptr;
        VirtualFree(g_v181TipWriteTrampoline, 0, MEM_RELEASE);
        g_v181TipWriteTrampoline = nullptr;
        g_v181TipWriteCommand = nullptr;
        WriteLog("[v1.81 UI HOOK] FAILED: handler entry is already breakpoint-hooked by another module.");
        return false;
    }

    *reinterpret_cast<BYTE*>(g_v181TipWriteCommand) = 0xCC;
    FlushInstructionCache(
        GetCurrentProcess(),
        g_v181TipWriteCommand,
        1);

    DWORD ignored = 0;
    VirtualProtect(
        g_v181TipWriteCommand,
        1,
        g_v181TipWriteOldProtect,
        &ignored);

    g_v181TipWriteHookInstalled.store(true, std::memory_order_release);

    WriteLog(
        "[v1.81 UI HOOK] ARMED NO-GAP: Satchel Tip writes are rewritten before display. command=" +
        Hex(reinterpret_cast<std::uint64_t>(g_v181TipWriteCommand), 16) +
        " originalByte=" + Hex(g_v181TipWriteOriginalByte, 2));
    return true;
}

static void V181RemoveImmediateTipWriteHook()
{
    g_v181TipWriteHookInstalled.store(false, std::memory_order_release);
    g_v181SatchelOpenForHook.store(false, std::memory_order_release);

    if (g_v181TipWriteCommand != nullptr)
    {
        DWORD oldProtect = 0;
        if (VirtualProtect(
                g_v181TipWriteCommand,
                1,
                PAGE_EXECUTE_READWRITE,
                &oldProtect))
        {
            *reinterpret_cast<BYTE*>(g_v181TipWriteCommand) =
                g_v181TipWriteOriginalByte;
            FlushInstructionCache(
                GetCurrentProcess(),
                g_v181TipWriteCommand,
                1);

            DWORD ignored = 0;
            VirtualProtect(
                g_v181TipWriteCommand,
                1,
                oldProtect,
                &ignored);
        }
    }

    if (g_v181TipWriteVeh != nullptr)
    {
        RemoveVectoredExceptionHandler(g_v181TipWriteVeh);
        g_v181TipWriteVeh = nullptr;
    }

    if (g_v181TipWriteTrampoline != nullptr)
    {
        VirtualFree(g_v181TipWriteTrampoline, 0, MEM_RELEASE);
        g_v181TipWriteTrampoline = nullptr;
    }

    g_v181TipWriteCommand = nullptr;
    g_v181TipWriteFirstInstructionLength = 0;
    g_v181CachedTipHandle.store(0, std::memory_order_release);
}

// ------------------------------------------------------------
// v1.81 SATCHEL LEAF-ITEM "HOARDING X" UI OVERRIDE
// ------------------------------------------------------------
// v1.48 already proved these live DataBinding paths:
//   Satchel.Selected.Name = selected item hash
//   Satchel.Selected.Tip  = the bottom "Carrying X of Y" line
//
// Here we keep Rockstar's real inventory and capacity untouched. The watcher
// only discovers live DataBinding handles; the native hook rewrites only the
// normal positive-quantity Carrying form and leaves all other Tips vanilla.

static int V181GetDataBindingPath(const char* path)
{
    return invoke<int>(
        N_DATABINDING_GET_DATA_CONTAINER_FROM_PATH,
        path);
}

static std::uint32_t V181ReadDataBindingHash(int handle)
{
    if (handle == 0)
        return 0;

    return static_cast<std::uint32_t>(
        invoke<Hash>(N_DATABINDING_READ_HASH, handle));
}

static int V181CountSelectedItem(std::uint32_t itemHash)
{
    if (itemHash == 0)
        return -1;

    const Ped player = PLAYER::PLAYER_PED_ID();
    if (player == 0 || !ENTITY::DOES_ENTITY_EXIST(player))
        return -1;

    const int inventoryId = invoke<int>(
        N_GET_INVENTORY_ID_FROM_PED,
        player);

    if (inventoryId < 0)
        return -1;

    return invoke<int>(
        N_COUNT_INVENTORY_ITEMS,
        inventoryId,
        static_cast<int>(itemHash),
        true);
}

static void V181UpdateSatchelHoardingUi()
{
    const int satchel = V181GetDataBindingPath(V181_SATCHEL_PATH);
    const int selected = V181GetDataBindingPath(V181_SELECTED_PATH);

    if (satchel == 0 || selected == 0)
    {
        if (g_v181SatchelUiOpen)
        {
            WriteLog(
                "[v1.81 UI] Satchel closed. interceptedWrites=" +
                std::to_string(static_cast<long long>(g_v181TipWriteIntercepts)));
        }

        g_v181SatchelUiOpen = false;
        g_v181SatchelOpenForHook.store(false, std::memory_order_release);
        g_v181CachedTipHandle.store(0, std::memory_order_release);
        g_v181LastUiItem = 0;
        g_v181LastUiCount = -999999;
        return;
    }

    if (!g_v181SatchelUiOpen)
    {
        WriteLog(
            "[v1.81 UI] Satchel detected. Satchel=" + Hex(satchel) +
            " Selected=" + Hex(selected));
        g_v181SatchelUiOpen = true;
        g_v181SatchelOpenForHook.store(true, std::memory_order_release);
    }

    const int nameHandle =
        V181GetDataBindingPath(V181_SELECTED_NAME_PATH);
    const int tipHandle =
        V181GetDataBindingPath(V181_SELECTED_TIP_PATH);

    if (nameHandle == 0 || tipHandle == 0)
    {
        if (!g_v181SatchelUiUnavailableLogged)
        {
            WriteLog(
                "[v1.81 UI] Satchel is open but Selected.Name/Selected.Tip "
                "is not ready yet; waiting.");
            g_v181SatchelUiUnavailableLogged = true;
        }
        return;
    }

    g_v181SatchelUiUnavailableLogged = false;
    g_v181SatchelOpenForHook.store(true, std::memory_order_release);
    g_v181CachedTipHandle.store(tipHandle, std::memory_order_release);

    const std::uint32_t selectedItem =
        V181ReadDataBindingHash(nameHandle);
    if (selectedItem == 0)
        return;

    const int count = V181CountSelectedItem(selectedItem);
    const bool changed =
        selectedItem != g_v181LastUiItem || count != g_v181LastUiCount;

    // v1.81 is intentionally READ-ONLY here. Do not write Selected.Tip from
    // the watcher. The no-gap native intercept decides from Rockstar's actual
    // incoming text whether this is a normal stackable leaf. That means:
    //   "Carrying X..." with X > 0 -> "Hoarding X"
    //   "This is a unique item."    -> untouched
    //   submenu/group/header Tips   -> untouched
    //   zero-count pseudo rows      -> untouched
    if (changed)
    {
        WriteLog(
            "[v1.81 UI] selectedItem=" + Hex(selectedItem) +
            " realCount=" + std::to_string(count) +
            " watcher=READ_ONLY" +
            " interceptedWrites=" +
            std::to_string(static_cast<long long>(g_v181TipWriteIntercepts)));
        g_v181LastUiItem = selectedItem;
        g_v181LastUiCount = count;
    }
}

static void MonitorNativeSide()
{
    const ULONGLONG now = GetTickCount64();
    if (now < g_v181NextWatchTick)
        return;

    g_v181NextWatchTick = now + 250u;

    const int count = GetPhysicalMintCount();
    const int max = GetReportedMintMax();

    if (count != g_v181LastMintCount || max != g_v181LastMintMax)
    {
        WriteLog(
            "[v1.81 NATIVE WATCH] physicalWildMint=" +
            std::to_string(count) +
            " reportedMintMax=" +
            std::to_string(max));

        g_v181LastMintCount = count;
        g_v181LastMintMax = max;
    }

    if (!g_v181PersistenceLogged &&
        count > 10 &&
        max >= V181_TARGET_MAX)
    {
        g_v181PersistenceLogged = true;
        WriteLog(
            "[v1.81 PERSISTENCE SUCCESS] Oversized saved Mint survived load "
            "with the full 517-record general SATCHEL pass active.");
    }

    if (!g_v181GeneralSuccessLogged &&
        g_v181State.load(std::memory_order_acquire) ==
            static_cast<int>(V181BootstrapState::Patched) &&
        g_v181StatTotal.load(std::memory_order_relaxed) ==
            V181_EXPECTED_REAL_SATCHEL_RECORDS &&
        g_v181StatPositive.load(std::memory_order_relaxed) ==
            V181_EXPECTED_POSITIVE_RECORDS &&
        max >= V181_TARGET_MAX)
    {
        g_v181GeneralSuccessLogged = true;
        WriteLog(
            "[v1.81 GENERAL SUCCESS] Verified live SATCHEL group=518 records; "
            "517 positive capacities are at least 500000; one negative special "
            "record is untouched.");

        SetScreenMessage(
            "GENERAL SATCHEL 500K PATCH ACTIVE",
            12000u);
    }


    const int specialGeneration = g_v181SpecialGeneration.load(std::memory_order_acquire);
    if (specialGeneration > g_v181PruneAttemptedGeneration)
    {
        PruneFourNonCardDocumentsFromCardBucket();
    }

    const int cardSampleMax = GetItemSlotMax(V181_CARD_VERIFY_SAMPLES[0].hash, V181_SLOT_ZERO);
    const int watchSampleMax = GetItemSlotMax(V181_WATCH_VERIFY_ITEMS[0].hash, V181_SLOT_WATCH);

    if (cardSampleMax != g_v181LastCardSampleMax || watchSampleMax != g_v181LastWatchSampleMax)
    {
        WriteLog("[v1.81 SPECIAL WATCH] sampleCardMax=" + std::to_string(cardSampleMax) +
                 " platinumWatchMax=" + std::to_string(watchSampleMax) +
                 " specialGeneration=" + std::to_string(specialGeneration) +
                 " prunedGeneration=" + std::to_string(g_v181PrunedGeneration));
        g_v181LastCardSampleMax = cardSampleMax;
        g_v181LastWatchSampleMax = watchSampleMax;
    }

    if ((cardSampleMax >= 0 && cardSampleMax < V181_TARGET_MAX) ||
        (watchSampleMax >= 0 && watchSampleMax < V181_TARGET_MAX))
    {
        if (!g_v181RescanRequestedLogged)
        {
            g_v181RescanRequestedLogged = true;
            WriteLog("[v1.81 WATCH] Card/watch special capacity dropped below 500000; requesting fresh region discovery/repatch.");
        }
        g_v181ForceRescan.store(true, std::memory_order_release);
        return;
    }

    // If Rockstar's live native suddenly reports a lower Mint max, the active
    // ItemDatabase moved/rebuilt somewhere else. Ask the memory worker to
    // discard the tracked region and rediscover/repatch immediately.
    if (max >= 0 && max < V181_TARGET_MAX)
    {
        if (!g_v181RescanRequestedLogged)
        {
            g_v181RescanRequestedLogged = true;
            WriteLog(
                "[v1.81 WATCH] Live Mint max dropped below 500000; "
                "requesting fresh early-region discovery/repatch.");
        }

        g_v181ForceRescan.store(true, std::memory_order_release);
    }
    else
    {
        g_v181RescanRequestedLogged = false;
    }
}



// ============================================================
// v2.13 - HWC ULTIMATE OUTLAW SATCHEL FIRST-RUN WELCOME
// ============================================================
// Uses the same proven first-Welcome readiness gates as Ultimate Frontier Stash:
//   1) Eastward Bound (MUD1) completed
//   2) normal player control active
//   3) genuine gameplay movement input detected
//   4) 4-second settling delay before pausing and drawing the Welcome
//
// Additional coordination rule for users running both mods:
// if UltimateFrontierStash.asi is actually loaded and its Welcome has not yet
// been dismissed, wait for its protected DAT to report welcomeSeen=true before
// starting this mod's 4-second settling delay. If Frontier Stash is not loaded,
// proceed normally with no dependency.
// ============================================================

static const fs::path UOS_GetWelcomeStatePath()
{
    return GetModDataDirectory() / L"UltimateOutlawSatchel.dat";
}

// Deliberately opaque fixed binary marker.  This file only records that the
// one-time Welcome was actually dismissed; it contains no inventory data.
static constexpr std::array<unsigned char, 32> UOS_WELCOME_SEEN_BYTES =
{
    0x93,0x17,0xC4,0x6A,0x2F,0xD8,0x51,0xBE,
    0x74,0x0D,0xE3,0x29,0xA6,0x5B,0x8F,0x10,
    0xCC,0x42,0x79,0xB1,0x05,0xEA,0x36,0x98,
    0x6D,0xF0,0x23,0x57,0xAD,0x81,0x14,0xCB
};

static bool UOS_WelcomeSeen()
{
    try
    {
        const fs::path path = UOS_GetWelcomeStatePath();
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
            return false;

        std::array<unsigned char, UOS_WELCOME_SEEN_BYTES.size()> bytes{};
        file.read(
            reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));

        if (file.gcount() != static_cast<std::streamsize>(bytes.size()))
            return false;

        // Reject extra bytes as well, so a random/edited DAT does not silently
        // become a valid Welcome marker.
        char extra = 0;
        if (file.read(&extra, 1))
            return false;

        return bytes == UOS_WELCOME_SEEN_BYTES;
    }
    catch (...)
    {
        return false;
    }
}

static bool UOS_SaveWelcomeSeen()
{
    try
    {
        fs::create_directories(GetModDataDirectory());

        const fs::path finalPath = UOS_GetWelcomeStatePath();
        const fs::path tempPath = finalPath.wstring() + L".tmp";

        {
            std::ofstream file(tempPath, std::ios::binary | std::ios::trunc);
            if (!file.is_open())
                return false;

            file.write(
                reinterpret_cast<const char*>(UOS_WELCOME_SEEN_BYTES.data()),
                static_cast<std::streamsize>(UOS_WELCOME_SEEN_BYTES.size()));
            file.flush();
            if (!file.good())
                return false;
        }

        std::error_code ec;
        fs::remove(finalPath, ec);
        ec.clear();
        fs::rename(tempPath, finalPath, ec);
        if (ec)
        {
            fs::remove(tempPath, ec);
            return false;
        }

        return UOS_WelcomeSeen();
    }
    catch (...)
    {
        return false;
    }
}

static bool UOS_PlayerHasControl()
{
    return invoke<bool>(
        0x7964097FCE4C244BULL, // IS_PLAYER_CONTROL_ON
        PLAYER::PLAYER_ID());
}

static void UOS_WaitForPlayerControlOnly()
{
    // v2.15.24:
    // Ultimate Outlaw Satchel remains valid in Chapter 1, so there is NO
    // MUD1 / Eastward Bound gate and NO Frontier-Welcome coordination.
    //
    // Returning-run Haris/Dusty startup still uses ONLY this Rockstar
    // player-control gate. The first-ever Welcome adds its own proven
    // movement-input readiness gate separately below.
    while (!UOS_PlayerHasControl())
        WAIT(250);
}

static bool UOS_GameplayMovementInputDetected()
{
    // Proven Welcome readiness recipe from Frontier Stash:
    // require genuine locomotion input before allowing a pausing Welcome.
    // Works with controller left stick and keyboard movement.
    constexpr Hash kMoveLR = static_cast<Hash>(0x4D8FB4C1); // INPUT_MOVE_LR
    constexpr Hash kMoveUD = static_cast<Hash>(0xFDA83190); // INPUT_MOVE_UD

    const float moveLR =
        invoke<float>(
            0xEC3C9B8D5327B563ULL, // GET_CONTROL_NORMAL
            0,
            kMoveLR);

    const float moveUD =
        invoke<float>(
            0xEC3C9B8D5327B563ULL, // GET_CONTROL_NORMAL
            0,
            kMoveUD);

    constexpr float kMovementThreshold = 0.20f;

    return
        moveLR > kMovementThreshold ||
        moveLR < -kMovementThreshold ||
        moveUD > kMovementThreshold ||
        moveUD < -kMovementThreshold;
}


static int UOS_CreateContinuePrompt()
{
    constexpr Hash kAccept = 0xC7B5340A; // INPUT_FRONTEND_ACCEPT

    const int prompt = invoke<int>(0x04F97DE45A519419ULL); // _UI_PROMPT_REGISTER_BEGIN
    if (prompt == 0)
        return 0;

    invoke<Any>(0xB5352B7494A08258ULL, prompt, kAccept); // _UI_PROMPT_SET_CONTROL_ACTION
    invoke<Void>(0xCA24F528D0D16289ULL, prompt, 2);      // _UI_PROMPT_SET_PRIORITY

    const char* text = invoke<const char*>(
        0xFA925AC00EB830B9ULL, 10, "LITERAL_STRING", "Continue");
    if (text)
        invoke<Void>(0x5DD02A8318420DD7ULL, prompt, text); // _UI_PROMPT_SET_TEXT

    invoke<Void>(0xCC6656799977741BULL, prompt, true);  // _UI_PROMPT_SET_STANDARD_MODE
    invoke<Void>(0xF7AA2696A22AD8B9ULL, prompt);        // _UI_PROMPT_REGISTER_END
    invoke<Void>(0x8A0FB4D03A630D21ULL, prompt, true);  // _UI_PROMPT_SET_ENABLED
    invoke<Void>(0x71215ACCFDE075EEULL, prompt, true);   // _UI_PROMPT_SET_VISIBLE
    return prompt;
}

static void UOS_DeletePrompt(int prompt)
{
    if (prompt == 0)
        return;

    invoke<Void>(0x8A0FB4D03A630D21ULL, prompt, false);
    invoke<Void>(0x71215ACCFDE075EEULL, prompt, false);
    invoke<Void>(0x00EDE88D4D13CF59ULL, prompt); // _UI_PROMPT_DELETE
}

static void UOS_DrawWelcome()
{
    constexpr const char* kDict = "hwc_uos_welcome";
    constexpr const char* kTexture = "hwc_uos_welcome";

    if (!invoke<BOOL>(0x54D6900929CCF162ULL, kDict))
    {
        invoke<Void>(0xC1BA29DF5631B0F8ULL, kDict, false);
        return;
    }

    // v2.13.3: the corrected DDS has the old baked-in outer border removed.
    // Scale the border-free texture down so the ACTUAL artwork occupies the
    // same on-screen footprint that the artwork itself had in the original image.
    constexpr float spriteH = 0.6250f;
    constexpr float spriteW = 0.6250f;

    invoke<Void>(
        0xC9884ECADE94CB34ULL, // DRAW_SPRITE
        kDict, kTexture,
        0.5000f, 0.5000f,
        spriteW, spriteH,
        0.0f,
        255, 255, 255, 255,
        false);
}

static void UOS_RunFirstEligibleWelcome()
{
    if (UOS_WelcomeSeen())
    {
        WriteLog("[v2.15.24 WELCOME] RETURNING RUN - Satchel Welcome already seen; skipped.");
        return;
    }

    WriteLog(
        "[v2.15.24 WELCOME] FIRST RUN - waiting for normal player control, "
        "then genuine gameplay movement. No Chapter/MUD1 gate. No Frontier coordination.");

    UOS_WaitForPlayerControlOnly();

    WriteLog(
        "[v2.15.24 WELCOME] Player control detected; waiting for genuine movement input.");

    while (!UOS_GameplayMovementInputDetected())
    {
        WAIT(0);
    }

    WriteLog(
        "[v2.15.24 WELCOME] Gameplay movement detected; beginning 4000 ms settling delay.");

    WAIT(4000);

    WriteLog(
        "[v2.15.24 WELCOME] 4000 ms settling delay complete; Welcome may now display.");
    constexpr const char* kDict = "hwc_uos_welcome";
    invoke<Void>(0xC1BA29DF5631B0F8ULL, kDict, false);

    const DWORD loadStart = GetTickCount();
    while (!invoke<BOOL>(0x54D6900929CCF162ULL, kDict))
    {
        WAIT(0);
        invoke<Void>(0xC1BA29DF5631B0F8ULL, kDict, false);

        if ((GetTickCount() - loadStart) >= 10000)
        {
            WriteLog("[v2.15.24 WELCOME] ERROR: hwc_uos_welcome texture dictionary did not load. Welcome state NOT saved.");
            return;
        }
    }

    const int continuePrompt = UOS_CreateContinuePrompt();
    if (continuePrompt == 0)
        WriteLog("[v2.15.24 WELCOME] WARNING: native Continue prompt could not be registered.");
    else
        WriteLog("[v2.15.24 WELCOME] Welcome displayed; waiting for native Continue/Accept input.");

    invoke<Void>(0xFAEC088D28B1DE4AULL, true); // SET_GAME_PAUSED

    // Same protection as Frontier Stash: do not let the input that just closed
    // another UI instantly dismiss this Welcome.
    WAIT(250);

    while (true)
    {
        WAIT(0);
        UOS_DrawWelcome();

        bool accepted = false;
        if (continuePrompt != 0)
        {
            accepted = invoke<BOOL>(
                0xC92AC953F0A982AEULL, // _UI_PROMPT_HAS_STANDARD_MODE_COMPLETED
                continuePrompt,
                0);
        }
        else
        {
            accepted = invoke<BOOL>(
                0x580417101DDB492FULL, // IS_CONTROL_JUST_PRESSED
                0,
                static_cast<int>(0xC7B5340A));
        }

        if (accepted)
            break;
    }

    UOS_DeletePrompt(continuePrompt);
    invoke<Void>(0xFAEC088D28B1DE4AULL, false); // SET_GAME_PAUSED

    if (UOS_SaveWelcomeSeen())
        WriteLog("[v2.15.24 WELCOME] Welcome dismissed; binary UltimateOutlawSatchel.dat marker saved.");
    else
        WriteLog("[v2.15.24 WELCOME] ERROR: Welcome dismissed but binary state marker could not be saved; next launch will retry.");
}


// ============================================================
// v2.15.24 - AUTO FRONTIER DETECTION + KNOWN-GOOD POINTING HARIS
// ============================================================
//
// No prompts and no Frontier DAT reads.
//
// Returning run:
//   - wait only for normal Rockstar player control
//   - if UltimateFrontierStash.asi is NOT loaded:
//         run the proven v2.15.7 solo path unchanged
//         using hwc_haris_solo_startup
//   - if UltimateFrontierStash.asi IS loaded:
//         preload hwc_haris_dusty
//         watch read-only for Frontier's rgp_dusty_startup dictionary
//         begin pointing Haris when Dusty's live texture signal appears
//
// First test should be Frontier ABSENT. That proves merely adding the router
// did not disturb the already-working solo branch.
// ============================================================

static float UOS_StartupEaseOutCubic(float t)
{
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    const float u = 1.0f - t;
    return 1.0f - (u * u * u);
}

static float UOS_StartupEaseInCubic(float t)
{
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t * t * t;
}

// v2.15.24:
// Exact Frontier-Stash-style texture loading architecture.
// There is NO blocking texture loader in this path.
//
// Every normal script frame:
//   HAS_STREAMED_TEXTURE_DICT_LOADED
//   if false -> REQUEST_STREAMED_TEXTURE_DICT -> return immediately
//   if true  -> DRAW_SPRITE
//
// No polling loop. No re-request timer. No WAIT inside a loader helper.
static bool g_uosSoloStartupPending = false;
static bool g_uosSoloStartupRunning = false;
static DWORD g_uosSoloStartupBegin = 0;
static DWORD g_uosSoloDelayBegin = 0;
static bool g_uosSoloFirstRequestLogged = false;
static bool g_uosSoloFirstRequestReturnedLogged = false;
static bool g_uosSoloPostRequestHasPreLogged = false;
static bool g_uosSoloPostRequestHasReturnLogged = false;
static bool g_uosSoloLoadedLogged = false;
static bool g_uosSoloFirstDrawAttemptLogged = false;
static bool g_uosSoloFirstDrawReturnedLogged = false;

// v2.15.24 paired-Haris state. This remains completely dormant unless the
// actual UltimateFrontierStash.asi module is loaded.
static bool g_uosPairedStartupRunning = false;
static bool g_uosPairedDictLoadedLogged = false;
static bool g_uosPairedFirstRequestLogged = false;
static bool g_uosPairedFirstRequestReturnedLogged = false;
static bool g_uosPairedDustySignalLogged = false;
static bool g_uosPairedFirstDrawLogged = false;
static DWORD g_uosPairedWaitBegin = 0;
static DWORD g_uosPairedAnimationBegin = 0;

// v2.15.24: use Frontier Stash's own startup log marker as the paired clock.
// The streamed-texture-loaded signal proved too variable from launch to launch
// to produce frame-tight message synchronization.
static fs::path g_uosFrontierStartupLogPath;
static std::uintmax_t g_uosFrontierStartupLogOffset = 0;
static std::string g_uosFrontierStartupLogCarry;
static bool g_uosFrontierStartupLogSyncArmed = false;

static void UOS_ArmSoloHarisPresentation()
{
    if (g_uosSoloStartupPending || g_uosSoloStartupRunning)
        return;

    g_uosSoloStartupPending = true;

    WriteLog(
        "[v2.15.24 STARTUP] Solo Haris presentation armed. "
        "Frontier Stash per-frame texture pattern selected.");
}

static void UOS_BeginSoloHarisPresentation()
{
    if (!g_uosSoloStartupPending || g_uosSoloStartupRunning)
        return;

    g_uosSoloStartupPending = false;
    g_uosSoloStartupRunning = true;
    g_uosSoloStartupBegin = 0;
    g_uosSoloDelayBegin = GetTickCount();
    g_uosSoloFirstRequestLogged = false;
    g_uosSoloFirstRequestReturnedLogged = false;
    g_uosSoloPostRequestHasPreLogged = false;
    g_uosSoloPostRequestHasReturnLogged = false;
    g_uosSoloLoadedLogged = false;
    g_uosSoloFirstDrawAttemptLogged = false;
    g_uosSoloFirstDrawReturnedLogged = false;

    WriteLog(
        "[v2.15.24 STARTUP] Solo Haris presentation begun; "
        "2000 ms pre-entry delay started.");
}

static void UOS_DrawSoloHarisFrontierExact(float centerY)
{
    constexpr const char* kDict = "hwc_haris_solo_startup";
    constexpr const char* kTexture = "hwc_haris_solo_startup";

    // v2.15.24 diagnostic probe:
    // After the first REQUEST has returned, log immediately BEFORE and AFTER
    // the very next HAS_STREAMED_TEXTURE_DICT_LOADED call. This changes no
    // loading behavior; it only tells us whether the crash occurs before that
    // next frame reaches HAS, inside HAS itself, or later.
    if (g_uosSoloFirstRequestReturnedLogged &&
        !g_uosSoloPostRequestHasPreLogged)
    {
        g_uosSoloPostRequestHasPreLogged = true;
        WriteLog(
            "[v2.15.24 STARTUP] PROBE: next frame reached; about to call "
            "HAS_STREAMED_TEXTURE_DICT_LOADED after the successful request.");
    }

    const BOOL uosHarisLoaded =
        invoke<BOOL>(0x54D6900929CCF162ULL, kDict);

    if (g_uosSoloFirstRequestReturnedLogged &&
        !g_uosSoloPostRequestHasReturnLogged)
    {
        g_uosSoloPostRequestHasReturnLogged = true;
        WriteLog(
            std::string(
                "[v2.15.24 STARTUP] PROBE: post-request HAS call returned ") +
            (uosHarisLoaded ? "TRUE." : "FALSE."));
    }

    // Otherwise preserve the exact Frontier-Stash-style behavior:
    // if not loaded, REQUEST once for this frame and return immediately.
    if (!uosHarisLoaded)
    {
        if (!g_uosSoloFirstRequestLogged)
        {
            g_uosSoloFirstRequestLogged = true;
            WriteLog(
                "[v2.15.24 STARTUP] First per-frame Haris dictionary request: "
                "hwc_haris_solo_startup");
        }

        invoke<Void>(
            0xC1BA29DF5631B0F8ULL, // REQUEST_STREAMED_TEXTURE_DICT
            kDict,
            false);

        if (!g_uosSoloFirstRequestReturnedLogged)
        {
            g_uosSoloFirstRequestReturnedLogged = true;
            WriteLog(
                "[v2.15.24 STARTUP] First Haris dictionary request returned; "
                "draw function exits immediately for this frame.");
        }

        return;
    }

    if (!g_uosSoloLoadedLogged)
    {
        g_uosSoloLoadedLogged = true;
        WriteLog(
            "[v2.15.24 STARTUP] Haris dictionary reports LOADED; "
            "DRAW_SPRITE path is now eligible.");
    }

    // Solo Haris uses the original 1024x1536 portrait startup asset, whose
    // visible non-transparent art fills much more of the texture than the
    // paired 2048x2048 square YTD does.
    //
    // To make SOLO Haris present at the same apparent on-screen size and show
    // roughly the same amount of outfit/body as the working DUO presentation,
    // keep the same stopY but reduce the portrait sprite height.
    constexpr float spriteH = 0.4500f;
    constexpr float imageAspect = 2.0f / 3.0f;
    constexpr float screenAspectCorrection = 9.0f / 16.0f;
    constexpr float spriteW =
        spriteH * imageAspect * screenAspectCorrection;

    if (!g_uosSoloFirstDrawAttemptLogged)
    {
        g_uosSoloFirstDrawAttemptLogged = true;
        WriteLog(
            "[v2.15.24 STARTUP] About to make first Haris DRAW_SPRITE call.");
    }

    invoke<Void>(
        0xC9884ECADE94CB34ULL, // DRAW_SPRITE
        kDict,
        kTexture,
        0.5000f,
        centerY,
        spriteW,
        spriteH,
        0.0f,
        255, 255, 255, 255,
        false);

    if (!g_uosSoloFirstDrawReturnedLogged)
    {
        g_uosSoloFirstDrawReturnedLogged = true;
        WriteLog(
            "[v2.15.24 STARTUP] First Haris DRAW_SPRITE returned successfully.");
    }
}


// v2.15.24:
// Match Frontier Stash's proven startup-message renderer exactly:
// Rockstar $title font, black shadow pass, aged parchment/tan foreground.
static void UOS_DrawStartupTextPass(
    const char* literal,
    float x,
    float y,
    float scaleX,
    float scaleY,
    int r,
    int g,
    int b,
    int a)
{
    std::string tagged = "<FONT FACE='$title'>";
    tagged += literal;
    tagged += "</FONT>";

    const char* inner = invoke<const char*>(
        0xFA925AC00EB830B9ULL,
        10,
        "LITERAL_STRING",
        tagged.c_str());

    if (!inner)
        return;

    const char* text = invoke<const char*>(
        0xFA925AC00EB830B9ULL,
        42,
        "COLOR_STRING",
        0,
        inner);

    if (!text)
        return;

    invoke<Void>(0xA1253A3C870B6843ULL, scaleX, scaleY);
    invoke<Void>(0xBE5261939FBECB8CULL, true);
    invoke<Void>(0x16FA5CE47F184F1EULL, r, g, b, a);
    invoke<Void>(0x16794E044C9EFB58ULL, text, x, y);
    invoke<Void>(0xBE5261939FBECB8CULL, false);
}

static void UOS_DrawStartupTitle(
    const char* literal,
    float x,
    float y,
    float scaleX,
    float scaleY)
{
    // Exact Frontier Stash shadow offset/color.
    UOS_DrawStartupTextPass(
        literal,
        x + 0.00075f,
        y + 0.00110f,
        scaleX,
        scaleY,
        0, 0, 0, 220);

    // Exact Frontier Stash parchment/tan foreground.
    UOS_DrawStartupTextPass(
        literal,
        x,
        y,
        scaleX,
        scaleY,
        205, 184, 145, 255);
}

static void UOS_DrawSoloSuccessMessage(float topLineY)
{
    // Exact Frontier Stash hierarchy, with Haris/Satchel wording.
    constexpr float mainScale = 0.390f;
    constexpr float subScale = 0.325f;

    UOS_DrawStartupTitle(
        "Halfwit Chipmunk's",
        0.5000f,
        topLineY,
        mainScale,
        mainScale);

    UOS_DrawStartupTitle(
        "Ultimate Outlaw Satchel",
        0.5000f,
        topLineY + 0.0280f,
        mainScale,
        mainScale);

    UOS_DrawStartupTitle(
        "Satchel loaded successfully",
        0.5000f,
        topLineY + 0.0580f,
        subScale,
        subScale);
}

static void UOS_DrawPairedSatchelMessage(float topLineY)
{
    // v2.15.24 mockup-matched layout:
    // - Dusty's existing three-line message remains untouched at x=0.5000.
    // - Haris's four-line block sits on Haris's (left) side.
    // - Every Haris line is independently centered on one shared X.
    // - The four-line Haris block is vertically centered against Dusty's
    //   existing three-line block instead of sharing the same line anchors.
    // - The ampersand sits in the visual gap between the two blocks.
    constexpr float mainScale = 0.390f;
    constexpr float ampScale = 0.550f;

    // With the approved $title/mainScale proportions, this places the
    // ampersand approximately midway between Haris's widest right edge
    // and Dusty's existing widest left edge.
    constexpr float satchelBlockX = 0.3650f;
    constexpr float ampersandX = 0.4100f;

    // Dusty's three anchors are topLineY, +0.028, +0.058.
    // Haris uses four equal 0.028-spaced lines. Starting 0.013 higher
    // aligns the vertical center of the four-line block with Dusty's block.
    constexpr float harisTopOffset = -0.0130f;

    UOS_DrawStartupTitle(
        "HWC's",
        satchelBlockX,
        topLineY + harisTopOffset,
        mainScale,
        mainScale);

    UOS_DrawStartupTitle(
        "Ultimate",
        satchelBlockX,
        topLineY + harisTopOffset + 0.0280f,
        mainScale,
        mainScale);

    UOS_DrawStartupTitle(
        "Outlaw",
        satchelBlockX,
        topLineY + harisTopOffset + 0.0560f,
        mainScale,
        mainScale);

    UOS_DrawStartupTitle(
        "Satchel",
        satchelBlockX,
        topLineY + harisTopOffset + 0.0840f,
        mainScale,
        mainScale);

    // Centered vertically between the two complete text blocks.
    UOS_DrawStartupTitle(
        "&",
        ampersandX,
        topLineY + 0.0290f,
        ampScale,
        ampScale);
}

static void UOS_UpdateSoloHarisPresentation()
{
    if (!g_uosSoloStartupRunning)
        return;

    // Same state-machine idea as Frontier Stash: the 2-second delay is
    // tracked across normal frames rather than implemented with WAIT(2000).
    if (g_uosSoloStartupBegin == 0)
    {
        if ((GetTickCount() - g_uosSoloDelayBegin) < 2000u)
            return;

        g_uosSoloStartupBegin = GetTickCount();

        WriteLog(
            "[v2.15.24 STARTUP] 2000 ms pre-entry delay complete; "
            "solo Haris animation window begun.");
    }

    const DWORD elapsed =
        GetTickCount() - g_uosSoloStartupBegin;

    // Exact Frontier Stash presentation timing:
    // Haris:
    //   0-350 ms      slide down
    //   350-1850 ms   hold
    //   1850-2150 ms  slide up
    // Message:
    //   2150-2450 ms  slide down
    //   2450-4950 ms  hold
    //   4950-5250 ms  slide up
    constexpr DWORD slideDownEnd = 350u;
    constexpr DWORD holdEnd = 1850u;
    constexpr DWORD slideUpEnd = 2150u;
    constexpr DWORD messageDownEnd = 2450u;
    constexpr DWORD messageHoldEnd = 4950u;
    constexpr DWORD sequenceEnd = 5250u;

    constexpr float offscreenY = -0.2250f;
    constexpr float stopY = 0.0550f;

    if (elapsed < slideDownEnd)
    {
        const float t =
            UOS_StartupEaseOutCubic(
                static_cast<float>(elapsed) /
                static_cast<float>(slideDownEnd));

        UOS_DrawSoloHarisFrontierExact(
            offscreenY + (stopY - offscreenY) * t);
        return;
    }

    if (elapsed < holdEnd)
    {
        UOS_DrawSoloHarisFrontierExact(stopY);
        return;
    }

    if (elapsed < slideUpEnd)
    {
        const float t =
            UOS_StartupEaseInCubic(
                static_cast<float>(elapsed - holdEnd) /
                static_cast<float>(slideUpEnd - holdEnd));

        UOS_DrawSoloHarisFrontierExact(
            stopY + (offscreenY - stopY) * t);
        return;
    }

    // Match Frontier Stash's exact success-message motion/landing point.
    constexpr float messageOffscreenY = -0.0600f;
    constexpr float messageStopY = 0.0450f;

    if (elapsed < messageDownEnd)
    {
        const float t =
            UOS_StartupEaseOutCubic(
                static_cast<float>(elapsed - slideUpEnd) /
                static_cast<float>(messageDownEnd - slideUpEnd));

        UOS_DrawSoloSuccessMessage(
            messageOffscreenY +
            (messageStopY - messageOffscreenY) * t);
        return;
    }

    if (elapsed < messageHoldEnd)
    {
        UOS_DrawSoloSuccessMessage(messageStopY);
        return;
    }

    if (elapsed < sequenceEnd)
    {
        const float t =
            UOS_StartupEaseInCubic(
                static_cast<float>(elapsed - messageHoldEnd) /
                static_cast<float>(sequenceEnd - messageHoldEnd));

        UOS_DrawSoloSuccessMessage(
            messageStopY +
            (messageOffscreenY - messageStopY) * t);
        return;
    }

    g_uosSoloStartupRunning = false;

    WriteLog(
        "[v2.15.24 STARTUP] Solo Haris + success-message presentation complete.");
}


static fs::path UOS_GetFrontierStartupLogPath()
{
    const HMODULE frontierModule =
        GetModuleHandleW(L"UltimateFrontierStash.asi");

    if (!frontierModule)
        return {};

    wchar_t modulePath[MAX_PATH]{};

    if (GetModuleFileNameW(
            frontierModule,
            modulePath,
            MAX_PATH) == 0)
    {
        return {};
    }

    return
        fs::path(modulePath).parent_path() /
        L"UltimateFrontierStash" /
        L"UltimateFrontierStash.log";
}

static void UOS_ArmFrontierStartupLogSync()
{
    g_uosFrontierStartupLogPath =
        UOS_GetFrontierStartupLogPath();

    g_uosFrontierStartupLogOffset = 0;
    g_uosFrontierStartupLogCarry.clear();
    g_uosFrontierStartupLogSyncArmed = false;

    if (g_uosFrontierStartupLogPath.empty())
    {
        WriteLog(
            "[v2.15.24 STARTUP] Frontier log sync unavailable: "
            "could not resolve Frontier log path.");
        return;
    }

    std::error_code ec;

    if (fs::exists(g_uosFrontierStartupLogPath, ec) && !ec)
    {
        g_uosFrontierStartupLogOffset =
            fs::file_size(g_uosFrontierStartupLogPath, ec);

        if (ec)
            g_uosFrontierStartupLogOffset = 0;
    }

    g_uosFrontierStartupLogSyncArmed = true;

    WriteLog(
        "[v2.15.24 STARTUP] Frontier log sync armed at current EOF; "
        "waiting for Frontier's exact presentation-begin marker.");
}

static bool UOS_PollFrontierStartupBeginMarker()
{
    if (!g_uosFrontierStartupLogSyncArmed ||
        g_uosFrontierStartupLogPath.empty())
    {
        return false;
    }

    std::error_code ec;

    if (!fs::exists(g_uosFrontierStartupLogPath, ec) || ec)
        return false;

    const std::uintmax_t currentSize =
        fs::file_size(g_uosFrontierStartupLogPath, ec);

    if (ec || currentSize <= g_uosFrontierStartupLogOffset)
        return false;

    std::ifstream file(
        g_uosFrontierStartupLogPath,
        std::ios::binary);

    if (!file.is_open())
        return false;

    file.seekg(
        static_cast<std::streamoff>(g_uosFrontierStartupLogOffset),
        std::ios::beg);

    std::string appended(
        (std::istreambuf_iterator<char>(file)),
        std::istreambuf_iterator<char>());

    g_uosFrontierStartupLogOffset = currentSize;

    std::string searchable =
        g_uosFrontierStartupLogCarry + appended;

    constexpr const char* kBeginMarker =
        "[V93.2 STARTUP] 2000 ms pre-entry delay complete; "
        "Dusty presentation begun.";

    if (searchable.find(kBeginMarker) != std::string::npos)
    {
        g_uosFrontierStartupLogCarry.clear();
        return true;
    }

    // Preserve enough tail text to catch a marker split across two reads.
    constexpr std::size_t kCarryLimit = 256;

    if (searchable.size() > kCarryLimit)
    {
        g_uosFrontierStartupLogCarry =
            searchable.substr(searchable.size() - kCarryLimit);
    }
    else
    {
        g_uosFrontierStartupLogCarry = searchable;
    }

    return false;
}

static void UOS_BeginPairedHarisPresentation()
{
    if (g_uosPairedStartupRunning)
        return;

    g_uosPairedStartupRunning = true;
    g_uosPairedDictLoadedLogged = false;
    g_uosPairedFirstRequestLogged = false;
    g_uosPairedFirstRequestReturnedLogged = false;
    g_uosPairedDustySignalLogged = false;
    g_uosPairedFirstDrawLogged = false;
    g_uosPairedWaitBegin = GetTickCount();
    g_uosPairedAnimationBegin = 0;
    UOS_ArmFrontierStartupLogSync();

    WriteLog(
        "[v2.15.24 STARTUP] Frontier Stash detected. "
        "Pointing Haris path armed with proven known-good hwc_haris_dusty resource.");
}

static void UOS_DrawPairedHarisOriginalStartup(float centerY)
{
    constexpr const char* kDict = "hwc_haris_dusty";
    constexpr const char* kTexture = "hwc_haris_dusty";

    // Proven known-good hwc_haris_dusty YTD is 2048x2048 (square).
    constexpr float spriteH = 0.6000f;
    constexpr float imageAspect = 1.0f;
    constexpr float screenAspectCorrection = 9.0f / 16.0f;
    constexpr float spriteW =
        spriteH * imageAspect * screenAspectCorrection;

    invoke<Void>(
        0xC9884ECADE94CB34ULL, // DRAW_SPRITE
        kDict,
        kTexture,
        // Close roughly half of the original visible gap to Dusty.
        // This is intentionally only a small nudge from the approved 0.2750 position.
        0.3050f,
        centerY,
        spriteW,
        spriteH,
        0.0f,
        255, 255, 255, 255,
        false);

    if (!g_uosPairedFirstDrawLogged)
    {
        g_uosPairedFirstDrawLogged = true;
        WriteLog(
            "[v2.15.24 STARTUP] First pointing-Haris DRAW_SPRITE returned successfully.");
    }
}

static void UOS_UpdatePairedHarisPresentation()
{
    if (!g_uosPairedStartupRunning)
        return;

    constexpr const char* kHarisDict = "hwc_haris_dusty";
    // Load Haris ourselves. Never request Dusty's resource; Frontier owns it.
    const BOOL harisLoaded =
        invoke<BOOL>(0x54D6900929CCF162ULL, kHarisDict);

    if (!harisLoaded)
    {
        if (!g_uosPairedFirstRequestLogged)
        {
            g_uosPairedFirstRequestLogged = true;
            WriteLog(
                "[v2.15.24 STARTUP] First pointing-Haris dictionary request: "
                "hwc_haris_dusty");
        }

        invoke<Void>(
            0xC1BA29DF5631B0F8ULL, // REQUEST_STREAMED_TEXTURE_DICT
            kHarisDict,
            false);

        if (!g_uosPairedFirstRequestReturnedLogged)
        {
            g_uosPairedFirstRequestReturnedLogged = true;
            WriteLog(
                "[v2.15.24 STARTUP] First pointing-Haris dictionary request returned.");
        }

        return;
    }

    if (!g_uosPairedDictLoadedLogged)
    {
        g_uosPairedDictLoadedLogged = true;
        WriteLog(
            "[v2.15.24 STARTUP] Pointing Haris dictionary reports LOADED. "
            "Waiting read-only for Frontier's rgp_dusty_startup live signal.");
    }

    // v2.15.24 synchronization:
    // Frontier itself logs the exact frame where it sets g_v90StartupBegin.
    // Use that marker instead of guessing from texture-load completion.
    if (g_uosPairedAnimationBegin == 0)
    {
        if (UOS_PollFrontierStartupBeginMarker())
        {
            g_uosPairedDustySignalLogged = true;
            g_uosPairedAnimationBegin = GetTickCount();

            WriteLog(
                "[v2.15.24 STARTUP] Frontier presentation-begin log marker detected; "
                "paired Haris/message clock synchronized to Frontier's real startup.");
        }
        else
        {
            // Preserve the established fail-open behavior: if no real Frontier
            // presentation is observed within eight seconds, show solo Haris.
            if ((GetTickCount() - g_uosPairedWaitBegin) >= 8000u)
            {
                WriteLog(
                    "[v2.15.24 STARTUP] Frontier presentation-begin log marker "
                    "not seen within 8 seconds; falling back to proven solo Haris.");

                g_uosPairedStartupRunning = false;
                UOS_ArmSoloHarisPresentation();
                UOS_BeginSoloHarisPresentation();
            }

            return;
        }
    }

    const DWORD elapsed =
        GetTickCount() - g_uosPairedAnimationBegin;

    // Mirror Frontier Stash's exact character/message timing so the Satchel
    // title block arrives with Dusty's existing success message.
    constexpr DWORD slideDownEnd = 350u;
    constexpr DWORD holdEnd = 1850u;
    constexpr DWORD slideUpEnd = 2150u;
    constexpr DWORD messageDownEnd = 2450u;
    constexpr DWORD messageHoldEnd = 4950u;
    constexpr DWORD sequenceEnd = 5250u;

    constexpr float offscreenY = -0.2250f;
    constexpr float stopY = 0.0300f;

    if (elapsed < slideDownEnd)
    {
        const float t =
            UOS_StartupEaseOutCubic(
                static_cast<float>(elapsed) /
                static_cast<float>(slideDownEnd));

        UOS_DrawPairedHarisOriginalStartup(
            offscreenY + (stopY - offscreenY) * t);
        return;
    }

    if (elapsed < holdEnd)
    {
        UOS_DrawPairedHarisOriginalStartup(stopY);
        return;
    }

    if (elapsed < slideUpEnd)
    {
        const float t =
            UOS_StartupEaseInCubic(
                static_cast<float>(elapsed - holdEnd) /
                static_cast<float>(slideUpEnd - holdEnd));

        UOS_DrawPairedHarisOriginalStartup(
            stopY + (offscreenY - stopY) * t);
        return;
    }

    constexpr float messageOffscreenY = -0.0600f;
    constexpr float messageStopY = 0.0450f;

    if (elapsed < messageDownEnd)
    {
        const float t =
            UOS_StartupEaseOutCubic(
                static_cast<float>(elapsed - slideUpEnd) /
                static_cast<float>(messageDownEnd - slideUpEnd));

        UOS_DrawPairedSatchelMessage(
            messageOffscreenY +
            (messageStopY - messageOffscreenY) * t);
        return;
    }

    if (elapsed < messageHoldEnd)
    {
        UOS_DrawPairedSatchelMessage(messageStopY);
        return;
    }

    if (elapsed < sequenceEnd)
    {
        const float t =
            UOS_StartupEaseInCubic(
                static_cast<float>(elapsed - messageHoldEnd) /
                static_cast<float>(sequenceEnd - messageHoldEnd));

        UOS_DrawPairedSatchelMessage(
            messageStopY +
            (messageOffscreenY - messageStopY) * t);
        return;
    }

    g_uosPairedStartupRunning = false;

    WriteLog(
        "[v2.15.24 STARTUP] Pointing Haris + paired Satchel message presentation complete.");
}


void ScriptMain()
{
    g_v181ScriptMainTick = GetTickCount64();

    MigrateLegacySatchelDataNamesIfNeeded();
    ResetLog();

    WriteLog("============================================================");
    WriteLog("[v2.15.24] HWC Ultimate Outlaw Satchel - SOLO + DUO STARTUP MESSAGES");
    WriteLog("[v2.15.24 STARTUP] Frontier absent -> solo Haris then Halfwit Chipmunk's success message; Frontier present -> paired Haris then HWC's Satchel block synchronized with Dusty's existing message.");
    WriteLog("[v2.15.24 WELCOME] First-run Welcome waits for player control + genuine movement, then 4000 ms.");
    WriteLog("[v2.12] NO F7. No catalog profile. Captures Rockstar\'s live item-table descriptor and patches resolved multiplicities.");
    WriteLog("[v2.12] General target: every positive live SLOTID_SATCHEL multiplicity -> at least 500000; zero/negative SATCHEL values untouched.");
    WriteLog("[v2.12] Cigarette cards: exact 144 card item identities on SLOTID_NONE/ZERO -> at least 500000.");
    WriteLog("[v2.12] Stackable watches: exact 4 known loot-watch identities on SLOTID_WATCH -> at least 500000; personal watch untouched.");
    WriteLog("[v2.12] Ammo target: all 43 mapped AMMO item identities, base SLOTID_NONE/ZERO record -> -1 regardless of catalog starting value.");
    WriteLog("[v2.12] Separate ammo upgrade/contribution records remain untouched.");
    WriteLog("[v2.12] Satchel UI retained: ordinary positive-quantity leaf items -> Hoarding <Rockstar current quantity>.");
    WriteLog("[v2.12] Weapon wheel retained: Rockstar current-ammo display; -1 suppresses slash/max.");
    WriteLog("[v2.12] No custom ammo text overlay.");
    WriteLog("[v2.12] No replacement catalog, no per-mod catalog profile, no process-wide ItemDatabase scan.");
    WriteLog("[v2.12] Log: UltimateOutlawSatchel\\UltimateOutlawSatchel.log");
    WriteLog("============================================================");

    const bool immediateTipHook = V181InstallImmediateTipWriteHook();
    WriteLog(
        std::string("[v1.81 UI HOOK] startup result=") +
        (immediateTipHook ? "ARMED" : "NOT-ARMED"));

    const ULONGLONG workerTick =
        g_v181WorkerTick.load(std::memory_order_relaxed);
    const ULONGLONG poolTick =
        g_v181PoolSeenTick.load(std::memory_order_relaxed);
    const ULONGLONG patchTick =
        g_v181PatchTick.load(std::memory_order_relaxed);

    WriteLog(
        "[v1.81 TIMING] ScriptMain started " +
        std::to_string(g_v181ScriptMainTick - g_v181DllAttachTick) +
        " ms after DLL_PROCESS_ATTACH.");

    WriteLog(
        "[v1.81 TIMING] bootstrapStateAtScriptMain=" +
        std::to_string(g_v181State.load(std::memory_order_acquire)) +
        " (0=not-started,1=running,2=patched,3=stopped)");

    WriteLog(
        "[v1.81 TIMING] bootstrapAttempts=" +
        std::to_string(g_v181Attempts.load(std::memory_order_relaxed)) +
        " exactSizeCandidateVisits=" +
        std::to_string(g_v181ExactSizeVisits.load(std::memory_order_relaxed)));

    if (workerTick != 0)
    {
        WriteLog(
            "[v1.81 TIMING] bootstrap worker began " +
            std::to_string(workerTick - g_v181DllAttachTick) +
            " ms after DLL attach.");
    }

    if (poolTick != 0)
    {
        WriteLog(
            "[v1.81 TIMING] verified ItemDatabase region appeared " +
            std::to_string(poolTick - g_v181DllAttachTick) +
            " ms after DLL attach.");
    }

    if (patchTick != 0)
    {
        WriteLog(
            "[v1.81 TIMING] general 517-record 500000 pass first verified " +
            std::to_string(patchTick - g_v181DllAttachTick) +
            " ms after DLL attach (" +
            std::to_string(
                static_cast<long long>(g_v181ScriptMainTick) -
                static_cast<long long>(patchTick)) +
            " ms before ScriptMain).");
    }

    WriteLog(
        "[v1.81 EARLY STATS] realSatchelRecords=" +
        std::to_string(g_v181StatTotal.load(std::memory_order_relaxed)) +
        " positive=" +
        std::to_string(g_v181StatPositive.load(std::memory_order_relaxed)) +
        " negative=" +
        std::to_string(g_v181StatNegative.load(std::memory_order_relaxed)) +
        " zero=" +
        std::to_string(g_v181StatZero.load(std::memory_order_relaxed)) +
        " mintOrdinal=" +
        std::to_string(g_v181StatMintOrdinal.load(std::memory_order_relaxed)) +
        " writesOnLastPass=" +
        std::to_string(g_v181StatWrites.load(std::memory_order_relaxed)) +
        " alreadyAtTarget=" +
        std::to_string(g_v181StatAlready.load(std::memory_order_relaxed)) +
        " aboveTargetPreserved=" +
        std::to_string(g_v181StatAbove.load(std::memory_order_relaxed)));


    WriteLog(
        "[v1.81 SPECIAL EARLY STATS] cardStyleRecords=" +
        std::to_string(g_v181SpecialCardStyleRecords.load(std::memory_order_relaxed)) +
        " cardStyleWrites=" +
        std::to_string(g_v181SpecialCardStyleWrites.load(std::memory_order_relaxed)) +
        " watchRecords=" +
        std::to_string(g_v181SpecialWatchRecords.load(std::memory_order_relaxed)) +
        " watchWrites=" +
        std::to_string(g_v181SpecialWatchWrites.load(std::memory_order_relaxed)) +
        " generation=" +
        std::to_string(g_v181SpecialGeneration.load(std::memory_order_acquire)));

    WriteLog(
        "[v1.81 AMMO EARLY STATS] mappedRecords=" +
        std::to_string(g_v181AmmoRecords.load(std::memory_order_relaxed)) +
        " writesOnLastPass=" +
        std::to_string(g_v181AmmoWrites.load(std::memory_order_relaxed)) +
        " alreadyAtTarget=" +
        std::to_string(g_v181AmmoAlreadyAtTarget.load(std::memory_order_relaxed)) +
        " aboveTargetPreserved=" +
        std::to_string(g_v181AmmoAboveTargetPreserved.load(std::memory_order_relaxed)) +
        " validationFailures=" +
        std::to_string(g_v181AmmoValidationFailures.load(std::memory_order_relaxed)));

    WriteLog(
        "[v1.81 REGION] base=" +
        Hex(g_v181RegionBase.load(std::memory_order_acquire), 16) +
        " sharedPointer=" +
        Hex(g_v181Mint.sharedPointer, 16) +
        " reapplyCount=" +
        std::to_string(g_v181ReapplyCount.load(std::memory_order_relaxed)));

    const bool v212DirectPatch = V212EnsurePatchAtScriptMain();

    // The universal resolver supersedes the old fixed-region worker once it
    // succeeds. Stop any legacy scan/reapply activity; the UI hook is separate.
    if (v212DirectPatch)
        g_v181Stop.store(true, std::memory_order_release);

    V212LogUniversalStats();

    const int initialCount = GetPhysicalMintCount();
    const int initialMax = GetReportedMintMax();

    g_v181LastMintCount = initialCount;
    g_v181LastMintMax = initialMax;

    WriteLog(
        "[v1.81 FIRST NATIVE CHECK] physicalWildMint=" +
        std::to_string(initialCount) +
        " reportedMintMax=" +
        std::to_string(initialMax));


    // Native calls are now available, so remove the four unrelated q5 DOCUMENT
    // records from the temporary early card bucket before normal gameplay begins.
    if (g_v181SpecialGeneration.load(std::memory_order_acquire) > 0)
        PruneFourNonCardDocumentsFromCardBucket();

    const int cardSampleMax = GetItemSlotMax(V181_CARD_VERIFY_SAMPLES[0].hash, V181_SLOT_ZERO);
    const int platinumWatchMax = GetItemSlotMax(V181_WATCH_VERIFY_ITEMS[0].hash, V181_SLOT_WATCH);
    const int personalWatchMax = GetItemSlotMax(V181_PERSONAL_WATCH, V181_SLOT_WATCH);

    g_v181LastCardSampleMax = cardSampleMax;
    g_v181LastWatchSampleMax = platinumWatchMax;

    WriteLog("[v1.81 SPECIAL NATIVE CHECK] sampleCardMax=" + std::to_string(cardSampleMax) +
             " platinumWatchMax=" + std::to_string(platinumWatchMax) +
             " personalUniqueWatchMax=" + std::to_string(personalWatchMax) +
             " pruneSucceeded=" + std::string(g_v181SpecialPruneSucceeded ? "YES" : "NO"));

    const int ammoNativePasses = VerifyMappedAmmoBaseNatives(true);

    WriteLog(
        "[v2.12 AMMO -1 VERIFY SUMMARY] baseRecordsExactlyMinusOne=" +
        std::to_string(ammoNativePasses) +
        "/" +
        std::to_string(V181_AMMO_MAP_ITEMS.size()));

    if (initialCount > 10 && initialMax >= V181_TARGET_MAX)
    {
        g_v181PersistenceLogged = true;
        WriteLog(
            "[v1.81 PERSISTENCE SUCCESS] Oversized saved Mint survived load "
            "under the broad 517-record patch.");
    }

    const bool v212NativeSuccess =
        v212DirectPatch &&
        g_v212CardsFound.load(std::memory_order_relaxed) == 144 &&
        g_v212WatchesFound.load(std::memory_order_relaxed) == 4 &&
        g_v212AmmoFound.load(std::memory_order_relaxed) == 43 &&
        g_v212DirectVerifyFailures.load(std::memory_order_relaxed) == 0 &&
        initialMax >= V181_TARGET_MAX &&
        cardSampleMax >= V181_TARGET_MAX &&
        platinumWatchMax >= V181_TARGET_MAX &&
        personalWatchMax == 1 &&
        ammoNativePasses == static_cast<int>(V181_AMMO_MAP_ITEMS.size());

    g_v212NativeVerified.store(
        v212NativeSuccess,
        std::memory_order_release);

    WriteLog(
        std::string("[v2.12 UNIVERSAL NATIVE VERIFY] result=") +
        (v212NativeSuccess ? "SUCCESS" : "FAILED") +
        " Mint=" + std::to_string(initialMax) +
        " cardSample=" + std::to_string(cardSampleMax) +
        " stackableWatch=" + std::to_string(platinumWatchMax) +
        " personalWatch=" + std::to_string(personalWatchMax) +
        " ammo=" + std::to_string(ammoNativePasses) + "/43");

    if (v212NativeSuccess)
    {
        g_v181GeneralSuccessLogged = true;
        WriteLog(
            "[v2.12 UNIVERSAL SUCCESS] Rockstar's live item table was resolved directly; "
            "all positive SATCHEL capacities are >=500000, all 144 cigarette cards and "
            "4 stackable watches are >=500000, and all 43 ammo base capacities are -1. "
            "The catalog's original numeric capacities were never trusted.");

        // v2.14.5: release cleanup - no on-screen v2.12 catalog-bypass notice.
    }
    else if (g_v181State.load(std::memory_order_acquire) ==
            static_cast<int>(V181BootstrapState::Patched) &&
        g_v181StatTotal.load(std::memory_order_relaxed) ==
            V181_EXPECTED_REAL_SATCHEL_RECORDS &&
        g_v181StatPositive.load(std::memory_order_relaxed) ==
            V181_EXPECTED_POSITIVE_RECORDS &&
        initialMax >= V181_TARGET_MAX &&
        cardSampleMax >= V181_TARGET_MAX &&
        platinumWatchMax >= V181_TARGET_MAX &&
        g_v181SpecialPruneSucceeded &&
        ammoNativePasses == static_cast<int>(V181_AMMO_MAP_ITEMS.size()))
    {
        g_v181GeneralSuccessLogged = true;
        WriteLog(
            "[v1.81 GENERAL SUCCESS] 517 positive normal SATCHEL capacities are >=500000; "
            "144 cigarette-card records are targeted at 500000; 4 stackable watch records are 500000; "
            "all 43 mapped AMMO base capacities report exactly -1; "
            "the single negative SATCHEL record and personal/unique watch records remain untouched.");

        SetScreenMessage(
            "ULTIMATE TRAVELER'S SATCHEL READY",
            12000u);
    }
    else
    {
        SetScreenMessage(
            "500K CAPACITY PATCH - CHECK LOG",
            12000u);
    }

    g_v181NextWatchTick = GetTickCount64();
    g_v181LastUiTick = GetTickCount64();

    // Record whether this launch was already a returning run BEFORE the
    // first-run Welcome function is allowed to create its marker.
    // This prevents Haris from appearing immediately after the Welcome is
    // dismissed on a genuine first-ever run.
    const bool uosWasReturningRun = UOS_WelcomeSeen();

    // v2.13 first-run Welcome runs only after the existing v2.12 core has
    // initialized/verified. Returning runs skip this immediately.
    UOS_RunFirstEligibleWelcome();

    // v2.15.24: first-ever runs stop after the Welcome.
    // Returning runs wait only for Rockstar player control, then automatically
    // choose solo or Dusty-paired Haris from the actual Frontier ASI module.
    if (uosWasReturningRun)
    {
        WriteLog(
            "[v2.15.24 STARTUP] RETURNING RUN - waiting only for normal player control.");

        UOS_WaitForPlayerControlOnly();

        WriteLog(
            "[v2.15.24 STARTUP] Player control detected; checking Frontier Stash module.");

        HMODULE frontierModule =
            GetModuleHandleW(L"UltimateFrontierStash.asi");

        if (frontierModule == nullptr)
        {
            WriteLog(
                "[v2.15.24 STARTUP] UltimateFrontierStash.asi NOT loaded; "
                "using proven solo Haris path unchanged.");

            UOS_ArmSoloHarisPresentation();
            UOS_BeginSoloHarisPresentation();
        }
        else
        {
            WriteLog(
                "[v2.15.24 STARTUP] UltimateFrontierStash.asi loaded; "
                "using pointing Haris + Dusty synchronization path.");

            UOS_BeginPairedHarisPresentation();
        }
    }

    while (true)
    {
        MonitorNativeSide();

        // v2.15.24: both Haris paths update from the normal ScriptMain frame
        // loop. Only the branch selected by the automatic router is active.
        UOS_UpdateSoloHarisPresentation();
        UOS_UpdatePairedHarisPresentation();

        // Run the lightweight read-only path/selection watcher every rendered frame.
        // The native intercept alone is allowed to alter the visible Tip.
        V181UpdateSatchelHoardingUi();
        g_v181LastUiTick = GetTickCount64();

        DrawScreenMessage();
        WAIT(0);
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_module = hModule;
        g_v181DllAttachTick = GetTickCount64();

        DisableThreadLibraryCalls(hModule);

        scriptRegister(hModule, ScriptMain);

        g_v181Stop.store(false, std::memory_order_relaxed);
        g_v181Thread = CreateThread(
            nullptr,
            0,
            V212UniversalBootstrapThread,
            nullptr,
            0,
            nullptr);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        g_v181Stop.store(true, std::memory_order_relaxed);

        if (g_v181Thread)
        {
            CloseHandle(g_v181Thread);
            g_v181Thread = nullptr;
        }

        V212RemoveLookupCaptureHook();
        V181RemoveImmediateTipWriteHook();
        scriptUnregister(hModule);
    }

    return TRUE;
}