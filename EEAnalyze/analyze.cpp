/*
#include "analyze.h"
#include <algorithm>
#include <set>
#include <vector>
#include <iostream>

// --- Forward-declarations for local helper functions ---
static bool is_mips_control_flow(csh handle, const cs_insn& insn);
static bool is_mips_conditional_branch(const cs_insn& insn);
static bool is_mips_unconditional_jump(const cs_insn& insn);
static uint64_t get_mips_branch_target(const cs_insn& insn);

// --- Main Analysis Function Implementation ---
Function analyze_mips_func(csh capstone_handle, const void* code, size_t size, uint64_t base) {
    Function fn{ base };

    auto& blocks = fn.blocks;
    blocks.reserve(16);
    blocks.emplace_back(0, 0);

    std::vector<size_t> blockStack;
    blockStack.reserve(32);
    blockStack.push_back(0);

    std::set<uint64_t> knownBlockStarts;
    knownBlockStarts.insert(base);

    std::cout << "[LOG] Starting analysis for function at 0x" << std::hex << base << std::endl;

    while (!blockStack.empty()) {
        size_t currentBlockIndex = blockStack.back();
        blockStack.pop_back();

        Function::Block& curBlock = blocks[currentBlockIndex];
        
        std::cout << "[LOG] Processing block starting at offset 0x" << std::hex << curBlock.base << std::endl;

        uint64_t start_pos = curBlock.base;
        const uint8_t* data = (const uint8_t*)code + start_pos;

        // stacking the blocks till reaches control flow
        while (true) {
            cs_insn* insn;
            uint64_t current_addr = base + (data - (const uint8_t*)code);
            size_t current_size = size - (data - (const uint8_t*)code);

            if (current_size < 4) break;

            size_t count = cs_disasm(capstone_handle, data, current_size, current_addr, 1, &insn);
            if (count == 0) {
                break;
            }

            std::cout << "[LOG]   Addr: 0x" << std::hex << insn->address << ": " << insn->mnemonic << " " << insn->op_str << std::endl;

            curBlock.size += insn->size;
            data += insn->size;
            

            if (is_mips_control_flow(capstone_handle, *insn)) {
                std::cout << "[LOG]   >>> CONTROL FLOW DETECTED <<<" << std::endl;
                if (current_size > insn->size) {
                    curBlock.size += insn->size;
                }

                if (is_mips_conditional_branch(*insn)) {
                    uint64_t target_addr = get_mips_branch_target(*insn);
                    uint64_t fallthrough_addr = insn->address + 8;

                    if (knownBlockStarts.find(target_addr) == knownBlockStarts.end()) {
                        knownBlockStarts.insert(target_addr);
                        blocks.emplace_back(target_addr - base, 0);
                        blockStack.push_back(blocks.size() - 1);
                    }
                    if (knownBlockStarts.find(fallthrough_addr) == knownBlockStarts.end()) {
                        knownBlockStarts.insert(fallthrough_addr);
                        blocks.emplace_back(fallthrough_addr - base, 0);
                        blockStack.push_back(blocks.size() - 1);
                    }
                } else if (is_mips_unconditional_jump(*insn)) {
                    uint64_t target_addr = get_mips_branch_target(*insn);
                    if (knownBlockStarts.find(target_addr) == knownBlockStarts.end()) {
                        knownBlockStarts.insert(target_addr);
                        blocks.emplace_back(target_addr - base, 0);
                        blockStack.push_back(blocks.size() - 1);
                    }
                }
                
                cs_free(insn, 1);
                break;
            }
            cs_free(insn, 1);
        }
    }

    if (blocks.size() > 1) {
        std::sort(blocks.begin(), blocks.end(), [](const Function::Block& a, const Function::Block& b) {
            return a.base < b.base;
        });

        size_t discontinuity = (size_t)-1;
        for (size_t i = 0; i < blocks.size() - 1; i++) {
            if (blocks[i].base + blocks[i].size >= blocks[i + 1].base) {
                continue;
            }
            discontinuity = i + 1;
            break;
        }

        if (discontinuity != (size_t)-1) {
            blocks.erase(blocks.begin() + discontinuity, blocks.end());
        }
    }

    fn.size = 0;
    for (const auto& block : blocks) {
        fn.size = std::max(fn.size, block.base + block.size);
    }

    return fn;
}

int Function::search_block(uint64_t absolute_address) const {
    for (size_t i = 0; i < this->blocks.size(); ++i) {
        const Function::Block& block = this->blocks[i];
        uint64_t block_start = this->base_address + block.base;
        uint64_t block_end = block_start + block.size;

        if (block.size == 0) {
            if (absolute_address == block_start) return i;
        } else {
            if (absolute_address >= block_start && absolute_address < block_end) return i;
        }
    }
    return -1;
}

static bool is_mips_control_flow(csh handle, const cs_insn& insn) {
    bool is_jump = cs_insn_group(handle, &insn, CS_GRP_JUMP);
    bool is_branch = cs_insn_group(handle, &insn, CS_GRP_BRANCH_RELATIVE);
    
    std::cout << "[LOG]     Control flow check for '" << insn.mnemonic 
              << "': is_jump=" << is_jump << ", is_branch=" << is_branch << std::endl;

    return is_jump || is_branch;
}

static bool is_mips_conditional_branch(const cs_insn& insn) {
    switch (insn.id) {
        case MIPS_INS_BEQ: case MIPS_INS_BNE: case MIPS_INS_BGEZ:
        case MIPS_INS_BGTZ: case MIPS_INS_BLEZ: case MIPS_INS_BLTZ:
        case MIPS_INS_BEQL: case MIPS_INS_BNEL: case MIPS_INS_BGEZL:
        case MIPS_INS_BGTZL: case MIPS_INS_BLEZL: case MIPS_INS_BLTZL:
        // Add pseudo-instructions that Capstone might generate
        case MIPS_INS_BEQZ: case MIPS_INS_BNEZ:
            return true;
        default:
            return false;
    }
}

static bool is_mips_unconditional_jump(const cs_insn& insn) {
    switch (insn.id) {
        case MIPS_INS_J:
        case MIPS_INS_JAL:
            return true;
        default:
            return false;
    }
}

static uint64_t get_mips_branch_target(const cs_insn& insn) {
    if (insn.detail->mips.op_count > 0) {
        const auto& last_op = insn.detail->mips.operands[insn.detail->mips.op_count - 1];
        if (last_op.type == MIPS_OP_IMM) {
            return static_cast<uint64_t>(last_op.imm);
        }
    }
    return 0;
}
*/
#include <iostream>
#include <vector>
#include <string>
#include "analyze.h"
#include <algorithm> // For std::sort
#include <vector>
#include "instructions/RabbitizerInstructionR5900.h"
#include "instructions/RabbitizerInstrDescriptor.h"

