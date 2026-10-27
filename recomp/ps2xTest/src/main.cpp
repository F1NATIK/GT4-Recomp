#include "runtime/gs/gs_cpu_backend.h"
#include "MiniTest.h"
#include <cstdlib>
#include <iostream>
#include <string_view>

void register_code_generator_tests();
void register_r5900_decoder_tests();
void register_elf_analyzer_tests();
void register_pad_input_tests();
void register_ps2_runtime_io_tests();
void register_ps2_runtime_kernel_tests();
void register_ps2_runtime_interrupt_tests();
void register_ps2_memory_tests();
void register_ps2_vu1_tests();
void register_ps2_vu_tests();
void register_ps2_gs_tests();
void register_ps2_iop_tests();
void register_ps2_sif_rpc_tests();
void register_ps2_sif_dma_tests();
void register_ps2_recompiler_tests();
void register_ps2_runtime_expansion_tests();
void reset_ps2_test_function_table();

int main(int argc, char **argv)
{
    MiniTest::BeforeEach(reset_ps2_test_function_table);
    // Most GS tests read VRAM directly after submitting draws; batched
    // rasterization has its own equivalence test that turns it on.
    GSCpuBackend::SetBatchingOverride(0);

    if (argc == 2 && std::string_view(argv[1]) == "--pad-only")
    {
        register_pad_input_tests();
        return MiniTest::Run();
    }
    // The SIF RPC suite alone (the full run stops earlier, in PS2SifDma's
    // pre-existing executor-thread assertion).
    if (argc == 2 && std::string_view(argv[1]) == "--sif-rpc-only")
    {
        register_ps2_sif_rpc_tests();
        return MiniTest::Run();
    }
    if (argc == 2 && std::string_view(argv[1]) == "--interrupt-only")
    {
        register_ps2_runtime_interrupt_tests();
        return MiniTest::Run();
    }
    if (argc == 2 && std::string_view(argv[1]) == "--memory-only")
    {
        register_ps2_memory_tests();
        return MiniTest::Run();
    }
    if (argc == 2 && std::string_view(argv[1]) == "--gs-only")
    {
        register_ps2_gs_tests();
        return MiniTest::Run();
    }
    if (argc == 2 && std::string_view(argv[1]) == "--vu-only")
    {
        register_ps2_vu1_tests();
        register_ps2_vu_tests();
        return MiniTest::Run();
    }
    if (argc == 2 && std::string_view(argv[1]) == "--expansion-only")
    {
        register_ps2_runtime_expansion_tests();
        return MiniTest::Run();
    }

    register_code_generator_tests();
    register_r5900_decoder_tests();
    register_elf_analyzer_tests();
    register_pad_input_tests();
    register_ps2_runtime_io_tests();
    register_ps2_runtime_kernel_tests();
    register_ps2_runtime_interrupt_tests();
    register_ps2_memory_tests();
    register_ps2_vu1_tests();
    register_ps2_vu_tests();
    register_ps2_gs_tests();
    register_ps2_iop_tests();
    register_ps2_sif_rpc_tests();
    register_ps2_sif_dma_tests();
    register_ps2_recompiler_tests();
    register_ps2_runtime_expansion_tests();
    int res = MiniTest::Run();
    std::cout.flush();
    std::cerr.flush();
    std::_Exit(res);
}
