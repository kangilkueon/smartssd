/*
 * (c) Copyright 2019 Xilinx, Inc. All rights reserved.
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
 *
 */
#include "SmartSSD.hpp"
#include "lz4_p2p_comp.hpp"
#include "xxhash.h"
#define BLOCK_SIZE 64
#define KB 1024
#define MAGIC_HEADER_SIZE 4
#define MAGIC_BYTE_1 4
#define MAGIC_BYTE_2 34
#define MAGIC_BYTE_3 77
#define MAGIC_BYTE_4 24
#define FLG_BYTE 104

#define RESIDUE_4K 4096

Compress::Compress(const std::string& binaryFile, uint8_t device_id, bool p2p_enable, uint32_t block_kb)
    : SmartSSD(binaryFile, device_id, p2p_enable)
{
    m_BlockSizeInKb = block_kb;
    
    m_compression_time = std::chrono::milliseconds::zero();
}

Compress::~Compress()
{
    std::cout << "########################### FPGA Operation ###########################################" << std::endl;
    std::cout << "\x1B[32m[FPGA Operation]\033[0m Compression Time : " << std::fixed << std::setprecision(2) << m_compression_time.count() << " ns" << std::endl;
    for (uint32_t i = 0; i < m_InputFileDescVec.size(); i++) {
        delete (h_headerVec[i]);
        delete (h_blkSizeVec[i]);
        delete (h_lz4OutSizeVec[i]);

        delete (bufTmpOutputVec[i]);
        delete (buflz4OutSizeVec[i]);
        delete (bufCompSizeVec[i]);
        delete (bufblockSizeVec[i]);
        delete (bufheadVec[i]);
        
        delete (packerKernelVec[i]);
        delete (compressKernelVec[i]);
    }
}

void Compress::MakeOutputFileList(const std::vector<std::string>& inputFile)
{
    for (std::string inFile : inputFile)
    {
        std::string out_file = inFile + ".lz4";
        m_OutputFileNameVec.push_back(out_file);
    }
}

void Compress::SetOutputFileSize()
{
    outputFileSizeVec = m_InputFileSizeVec;
}