// The main entry point for the analysis phase.
std::vector<Function> analyze_executable(uint32_t entry_point, const uint8_t* text_buffer, uint32_t text_size, uint32_t text_vram_start) {

    // --- Step 1: Global Analysis ---
    // Find the VRAM address of every function in the .text section.
    std::set<uint32_t> function_starts_set = find_function_starts(text_buffer, text_size, text_vram_start);

    // Manually add the main program entry point, as it's not called by a 'jal'.
    function_starts_set.insert(entry_point);

    // Convert to a vector and sort so we can determine function sizes.
    std::vector<uint32_t> sorted_starts(function_starts_set.begin(), function_starts_set.end());
    std::sort(sorted_starts.begin(), sorted_starts.end());

    std::vector<Function> all_functions;

    // --- Step 2: Per-Function Analysis ---
    // Iterate through the list of function start addresses.
    for (size_t i = 0; i < sorted_starts.size(); ++i) {
        uint32_t func_start_vram = sorted_starts[i];

        // Determine the function's size by looking at where the next function starts.
        uint32_t next_func_start_vram = (i + 1 < sorted_starts.size())
                                      ? sorted_starts[i+1]
                                      : (text_vram_start + text_size);
        uint32_t func_size = next_func_start_vram - func_start_vram;

        // Calculate a pointer to this specific function's code within the larger .text buffer.
        uint32_t func_offset_in_buffer = func_start_vram - text_vram_start;
        const uint8_t* func_code_ptr = text_buffer + func_offset_in_buffer;

        std::cout << "[+] Analyzing function #" << (i + 1) << " at VRAM 0x" << std::hex << func_start_vram << " (Size: " << std::dec << func_size << " bytes)" << std::endl;

        // --- Step 3: Create and Analyze ---
        // Create the Function object with its VRAM address.
        Function func(func_start_vram);

        // Tell the function to analyze itself, giving it a pointer to its own
        // code and its specific size.
        func.analyze(func_code_ptr, func_size);

        all_functions.push_back(func);
        std::cout << "    -> Successfully analyzed and stored function 0x" << std::hex << func_start_vram << std::dec << std::endl;
    }

    return all_functions;
}

/**
 * Scans the entire .text section to find the starting address of every function.
 * It does this by finding all instructions that are targets of a JAL instruction.
 * @param code A pointer to the raw bytes of the .text section.
 * @param code_size The size of the .text section in bytes.
 * @param text_vram_start The virtual memory address where the .text section starts.
 * @return A set of unique addresses, each being the start of a function.
 */
std::set<uint32_t> find_function_starts(const uint8_t* code, uint32_t code_size, uint32_t text_vram_start) {
    std::set<uint32_t> function_starts;

    // Note: A more advanced version of this function would also parse the ELF's
    // symbol table (.symtab) to find named functions, which is another
    // excellent source for function entry points.

    // Loop through the entire .text section to find all 'jal' targets.
    for (uint32_t offset = 0; offset + 4 <= code_size; offset += 4) {
        uint32_t current_vram = text_vram_start + offset;
        uint32_t instruction_word = *(reinterpret_cast<const uint32_t*>(code + offset));

        RabbitizerInstruction instr;
        RabbitizerInstructionR5900_init(&instr, instruction_word, current_vram);
        RabbitizerInstructionR5900_processUniqueId(&instr);

        const RabbitizerInstrDescriptor* descriptor = instr.descriptor;

        // The `doesLink` property is true for instructions that save a return address, like 'jal'.
        // `isJumpWithAddress` distinguishes `jal` from `jalr`.
        if (descriptor->doesLink && descriptor->isJumpWithAddress) {
            uint32_t target_vram = RabbitizerInstruction_getInstrIndexAsVram(&instr);
            std::cout << "Found JAL at 0x" << std::hex << current_vram
                      << "  ->  Target: 0x" << target_vram << std::dec << std::endl;

            function_starts.insert(target_vram);
        }

        RabbitizerInstruction_destroy(&instr);
    }

    return function_starts;
}