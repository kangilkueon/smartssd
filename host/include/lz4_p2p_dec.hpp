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

#ifndef _XFCOMPRESSION_LZ4_P2P_DEC_HPP_
#define _XFCOMPRESSION_LZ4_P2P_DEC_HPP_

#include <iomanip>
#include "xcl2.hpp"
#include <fcntl.h>
#include <unistd.h>

// Maximum host buffer used to operate
// per kernel invocation
#define HOST_BUFFER_SIZE (2 * 1024 * 1024)

// Default block size
#define BLOCK_SIZE_IN_KB 64

#define KB 1024

// Max Input buffer Size
#define MAX_IN_BUFFER_SIZE (1024 * 1024 * 1024)

// Max Input Buffer Partitions
#define MAX_IN_BUFFER_PARTITION MAX_IN_BUFFER_SIZE / HOST_BUFFER_SIZE

// Maximum number of blocks based on host buffer size
#define MAX_NUMBER_BLOCKS (HOST_BUFFER_SIZE / (BLOCK_SIZE_IN_KB * 1024))


class Decompress : public SmartSSD {
    public:
    Decompress(const std::string& binaryFile, uint8_t device_id, bool p2p_enable);
    ~Decompress();

    void MakeOutputFileList(const std::vector<std::string>& inputFile);
    

    virtual void preProcess();
    virtual void run();
    virtual void postProcess();
private:
    std::vector<uint32_t> oriFileSizeVec;

    std::vector<std::string> outFileList;
    std::vector<std::string> orgFileList;

    std::vector<cl::Buffer*> bufChunkInfoVec;
    std::vector<cl::Buffer*> bufBlockInfoVec;

    std::vector<cl::Kernel*> unpackerKernelVec;
    std::vector<cl::Kernel*> decompressKernelVec;
    // Kernel names
    std::vector<std::string> unpacker_kernel_names = {"xilLz4Unpacker"};
    std::vector<std::string> decompress_kernel_names = {"xilLz4P2PDecompress"};
    
    std::chrono::duration<double, std::nano> m_compression_time;
};
#endif  // _XFCOMPRESSION_LZ4_P2P_DEC_HPP_