void Compress::preProcess()
{
    if (m_InputFileSizeVec.size() <= 0)
    {
        std::cout << "Set Input File First\n" << std::endl;
        exit(1);
    }

    for (uint32_t i = 0; i < m_InputFileDescVec.size(); i++) {
        uint8_t* h_header = (uint8_t*)aligned_alloc(4096, 4096);
        uint32_t* h_blksize = (uint32_t*)aligned_alloc(4096, 4096);
        uint32_t* h_lz4outSize = (uint32_t*)aligned_alloc(4096, 4096);
        uint32_t block_size_in_bytes = m_BlockSizeInKb * 1024;
        uint32_t head_size = create_header(h_header, m_InputFileSizeVec[i]);
        headerSizeVec.push_back(head_size);
        h_headerVec.push_back(h_header);
        h_blkSizeVec.push_back(h_blksize);
        h_lz4OutSizeVec.push_back(h_lz4outSize);
        
        std::string comp_kname = compress_kernel_names[0];
        std::string pack_kname = packer_kernel_names[0];

        // Total chunks in input file
        // For example: Input file size is 12MB and Host buffer size is 2MB
        // Then we have 12/2 = 6 chunks exists
        // Calculate the count of total chunks based on input size
        // This count is used to overlap the execution between chunks and file
        // operations

        uint32_t num_blocks = (m_InputFileSizeVec[i] - 1) / block_size_in_bytes + 1;

        int cu_num = 0; //i % 2;

        if (cu_num == 0) {
            comp_kname += ":{xilLz4Compress_1}";
            pack_kname += ":{xilLz4Packer_1}";
        } else {
            comp_kname += ":{xilLz4Compress_2}";
            pack_kname += ":{xilLz4Packer_2}";
        }
        
        // K1 Output:- This buffer contains compressed data written by device
        // K2 Input:- This is a input to data packer kernel
        cl::Buffer* buffer_output = new cl::Buffer(*m_context, CL_MEM_WRITE_ONLY, m_InputFileSizeVec[i]);
        bufTmpOutputVec.push_back(buffer_output);

        // K2 input:- This buffer contains compressed data written by device
        cl::Buffer* buffer_lz4OutSize = new cl::Buffer(*m_context, CL_MEM_USE_HOST_PTR | CL_MEM_WRITE_ONLY, 10 * sizeof(uint32_t), h_lz4OutSizeVec[i]);
        buflz4OutSizeVec.push_back(buffer_lz4OutSize);

        // K1 Ouput:- This buffer contains compressed block sizes
        // K2 Input:- This buffer is used in data packer kernel
        cl::Buffer* buffer_compressed_size = new cl::Buffer(*m_context, CL_MEM_WRITE_ONLY, num_blocks * sizeof(uint32_t));
        bufCompSizeVec.push_back(buffer_compressed_size);

        // Input:- This buffer contains original input block sizes
        cl::Buffer* buffer_block_size = new cl::Buffer(*m_context, CL_MEM_USE_HOST_PTR | CL_MEM_READ_ONLY, num_blocks * sizeof(uint32_t), h_blkSizeVec[i]);
        bufblockSizeVec.push_back(buffer_block_size);

        // Input:- Header buffer only used once
        cl::Buffer* buffer_header = new cl::Buffer(*m_context, CL_MEM_USE_HOST_PTR | CL_MEM_READ_ONLY, head_size * sizeof(uint8_t), h_headerVec[i]);
        bufheadVec.push_back(buffer_header);

        // Main loop of overlap execution
        // Loop below runs over total bricks i.e., host buffer size chunks
        // Figure out block sizes per brick
        uint32_t bIdx = 0;
        for (uint32_t j = 0; j < m_InputFileSizeVec[i]; j += block_size_in_bytes) {
            uint32_t block_size = block_size_in_bytes;
            if (j + block_size > m_InputFileSizeVec[i]) {
                block_size = m_InputFileSizeVec[i] - j;
            }
            h_blksize[bIdx++] = block_size;
        }

        // Set kernel arguments
        cl::Kernel* compress_kernel_lz4 = new cl::Kernel(*m_program, comp_kname.c_str());
        int narg = 0;
        compress_kernel_lz4->setArg(narg++, *(m_InputCLBufVec[i]));
        compress_kernel_lz4->setArg(narg++, *(bufTmpOutputVec[i]));
        compress_kernel_lz4->setArg(narg++, *(bufCompSizeVec[i]));
        compress_kernel_lz4->setArg(narg++, *(bufblockSizeVec[i]));
        compress_kernel_lz4->setArg(narg++, m_BlockSizeInKb);
        compress_kernel_lz4->setArg(narg++, m_InputFileSizeVec[i]);
        compressKernelVec.push_back(compress_kernel_lz4);

        uint32_t offset = 0;
        uint32_t tail_bytes = 0;
        tail_bytes = 1;
        uint32_t no_blocks_calc = (m_InputFileSizeVec[i] - 1) / (m_BlockSizeInKb * 1024) + 1;

        // K2 Set Kernel arguments
        cl::Kernel* packer_kernel_lz4 = new cl::Kernel(*m_program, pack_kname.c_str());
        narg = 0;
        packer_kernel_lz4->setArg(narg++, *(bufTmpOutputVec[i]));
        packer_kernel_lz4->setArg(narg++, *(m_OutputCLBufVec[i]));
        packer_kernel_lz4->setArg(narg++, *(bufheadVec[i]));
        packer_kernel_lz4->setArg(narg++, *(bufCompSizeVec[i]));
        packer_kernel_lz4->setArg(narg++, *(bufblockSizeVec[i]));
        packer_kernel_lz4->setArg(narg++, *(buflz4OutSizeVec[i]));
        packer_kernel_lz4->setArg(narg++, *(m_InputCLBufVec[i]));
        packer_kernel_lz4->setArg(narg++, headerSizeVec[i]);
        packer_kernel_lz4->setArg(narg++, offset);
        packer_kernel_lz4->setArg(narg++, m_BlockSizeInKb);
        packer_kernel_lz4->setArg(narg++, no_blocks_calc);
        packer_kernel_lz4->setArg(narg++, tail_bytes);
        packerKernelVec.push_back(packer_kernel_lz4);
    }
    m_q->finish();
}

