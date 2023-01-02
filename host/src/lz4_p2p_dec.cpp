/*
 * Copyright 2019 Xilinx, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "SmartSSD.hpp"
#include <iostream>
#include <cassert>
#include <vector>
#include "../../kernel/include/lz4_p2p.hpp"
#include "lz4_p2p_dec.hpp"
#include <cstdio>
#include <fstream>
#include <iosfwd>
#include "CL/cl.h"

using std::ifstream;
using std::ios;
using std::streamsize;
int fd_p2p_c_out = 0;
int fd_p2p_c_in = 0;


Decompress::Decompress(const std::string& binaryFile, uint8_t device_id, bool p2p_enable)
    : SmartSSD(binaryFile, device_id, p2p_enable)
{
    m_compression_time = std::chrono::milliseconds::zero();
}

Decompress::~Decompress()
{
    std::cout << "########################### FPGA Operation ###########################################" << std::endl;
    std::cout << "\x1B[32m[FPGA Operation]\033[0m Compression Time : " << std::fixed << std::setprecision(2) << m_compression_time.count() << " ns" << std::endl;
    for (uint32_t i = 0; i < m_InputFileDescVec.size(); i++) {
        delete (bufChunkInfoVec[i]);
        delete (bufBlockInfoVec[i]);

        delete (unpackerKernelVec[i]);
        delete (decompressKernelVec[i]);
    }
}

void Decompress::MakeOutputFileList(const std::vector<std::string>& inputFile)
{
    for (std::string inFile : inputFile)
    {
        std::string out_file = inFile + ".org";
        std::string delimiter = ".lz4";
        std::string token = inFile.substr(0, inFile.find(delimiter));
        orgFileList.push_back(token);
        m_OutputFileNameVec.push_back(out_file);

        uint32_t input_size = get_file_size(token.c_str());
        uint64_t input_size_4k_multiple = ((input_size - 1) / (4096) + 1) * 4096;
        oriFileSizeVec.push_back(input_size_4k_multiple);
    }

    outputFileSizeVec = oriFileSizeVec;
}

void Decompress::preProcess()
{
    cl_mem_ext_ptr_t hostBoExt = {0};
    for (uint32_t fid = 0; fid < m_InputFileDescVec.size(); fid++) {
        uint64_t original_size = 0;
        uint32_t block_size_in_bytes = BLOCK_SIZE_IN_KB * 1024;
        uint32_t m_BlockSizeInKb = BLOCK_SIZE_IN_KB;
        original_size = oriFileSizeVec[fid];

        uint32_t num_blocks = (original_size - 1) / block_size_in_bytes + 1;
        uint8_t total_no_cu = 1;
        uint8_t first_chunk = 1;
        std::string up_kname = unpacker_kernel_names[0];
        std::string dec_kname = decompress_kernel_names[0];

        int cu_num = 0; //i % 2;

        if (cu_num == 0) {
            up_kname += ":{xilLz4Unpacker_1}";
            dec_kname += ":{xilLz4P2PDecompress_1}";
        } else {
            up_kname += ":{xilLz4Unpacker_2}";
            dec_kname += ":{xilLz4P2PDecompress_2}";
        }

        assert(sizeof(dt_blockInfo) == (GMEM_DATAWIDTH / 8));
        cl::Buffer* buffer_chunk_info = new cl::Buffer(*m_context, CL_MEM_EXT_PTR_XILINX | CL_MEM_WRITE_ONLY, sizeof(dt_chunkInfo), &hostBoExt);
        cl::Buffer* buffer_block_info = new cl::Buffer(*m_context, CL_MEM_EXT_PTR_XILINX | CL_MEM_WRITE_ONLY, sizeof(dt_blockInfo) * num_blocks, &hostBoExt);

        bufChunkInfoVec.push_back(buffer_chunk_info);
        bufBlockInfoVec.push_back(buffer_block_info);

        cl::Kernel* unpacker_kernel_lz4 = new cl::Kernel(*m_program, up_kname.c_str());
        uint32_t narg = 0;
        unpacker_kernel_lz4->setArg(narg++, *(m_InputCLBufVec[fid]));
        unpacker_kernel_lz4->setArg(narg++, *(bufBlockInfoVec[fid]));
        unpacker_kernel_lz4->setArg(narg++, *(bufChunkInfoVec[fid]));
        unpacker_kernel_lz4->setArg(narg++, m_BlockSizeInKb);
        unpacker_kernel_lz4->setArg(narg++, first_chunk);
        unpacker_kernel_lz4->setArg(narg++, total_no_cu);
        unpacker_kernel_lz4->setArg(narg++, num_blocks);
        unpackerKernelVec.push_back(unpacker_kernel_lz4);

        narg = 0;
        uint32_t tmp = 0;

        cl::Kernel* decompress_kernel_lz4 = new cl::Kernel(*m_program, dec_kname.c_str());
        decompress_kernel_lz4->setArg(narg++, *(m_InputCLBufVec[fid]));
        decompress_kernel_lz4->setArg(narg++, *(m_OutputCLBufVec[fid]));
        decompress_kernel_lz4->setArg(narg++, *(bufBlockInfoVec[fid]));
        decompress_kernel_lz4->setArg(narg++, *(bufChunkInfoVec[fid]));
        decompress_kernel_lz4->setArg(narg++, m_BlockSizeInKb);
        decompress_kernel_lz4->setArg(narg++, tmp);
        decompress_kernel_lz4->setArg(narg++, total_no_cu);
        decompress_kernel_lz4->setArg(narg++, num_blocks);
        decompressKernelVec.push_back(decompress_kernel_lz4);
    }
    m_q->finish();
}
void Decompress::run()
{
    std::vector<cl::Event> writeWait;
    std::vector<cl::Event> unpackWait;
    std::vector<cl::Event> opFinishEvent;
    
    auto kernel_start = std::chrono::high_resolution_clock::now();
    for (uint32_t fid = 0; fid < m_InputFileDescVec.size(); fid++) {
        cl::Event write_event;
        cl::Event unpack_event;
        cl::Event opFinish_event;
        if (m_p2pEnable == false)
        {
            m_q->enqueueMigrateMemObjects({*(m_InputCLBufVec[fid])}, 0 /* 0 means from host*/, NULL, &write_event);
            write_event.wait();
        }

        m_q->enqueueTask(*unpackerKernelVec[fid], NULL, &unpack_event);
        unpackWait.push_back(unpack_event);
        
        m_q->enqueueTask(*decompressKernelVec[fid], &unpackWait, &opFinish_event);
        opFinishEvent.push_back(opFinish_event);
    }

    for (uint32_t i = 0; i < m_InputFileDescVec.size(); i++) {
        opFinishEvent[i].wait();

        if (m_p2pEnable == false) {
            m_q->enqueueReadBuffer(*(m_OutputCLBufVec[i]), 0, 0, oriFileSizeVec[i], m_OutputHostMappedBufVec[i]);
        }
    }

    m_q->finish();
    auto comp_end = std::chrono::high_resolution_clock::now();
    m_compression_time = std::chrono::duration<double, std::nano>(comp_end - kernel_start);
}

void Decompress::postProcess()
{
}