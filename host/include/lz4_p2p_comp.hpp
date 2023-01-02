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
#ifndef _XFCOMPRESSION_LZ4_P2P_COMP_HPP_
#define _XFCOMPRESSION_LZ4_P2P_COMP_HPP_

#pragma once
#include "defns.h"

// Maximum compute units supported
#define MAX_COMPUTE_UNITS 2

// Maximum host buffer used to operate
// per kernel invocation
#define HOST_BUFFER_SIZE (100 * 1024 * 1024)

// Default block size
#define BLOCK_SIZE_IN_KB 64

// Value below is used to associate with
// Overlapped buffers, ideally overlapped
// execution requires 2 resources per invocation
#define OVERLAP_BUF_COUNT 1

// Maximum number of blocks based on host buffer size
#define MAX_NUMBER_BLOCKS (HOST_BUFFER_SIZE / (BLOCK_SIZE_IN_KB * 1024))

// Below are the codes as per LZ4 standard for
// various maximum block sizes supported.
#define BSIZE_STD_64KB 64
#define BSIZE_STD_256KB 80
#define BSIZE_STD_1024KB 96
#define BSIZE_STD_4096KB 112

// Maximum block sizes supported by LZ4
#define MAX_BSIZE_64KB 65536
#define MAX_BSIZE_256KB 262144
#define MAX_BSIZE_1024KB 1048576
#define MAX_BSIZE_4096KB 4194304

// This value is used to set
// uncompressed block size value
// 4th byte is always set to below
// and placed as uncompressed byte
#define NO_COMPRESS_BIT 128

// In case of uncompressed block
// Values below are used to set
// 3rd byte to following values
// w.r.t various maximum block sizes
// supported by standard
#define BSIZE_NCOMP_64 1
#define BSIZE_NCOMP_256 4
#define BSIZE_NCOMP_1024 16
#define BSIZE_NCOMP_4096 64

int validate(std::string& inFile_name, std::string& outFile_name);

class Compress : public SmartSSD {
    public:
    Compress(const std::string& binaryFile, uint8_t device_id, bool p2p_enable, uint32_t block_kb);
    ~Compress();

    void MakeOutputFileList(const std::vector<std::string>& inputFile);
    void SetOutputFileSize();
    
    virtual void preProcess();
    virtual void run();
    virtual void postProcess();
private:
    size_t create_header(uint8_t* h_header, uint32_t inSize);
    
    // Block Size
    uint32_t m_BlockSizeInKb;

    std::vector<uint32_t> headerSizeVec;
    std::vector<uint8_t*> h_headerVec;
    std::vector<uint32_t*> h_blkSizeVec;
    std::vector<uint32_t*> h_lz4OutSizeVec;

    std::vector<cl::Buffer*> bufTmpOutputVec;
    std::vector<cl::Buffer*> buflz4OutSizeVec;
    std::vector<cl::Buffer*> bufCompSizeVec;
    std::vector<cl::Buffer*> bufblockSizeVec;
    std::vector<cl::Buffer*> bufheadVec;
    
    std::vector<cl::Kernel*> packerKernelVec;
    std::vector<cl::Kernel*> compressKernelVec;
    // Kernel names
    std::vector<std::string> compress_kernel_names = {"xilLz4Compress"};
    std::vector<std::string> packer_kernel_names = {"xilLz4Packer"};
    
    std::chrono::duration<double, std::nano> m_compression_time;
};
#endif // _XFCOMPRESSION_LZ4_P2P_COMP_HPP_