void Compress::run()
{
    std::vector<cl::Event> compWait;
    std::vector<cl::Event> packWait;
    std::vector<cl::Event> writeWait;
    std::vector<cl::Event> opFinishEvent;
    
    auto comp_start = std::chrono::high_resolution_clock::now();
    for (uint32_t i = 0; i < m_InputFileDescVec.size(); i++) {
        /* Transfer data from host to device
        * In p2p case, no need to transfer buffer input to device from host.
        */
        cl::Event comp_event;
        cl::Event pack_event;
        cl::Event write_event;
        cl::Event opFinish_event;

        // Migrate memory - Map host to device buffers
        if (m_p2pEnable == false)
        {
            m_q->enqueueMigrateMemObjects({*(m_InputCLBufVec[i]), *(bufblockSizeVec[i]), *(bufheadVec[i])}, 0 /* 0 means from host*/, NULL, &write_event);
        }
        else
        {
            m_q->enqueueMigrateMemObjects({*(bufblockSizeVec[i]), *(bufheadVec[i])}, 0 /* 0 means from host*/, NULL, &write_event);
        }
        writeWait.push_back(write_event);

        // Fire compress kernel
        m_q->enqueueTask(*compressKernelVec[i], &writeWait, &comp_event);
        compWait.push_back(comp_event);

        // Fire packer kernel
        m_q->enqueueTask(*packerKernelVec[i], &compWait, &pack_event);
        packWait.push_back(pack_event);
        // Read back data
        
        m_q->enqueueMigrateMemObjects({*(buflz4OutSizeVec[i])}, CL_MIGRATE_MEM_OBJECT_HOST, &packWait, &opFinish_event);
        opFinishEvent.push_back(opFinish_event);
    }

    for (uint32_t i = 0; i < m_InputFileDescVec.size(); i++) {
        opFinishEvent[i].wait();

        uint32_t compressed_size = *(h_lz4OutSizeVec[i]);
        if (m_p2pEnable == false) {
            m_q->enqueueReadBuffer(*(m_OutputCLBufVec[i]), 0, 0, compressed_size, m_OutputHostMappedBufVec[i]);
        }
    }
    m_q->finish();
    auto comp_end = std::chrono::high_resolution_clock::now();
    m_compression_time = std::chrono::duration<double, std::nano>(comp_end - comp_start);
}

void Compress::postProcess()
{

    uint8_t empty_buffer[4096] = {0};
    for (uint32_t i = 0; i < m_InputFileDescVec.size(); i++) {
        uint32_t compressed_size = *(h_lz4OutSizeVec[i]);
        uint32_t align_4k = compressed_size / RESIDUE_4K;
        uint32_t outIdx_align = RESIDUE_4K * align_4k;
        uint32_t residue_size = compressed_size - outIdx_align;
        // Counter which helps in tracking
        // Output buffer index
        
        if (m_p2pEnable) {
            uint8_t* temp;
            temp = (uint8_t*) m_OutputHostMappedBufVec[i];

            /* Make last packer output block divisible by 4K by appending 0's */
            temp = temp + compressed_size;
            memcpy(temp, empty_buffer, RESIDUE_4K - residue_size);
        }
        compressed_size = outIdx_align + RESIDUE_4K;
        outputFileSizeVec[i] = compressed_size;
    }
}

size_t Compress::create_header(uint8_t* h_header, uint32_t inSize) {
    uint8_t block_size_header = 0;
    switch (m_BlockSizeInKb) {
        case 64:
            block_size_header = BSIZE_STD_64KB;
            break;
        case 256:
            block_size_header = BSIZE_STD_256KB;
            break;
        case 1024:
            block_size_header = BSIZE_STD_1024KB;
            break;
        case 4096:
            block_size_header = BSIZE_STD_4096KB;
            break;
        default:
            block_size_header = BSIZE_STD_64KB;
            std::cout << "Valid block size not given, setting to 64K" << std::endl;
            break;
    }

    uint8_t temp_buff[10] = {FLG_BYTE, block_size_header, inSize, inSize >> 8, inSize >> 16, inSize >> 24, 0, 0, 0, 0};

    // xxhash is used to calculate hash value
    uint32_t xxh = XXH32(temp_buff, 10, 0);
    // This value is sent to Kernel 2
    uint32_t xxhash_val = (xxh >> 8);

    // Header information
    uint32_t head_size = 0;

    h_header[head_size++] = MAGIC_BYTE_1;
    h_header[head_size++] = MAGIC_BYTE_2;
    h_header[head_size++] = MAGIC_BYTE_3;
    h_header[head_size++] = MAGIC_BYTE_4;

    h_header[head_size++] = FLG_BYTE;

    // Value
    switch (m_BlockSizeInKb) {
        case 64:
            h_header[head_size++] = BSIZE_STD_64KB;
            break;
        case 256:
            h_header[head_size++] = BSIZE_STD_256KB;
            break;
        case 1024:
            h_header[head_size++] = BSIZE_STD_1024KB;
            break;
        case 4096:
            h_header[head_size++] = BSIZE_STD_4096KB;
            break;
    }

    // Input size
    h_header[head_size++] = inSize;
    h_header[head_size++] = inSize >> 8;
    h_header[head_size++] = inSize >> 16;
    h_header[head_size++] = inSize >> 24;
    h_header[head_size++] = 0;
    h_header[head_size++] = 0;
    h_header[head_size++] = 0;
    h_header[head_size++] = 0;

    // XXHASH value
    h_header[head_size++] = xxhash_val;
    return head_size;
}