#include <iostream>
#include <iomanip>
#include <vector>
#include <set>
#include <algorithm>
#include <elfio/elfio.hpp>

// Rabbitizer headers
#include "rabbitizer.h"
#include "instructions/RabbitizerInstruction.h"
#include "instructions/RabbitizerInstructionR5900.h"

// Use the macros from Rabbitizer's headers
#define GET_instr_index(instr_ptr)     RAB_INSTR_GET_instr_index(instr_ptr)
#define GET_rs(instr_ptr)              RAB_INSTR_GET_rs(instr_ptr)
#define GET_rt(instr_ptr)              RAB_INSTR_GET_rt(instr_ptr)

using namespace ELFIO;

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cout << "Usage: function_finder <elf_file>" << std::endl;
        return 1;
    }

    elfio reader;
    if (!reader.load(argv[1])) {
        std::cout << "Can't find or process ELF file " << argv[1] << std::endl;
        return 2;
    }

    section* text_sec = reader.sections[".text"];
    if (!text_sec) {
        std::cout << "'.text' section not found." << std::endl;
        return 3;
    }

    const char* text_data = text_sec->get_data();
    Elf_Xword text_size = text_sec->get_size();
    Elf64_Addr text_addr = text_sec->get_address();

    std::set<Elf64_Addr> jal_targets;
    std::set<Elf64_Addr> prologue_targets;
    float count = 0;

    // --- Sweep 1: Find all JAL targets ---
    for (unsigned int i = 0; i < text_size; i += 4) {
        uint32_t instr_word = *(reinterpret_cast<const uint32_t*>(text_data + i));
        RabbitizerInstruction instr;
        RabbitizerInstruction_init(&instr, instr_word, text_addr + i);
        RabbitizerInstructionR5900_processUniqueId(&instr);

        if (instr.uniqueId == RABBITIZER_INSTR_ID_cpu_jal) {
            uint32_t target_val = GET_instr_index(&instr);
            Elf64_Addr target_addr = (instr.vram & 0xF0000000) | (target_val << 2);
            jal_targets.insert(target_addr);
            count = count + 1;
        }

        RabbitizerInstruction_destroy(&instr);
    }

    // --- Sweep 2: Find functions via prologue heuristic ---
    for (unsigned int i = 0; i < text_size - 12; i += 4) { // Stop 3 instructions before the end
        uint32_t instr_word = *(reinterpret_cast<const uint32_t*>(text_data + i));
        RabbitizerInstruction instr;
        RabbitizerInstruction_init(&instr, instr_word, text_addr + i);
        RabbitizerInstructionR5900_processUniqueId(&instr);

        if (instr.uniqueId == RABBITIZER_INSTR_ID_cpu_j || instr.uniqueId == RABBITIZER_INSTR_ID_cpu_jr) {
            uint32_t after_delay_slot_word = *(reinterpret_cast<const uint32_t*>(text_data + i + 8));
            uint32_t after_that_word = *(reinterpret_cast<const uint32_t*>(text_data + i + 12));

            RabbitizerInstruction instr_after_delay;
            RabbitizerInstruction_init(&instr_after_delay, after_delay_slot_word, text_addr + i + 8);
            RabbitizerInstructionR5900_processUniqueId(&instr_after_delay);

            RabbitizerInstruction instr_after_that;
            RabbitizerInstruction_init(&instr_after_that, after_that_word, text_addr + i + 12);
            RabbitizerInstructionR5900_processUniqueId(&instr_after_that);

            bool is_move = (instr_after_delay.uniqueId == RABBITIZER_INSTR_ID_cpu_move);
            bool is_stack_alloc = (instr_after_delay.uniqueId == RABBITIZER_INSTR_ID_cpu_addiu && 
                                   GET_rs(&instr_after_that) == 29 && 
                                   GET_rt(&instr_after_that) == 29);

            if (is_stack_alloc) {
                prologue_targets.insert(text_addr + i + 8);
                count += 1;
            }

            RabbitizerInstruction_destroy(&instr_after_delay);
            RabbitizerInstruction_destroy(&instr_after_that);
        }
        RabbitizerInstruction_destroy(&instr);
    }

    // --- Report Results ---
    std::cout << "--- Functions found via JAL targets ---" << std::endl;
    for (const auto& addr : jal_targets) {
        std::cout << "0x" << std::hex << addr << std::endl;
    }

    std::cout << "\n--- Functions found via Prologue Heuristics ---" << std::endl;
    for (const auto& addr : prologue_targets) {
        std::cout << "0x" << std::hex << addr << std::endl;
    }

    std::cout << "\nScan complete. Found: " << count << std::endl;

    return 0;
}
