/*

#include "gtest/gtest.h"
#include "analyze.h" // Your new analysis header
#include <capstone/capstone.h>
#include <vector>
#include <algorithm>

// --- Test Suite for analyze_mips_func ---

// Test Case 1: A simple, linear function with no branches.
//
// void simple_func() {
//   $a0 = $a0 + 1;
//   $a0 = $a0 + 1;
//   return;
// }
TEST(Analysis, SimpleLinearFunction) {
    // MIPS machine code for the function above
    const std::vector<uint8_t> code = {
        0x24, 0x84, 0x00, 0x01, // 0x00: addiu $a0, $a0, 1
        0x24, 0x84, 0x00, 0x01, // 0x04: addiu $a0, $a0, 1
        0x03, 0xe0, 0x00, 0x08, // 0x08: jr $ra
        0x00, 0x00, 0x00, 0x00  // 0x0C: nop (delay slot)
    };

    // Setup Capstone
    csh handle;
    ASSERT_EQ(cs_open(CS_ARCH_MIPS, (cs_mode)(CS_MODE_MIPS32 | CS_MODE_BIG_ENDIAN), &handle), CS_ERR_OK);
    cs_option(handle, CS_OPT_DETAIL, CS_OPT_ON);

    // Run the analysis
    Function result = analyze_mips_func(handle, code.data(), code.size(), 0x1000);

    // --- Assertions ---
    // We expect exactly 1 basic block.
    ASSERT_EQ(result.blocks.size(), 1);

    const auto& block0 = result.blocks[0];
    // The block should start at offset 0.
    EXPECT_EQ(block0.base, 0);
    // The block should contain all 4 instructions (4 * 4 = 16 bytes).
    EXPECT_EQ(block0.size, 16);
    // The total function size should also be 16.
    EXPECT_EQ(result.size, 16);

    cs_close(&handle);
}

// Test Case 2: A function with a simple forward conditional branch (if).
//
// void if_func(int a0) {
//   if (a0 == 0) {
//     $v0 = 1;
//   }
//   return;
// }
TEST(Analysis, ForwardBranch) {
    // MIPS machine code for the function above
    const std::vector<uint8_t> code = {
        0x10, 0x80, 0x00, 0x03, // 0x00: beq $a0, $zero, +8 (label_if_body)
        0x00, 0x00, 0x00, 0x00, // 0x04: nop (delay slot for beq)

        0x03, 0xe0, 0x00, 0x08, // 0x08: jr $ra (fall-through path)
        0x00, 0x00, 0x00, 0x00, // 0x0C: nop (delay slot for jr)
    
        0x24, 0x02, 0x00, 0x01, // 0x10: label_if_body: addiu $v0, $zero, 1
        0x03, 0xe0, 0x00, 0x08, // 0x14: jr $ra
        0x00, 0x00, 0x00, 0x00  // 0x18: nop (delay slot for jr)

    };

    csh handle;
    ASSERT_EQ(cs_open(CS_ARCH_MIPS, (cs_mode)(CS_MODE_MIPS32 | CS_MODE_BIG_ENDIAN), &handle), CS_ERR_OK);
    cs_option(handle, CS_OPT_DETAIL, CS_OPT_ON);

    Function result = analyze_mips_func(handle, code.data(), code.size(), 0x1000);

    // --- Assertions ---
    // We expect 3 basic blocks:
    // 1. The initial `beq` and its delay slot.
    // 2. The "fall-through" path that just returns.
    // 3. The "if body" that is branched to.
    ASSERT_EQ(result.blocks.size(), 3);
    
    // Note: The order of blocks discovered might differ depending on stack implementation,
    // so we sort them by base address to have a predictable order for testing.
    std::sort(result.blocks.begin(), result.blocks.end(), [](const Function::Block& a, const Function::Block& b) {
        return a.base < b.base;
    });

    // Block 0: The initial branch
    EXPECT_EQ(result.blocks[0].base, 0x00);
    EXPECT_EQ(result.blocks[0].size, 8); // beq + nop

    // Block 1: The fall-through return
    EXPECT_EQ(result.blocks[1].base, 0x08);
    EXPECT_EQ(result.blocks[1].size, 8); // jr + nop

    // Block 2: The if-body
    EXPECT_EQ(result.blocks[2].base, 0x10);
    EXPECT_EQ(result.blocks[2].size, 12); // addiu + jr + nop

    EXPECT_EQ(result.size, 28); // Furthest point is 0x10 + 12 = 28

    cs_close(&handle);
}

// Test Case 3: A function with a backward branch (a simple loop).
//
// void loop_func() {
//   $v0 = 5;
// L1:
//   $v0 = $v0 - 1;
//   if ($v0 != 0) goto L1;
//   return;
// }
TEST(Analysis, BackwardBranchLoop) {
    const std::vector<uint8_t> code = {
        0x24, 0x02, 0x00, 0x05, // 0x00: addiu $v0, $zero, 5

        0x20, 0x42, 0xff, 0xff, // 0x04: L1: addi $v0, $v0, -1
        0x14, 0x40, 0xff, 0xfe, // 0x08: bne $v0, $zero, -8 (to L1)
        0x00, 0x00, 0x00, 0x00, // 0x0C: nop (delay slot)

        0x03, 0xe0, 0x00, 0x08, // 0x10: jr $ra
        0x00, 0x00, 0x00, 0x00  // 0x14: nop (delay slot)
    };

    csh handle;
    ASSERT_EQ(cs_open(CS_ARCH_MIPS, (cs_mode)(CS_MODE_MIPS32 | CS_MODE_BIG_ENDIAN), &handle), CS_ERR_OK);
    cs_option(handle, CS_OPT_DETAIL, CS_OPT_ON);

    Function result = analyze_mips_func(handle, code.data(), code.size(), 0x1000);

    // --- Assertions ---
    // We expect 3 basic blocks:
    // 1. The initial setup before the loop.
    // 2. The loop body itself.
    // 3. The final return after the loop finishes.
    ASSERT_EQ(result.blocks.size(), 3);

    std::sort(result.blocks.begin(), result.blocks.end(), [](const Function::Block& a, const Function::Block& b) {
        return a.base < b.base;
    });

    // Block 0: Initial setup
    EXPECT_EQ(result.blocks[0].base, 0x00);
    EXPECT_EQ(result.blocks[0].size, 4); // Just the addiu

    // Block 1: Loop body
    EXPECT_EQ(result.blocks[1].base, 0x04);
    EXPECT_EQ(result.blocks[1].size, 12); // addi + bne + nop

    // Block 2: Final return
    EXPECT_EQ(result.blocks[2].base, 0x10);
    EXPECT_EQ(result.blocks[2].size, 8); // jr + nop

    EXPECT_EQ(result.size, 24);

    cs_close(&handle);
}

*/