/*
 * SigmaCore
 * Copyright (c) 2026 David Boarman (BadKraft) and contributors
 * QuantumOverride [Q|]
 * ----------------------------------------------
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 * ----------------------------------------------
 * File: slab_manager.c
 * Description: SigmaCore slab manager implementation
 */

#include "internal/slab_manager.h"
// ----------------
#include <sigma.collections/parray.h>
#include <sigma.collections/slotarray.h>
#include <string.h>

// define slab slot base parray
// declare the Slab PointerArray struct
typedef struct sc_slab_array {
    char handle[2];  // {'P', '\0'} - type identifier
    addr *bucket;    // pointer to first element (array of addr)
    addr end;        // one past allocated memory (as raw addr)
} sc_slab_array;
// define the slab slot array variable
static sc_slab_array slab_array = {
    .handle = "P\0",
    .bucket = NULL,
    .end = 0,
};
// slab slotarray handle
static slotarray slab_slots = NULL;

#if 1  // Region: Utility Declarations
slotarray init_slab_slot_array(parray);
#endif

#if 1  // Region: Slab Manager Methods
bool slab_manager_init_slab_array(void) {
    bool success = false;
    slab_array.bucket = (addr *)Memory.get_slots_base();
    slab_array.end = Memory.get_slots_end();

    // zero the slots memory to ensure all slots start as ADDR_EMPTY
    memset(slab_array.bucket, 0, SYS0_SLOTS_SIZE);

    // is there validation here?
    slab_slots = init_slab_slot_array((parray)&slab_array);
    if (slab_slots == NULL) {
        goto exit;
    }
    success = true;

exit:
    return success;
}
addr slab_manager_get_slab_slot(usize slot_index) {
    if (slab_slots == NULL || slot_index >= 16) {
        return ADDR_EMPTY;
    }
    void *value;
    if (SlotArray.get_at(slab_slots, slot_index, &value) != OK) {
        return ADDR_EMPTY;
    }
    return (addr)value;
}
bool slab_manager_set_slab_slot(usize slot_index, addr value) {
    if (slab_slots == NULL || slot_index >= 16) {
        return false;
    }
    // Use PArray.set() on the underlying array
    // Note: SlotArray doesn't have set_at, so we use PArray directly
    return PArray.set((parray)&slab_array, slot_index, value) == OK;
}
#endif

// initialize slab slot array in SYS0
#if 1  // Region: Internal Slab Manager Interface
const sc_slab_manager_i SlabManager = {
    .init_slab_array = slab_manager_init_slab_array,
    .get_slab_slot = slab_manager_get_slab_slot,
    .set_slab_slot = slab_manager_set_slab_slot,
};
#endif

#if 1  // Region: Utility Definitions
slotarray init_slab_slot_array(parray array) {
    slotarray slots = NULL;
    if (array == NULL) {
        goto exit;
    }
    slots = PArray.as_slotarray(array);
exit:
    return slots;
}
#endif