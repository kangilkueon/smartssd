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
    std::cout << "\x1B[32m[FPGA Operation]\033[0m Compression Time : " << m_compression_time.count() << " ns" << std::endl;
    for (uint32_t i = 0; i < inputFDVec.size(); i++) {
        delete (h_headerVec[i]);
        delete (h_blkSizeVec[i]);
        delete (h_lz4OutSizeVec[i]);

        delete (bufTmpOutputVec[i]);
        delete (buflz4OutSizeVec[i]);
        delete (bufCompSizeVec[i]);
        delete (bufblockSizeVec[i]);
        delete (bufheadVec[i]);
        
        delete (opFinishEvent[i]);
        
        delete (packerKernelVec[i]);
        delete (compressKernelVec[i]);
    }
}

void Compress::MakeOutputFileList(const std::vector<std::string>& inputFile)
{
    for (std::string inFile : inputFile)
    {
        std::string out_file = inFile + ".lz4";
        outputFileVec.push_back(out_file);
    }
}
void Compress::preProcess()
{
    if (inputFileSizeVec.size() <= 0)
    {
        std::cout << "Set Input File First\n" << std::endl;
        exit(1);
    }

    size_t outputSize = 0;
    for (uint32_t i = 0; i < inputFDVec.size(); i++) {
        // To handle files size less than 4K
        if (inputFileSizeVec[i] < RESIDUE_4K) {
            outputSize = RESIDUE_4K;
        } else {
            outputSize = inputFileSizeVec[i];
        }
        //outputFileSizeVec.push_back(outputSize);
        

        uint8_t* h_header = (uint8_t*)aligned_alloc(4096, 4096);
        uint32_t* h_blksize = (uint32_t*)aligned_alloc(4096, 4096);
        uint32_t* h_lz4outSize = (uint32_t*)aligned_alloc(4096, 4096);
        uint32_t block_size_in_bytes = m_BlockSizeInKb * 1024;
        uint32_t head_size = create_header(h_header, inputFileSizeVec[i]);
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

        uint32_t num_blocks = (inputFileSizeVec[i] - 1) / block_size_in_bytes + 1;

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
        cl::Buffer* buffer_output = new cl::Buffer(*m_context, CL_MEM_WRITE_ONLY, inputFileSizeVec[i]);
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
        cl::Event* event = new cl::Event();
        opFinishEvent.push_back(event);

        // Main loop of overlap execution
        // Loop below runs over total bricks i.e., host buffer size chunks
        // Figure out block sizes per brick
        uint32_t bIdx = 0;
        for (uint32_t j = 0; j < inputFileSizeVec[i]; j += block_size_in_bytes) {
            uint32_t block_size = block_size_in_bytes;
            if (j + block_size > inputFileSizeVec[i]) {
                block_size = inputFileSizeVec[i] - j;
            }
            h_blksize[bIdx++] = block_size;
        }

        // Set kernel arguments
        cl::Kernel* compress_kernel_lz4 = new cl::Kernel(*m_program, comp_kname.c_str());
        int narg = 0;
        compress_kernel_lz4->setArg(narg++, *(bufInputVec[i]));
        compress_kernel_lz4->setArg(narg++, *(bufTmpOutputVec[i]));
        compress_kernel_lz4->setArg(narg++, *(bufCompSizeVec[i]));
        compress_kernel_lz4->setArg(narg++, *(bufblockSizeVec[i]));
        compress_kernel_lz4->setArg(narg++, m_BlockSizeInKb);
        compress_kernel_lz4->setArg(narg++, inputFileSizeVec[i]);
        compressKernelVec.push_back(compress_kernel_lz4);

        uint32_t offset = 0;
        uint32_t tail_bytes = 0;
        tail_bytes = 1;
        uint32_t no_blocks_calc = (inputFileSizeVec[i] - 1) / (m_BlockSizeInKb * 1024) + 1;

        // K2 Set Kernel arguments
        cl::Kernel* packer_kernel_lz4 = new cl::Kernel(*m_program, pack_kname.c_str());
        narg = 0;
        packer_kernel_lz4->setArg(narg++, *(bufTmpOutputVec[i]));
        packer_kernel_lz4->setArg(narg++, *(bufOutputVec[i]));
        packer_kernel_lz4->setArg(narg++, *(bufheadVec[i]));
        packer_kernel_lz4->setArg(narg++, *(bufCompSizeVec[i]));
        packer_kernel_lz4->setArg(narg++, *(bufblockSizeVec[i]));
        packer_kernel_lz4->setArg(narg++, *(buflz4OutSizeVec[i]));
        packer_kernel_lz4->setArg(narg++, *(bufInputVec[i]));
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
    auto comp_start = std::chrono::high_resolution_clock::now();
    for (uint32_t i = 0; i < inputFDVec.size(); i++) {
        /* Transfer data from host to device
        * In p2p case, no need to transfer buffer input to device from host.
        */
        std::vector<cl::Event> compWait;
        std::vector<cl::Event> packWait;
        std::vector<cl::Event> writeWait;

        cl::Event comp_event, pack_event;
        cl::Event write_event;

        // Migrate memory - Map host to device buffers
        if (m_p2p_enable == false)
        {
            m_q->enqueueMigrateMemObjects({*(bufInputVec[i]), *(bufblockSizeVec[i]), *(bufheadVec[i])}, 0 /* 0 means from host*/, NULL, &write_event);
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
        
        m_q->enqueueMigrateMemObjects({*(buflz4OutSizeVec[i])}, CL_MIGRATE_MEM_OBJECT_HOST, &packWait,
                                      opFinishEvent[i]);
    }

    for (uint32_t i = 0; i < inputFDVec.size(); i++) {
        opFinishEvent[i]->wait();

        uint32_t compressed_size = *(h_lz4OutSizeVec[i]);
        if (m_p2p_enable == false) {
            m_q->enqueueReadBuffer(*(bufOutputVec[i]), 0, 0, compressed_size, resultDataInHostVec[i]);
        }
    }
    m_q->finish();
    auto comp_end = std::chrono::high_resolution_clock::now();
    m_compression_time = std::chrono::duration<double, std::nano>(comp_end - comp_start);
}

void Compress::postProcess()
{

    uint8_t empty_buffer[4096] = {0};
    uint64_t total_file_size = 0;
    uint64_t comp_file_size = 0;
    for (uint32_t i = 0; i < inputFDVec.size(); i++) {
        uint32_t compressed_size = *(h_lz4OutSizeVec[i]);
        uint32_t align_4k = compressed_size / RESIDUE_4K;
        uint32_t outIdx_align = RESIDUE_4K * align_4k;
        uint32_t residue_size = compressed_size - outIdx_align;
        // Counter which helps in tracking
        // Output buffer index
        
        if (m_p2p_enable) {
            uint8_t* temp;

            temp = (uint8_t*)bufp2pOutVec[i];

            /* Make last packer output block divisible by 4K by appending 0's */
            temp = temp + compressed_size;
            memcpy(temp, empty_buffer, RESIDUE_4K - residue_size);
        }
        compressed_size = outIdx_align + RESIDUE_4K;
        outputFileSizeVec[i] = compressed_size;

        comp_file_size += compressed_size;
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

    uint32_t block_size_in_bytes = m_BlockSizeInKb * 1024;

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

#if 0
/* File descriptors to open the input and output files with O_DIRECT option
 * These descriptors used in P2P case only
 */

// Constructor
xflz4::xflz4(const std::string& binaryFileName, uint8_t device_id, uint32_t m_block_kb) {
    // Index calculation
    // The get_xil_devices will return vector of Xilinx Devices
    std::vector<cl::Device> devices = xcl::get_xil_devices();
    m_BlockSizeInKb = m_block_kb;
    /* Multi board support: selecting the right device based on the device_id,
     * provided through command line args (-id <device_id>).
     */
    if (devices.size() <= device_id) {
        std::cout << "Identfied devices = " << devices.size() << ", given device id = " << unsigned(device_id)
                  << std::endl;
        std::cout << "Error: Device ID should be within the range of number of Devices identified" << std::endl;
        std::cout << "Program exited..\n" << std::endl;
        exit(1);
    }
    devices.at(0) = devices.at(device_id);

    cl::Device device = devices.at(0);

    // Creating Context and Command Queue for selected Device
    m_context = new cl::Context(device);
    m_q = new cl::CommandQueue(*m_context, device, CL_QUEUE_OUT_OF_ORDER_EXEC_MODE_ENABLE | CL_QUEUE_PROFILING_ENABLE);
    std::string device_name = device.getInfo<CL_DEVICE_NAME>();
    std::cout << "Found Device=" << device_name.c_str() << ", device id = " << unsigned(device_id) << std::endl;

    // import_binary() command will find the OpenCL binary file created using the
    // v++ compiler load into OpenCL Binary and return as Binaries
    // OpenCL and it can contain many functions which can be executed on the
    // device.

    auto fileBuf = xcl::read_binary_file(binaryFileName.c_str());
    cl::Program::Binaries bins{{fileBuf.data(), fileBuf.size()}};
    devices.resize(1);

    m_program = new cl::Program(*m_context, devices, bins);
}

size_t xflz4::create_header(uint8_t* h_header, uint32_t inSize) {
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

    uint32_t block_size_in_bytes = m_BlockSizeInKb * 1024;

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

// Destructor
xflz4::~xflz4() {
    delete (m_program);
    delete (m_q);
    delete (m_context);
}

// This version of compression does overlapped execution between
// Kernel and Host. I/O operations between Host and Device are
// overlapped with Kernel execution between multiple compute units
void xflz4::compress_in_line_multiple_files(std::vector<int>& fd_p2p_in_vec,
                                            const std::vector<std::string>& outFileVec,
                                            std::vector<uint32_t>& inSizeVec,
                                            bool enable_p2p) {
    std::vector<cl::Buffer*> bufInputVec;
    std::vector<cl::Buffer*> bufOutputVec;
    std::vector<cl::Buffer*> buflz4OutVec;
    std::vector<cl::Buffer*> buflz4OutSizeVec;
    std::vector<cl::Buffer*> bufblockSizeVec;
    std::vector<cl::Buffer*> bufCompSizeVec;
    std::vector<cl::Buffer*> bufheadVec;
    std::vector<uint8_t*> bufp2pInVec;
    std::vector<uint8_t*> bufp2pOutVec;
    
    std::vector<int> fd_p2p_out_vec;

    std::vector<uint8_t*> h_headerVec;
    std::vector<uint32_t*> h_blkSizeVec;
    std::vector<uint32_t*> h_lz4OutSizeVec;
    std::vector<cl::Event*> opFinishEvent;
    std::vector<uint32_t> headerSizeVec;
    std::vector<uint32_t> compressSizeVec;

    std::vector<cl::Kernel*> packerKernelVec;
    std::vector<cl::Kernel*> compressKernelVec;

    // only for Non-P2P
    std::vector<uint8_t*> originalDataInHostVec;
    std::vector<uint8_t*> compressDataInHostVec;
    uint32_t outputSize = 0;

    int ret = 0;

    uint64_t total_kernel_time = 0;
    uint64_t total_packer_kernel_time = 0;
    std::chrono::duration<double, std::nano> total_ssd_read_time_ns(0);
    std::chrono::duration<double, std::nano> total_ssd_write_time_ns(0);
    std::chrono::duration<double, std::nano> total_comp_time_ns(0);

    // Pre Processing
    for (uint32_t i = 0; i < fd_p2p_in_vec.size(); i++) {
        // To handle files size less than 4K
        if (inSizeVec[i] < RESIDUE_4K) {
            outputSize = RESIDUE_4K;
        } else {
            outputSize = inSizeVec[i];
        }

        uint8_t* h_header = (uint8_t*)aligned_alloc(4096, 4096);
        uint32_t* h_blksize = (uint32_t*)aligned_alloc(4096, 4096);
        uint32_t* h_lz4outSize = (uint32_t*)aligned_alloc(4096, 4096);
        uint32_t block_size_in_bytes = m_BlockSizeInKb * 1024;
        uint32_t head_size = create_header(h_header, inSizeVec[i]);
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

        uint32_t num_blocks = (inSizeVec[i] - 1) / block_size_in_bytes + 1;

        int cu_num = 0; //i % 2;

        if (cu_num == 0) {
            comp_kname += ":{xilLz4Compress_1}";
            pack_kname += ":{xilLz4Packer_1}";
        } else {
            comp_kname += ":{xilLz4Compress_2}";
            pack_kname += ":{xilLz4Packer_2}";
        }
        // Device buffer allocation
        // K1 Input:- This buffer contains input chunk data
        if (enable_p2p == true)
        {
            // DDR buffer extensions
            cl_mem_ext_ptr_t lz4Ext;
            lz4Ext.flags = XCL_MEM_DDR_BANK0 | XCL_MEM_EXT_P2P_BUFFER;
            lz4Ext.param = NULL;
            lz4Ext.obj = nullptr;
            cl::Buffer* buffer_input =new cl::Buffer(*m_context, CL_MEM_EXT_PTR_XILINX | CL_MEM_READ_WRITE, inSizeVec[i], &(lz4Ext));
            bufInputVec.push_back(buffer_input);

            uint8_t* h_buf_in_p2p = (uint8_t*)m_q->enqueueMapBuffer(*(buffer_input), CL_TRUE, CL_MAP_READ, 0, inSizeVec[i]);
            bufp2pInVec.push_back(h_buf_in_p2p);
        }
        else
        {
            uint8_t* originalData = (uint8_t*) aligned_alloc(4096, inSizeVec[i]); //new uint8_t[inSizeVec[i]];
            originalDataInHostVec.push_back(originalData);

            cl::Buffer* buffer_input =new cl::Buffer(*m_context, CL_MEM_USE_HOST_PTR | CL_MEM_READ_WRITE, inSizeVec[i], originalDataInHostVec[i]);
            bufInputVec.push_back(buffer_input);
        }

        //K2 Output:- This buffer contains compressed data written by device
        if (enable_p2p) 
        {
            // DDR buffer extensions
            cl_mem_ext_ptr_t lz4Ext;
            lz4Ext.flags = XCL_MEM_DDR_BANK0 | XCL_MEM_EXT_P2P_BUFFER;
            lz4Ext.param = NULL;
            lz4Ext.obj = nullptr;
            cl::Buffer* buffer_lz4out = new cl::Buffer(*m_context, CL_MEM_WRITE_ONLY | CL_MEM_EXT_PTR_XILINX, outputSize, &(lz4Ext));
            buflz4OutVec.push_back(buffer_lz4out);
            uint8_t* h_buf_out_p2p = (uint8_t*)m_q->enqueueMapBuffer(*(buffer_lz4out), CL_TRUE, CL_MAP_READ, 0, outputSize);
            bufp2pOutVec.push_back(h_buf_out_p2p);
        }
        else
        {
            // Creating Host memory to read the compressed data back to host for non-p2p flow case
            uint8_t* compressData = (uint8_t*)  aligned_alloc(4096, outputSize);// new uint8_t[outputSize];
            compressDataInHostVec.push_back(compressData);
            cl::Buffer* buffer_lz4out = new cl::Buffer(*m_context, CL_MEM_USE_HOST_PTR | CL_MEM_WRITE_ONLY, outputSize, compressDataInHostVec[i]);
            buflz4OutVec.push_back(buffer_lz4out);
        }
        
        // K1 Output:- This buffer contains compressed data written by device
        // K2 Input:- This is a input to data packer kernel
        cl::Buffer* buffer_output = new cl::Buffer(*m_context, CL_MEM_WRITE_ONLY, inSizeVec[i]);
        bufOutputVec.push_back(buffer_output);

        // K2 input:- This buffer contains compressed data written by device
        cl::Buffer* buffer_lz4OutSize = new cl::Buffer(*m_context, CL_MEM_USE_HOST_PTR | CL_MEM_WRITE_ONLY,
                                                       10 * sizeof(uint32_t), h_lz4OutSizeVec[i]);
        buflz4OutSizeVec.push_back(buffer_lz4OutSize);

        // K1 Ouput:- This buffer contains compressed block sizes
        // K2 Input:- This buffer is used in data packer kernel
        cl::Buffer* buffer_compressed_size =
            new cl::Buffer(*m_context, CL_MEM_WRITE_ONLY, num_blocks * sizeof(uint32_t));
        bufCompSizeVec.push_back(buffer_compressed_size);

        // Input:- This buffer contains original input block sizes
        cl::Buffer* buffer_block_size = new cl::Buffer(*m_context, CL_MEM_USE_HOST_PTR | CL_MEM_READ_ONLY,
                                                       num_blocks * sizeof(uint32_t), h_blkSizeVec[i]);
        bufblockSizeVec.push_back(buffer_block_size);

        // Input:- Header buffer only used once
        cl::Buffer* buffer_header = new cl::Buffer(*m_context, CL_MEM_USE_HOST_PTR | CL_MEM_READ_ONLY,
                                                   head_size * sizeof(uint8_t), h_headerVec[i]);
        bufheadVec.push_back(buffer_header);
        cl::Event* event = new cl::Event();
        opFinishEvent.push_back(event);

        // Main loop of overlap execution
        // Loop below runs over total bricks i.e., host buffer size chunks
        // Figure out block sizes per brick
        uint32_t bIdx = 0;
        for (uint32_t j = 0; j < inSizeVec[i]; j += block_size_in_bytes) {
            uint32_t block_size = block_size_in_bytes;
            if (j + block_size > inSizeVec[i]) {
                block_size = inSizeVec[i] - j;
            }
            h_blksize[bIdx++] = block_size;
        }

        int fd_p2p_c_out = open(outFileVec[i].c_str(), O_CREAT | O_WRONLY | O_DIRECT, 0777);
        if (fd_p2p_c_out <= 0) {
            std::cout << "P2P: Unable to open output file, exited!, ret: " << fd_p2p_c_out << std::endl;
            close(fd_p2p_c_out);
            exit(1);
        }
        fd_p2p_out_vec.push_back(fd_p2p_c_out);

        // Set kernel arguments
        cl::Kernel* compress_kernel_lz4 = new cl::Kernel(*m_program, comp_kname.c_str());
        int narg = 0;
        compress_kernel_lz4->setArg(narg++, *(bufInputVec[i]));
        compress_kernel_lz4->setArg(narg++, *(bufOutputVec[i]));
        compress_kernel_lz4->setArg(narg++, *(bufCompSizeVec[i]));
        compress_kernel_lz4->setArg(narg++, *(bufblockSizeVec[i]));
        compress_kernel_lz4->setArg(narg++, m_BlockSizeInKb);
        compress_kernel_lz4->setArg(narg++, inSizeVec[i]);
        compressKernelVec.push_back(compress_kernel_lz4);

        uint32_t offset = 0;
        uint32_t tail_bytes = 0;
        tail_bytes = 1;
        uint32_t no_blocks_calc = (inSizeVec[i] - 1) / (m_BlockSizeInKb * 1024) + 1;

        // K2 Set Kernel arguments
        cl::Kernel* packer_kernel_lz4 = new cl::Kernel(*m_program, pack_kname.c_str());
        narg = 0;
        packer_kernel_lz4->setArg(narg++, *(bufOutputVec[i]));
        packer_kernel_lz4->setArg(narg++, *(buflz4OutVec[i]));
        packer_kernel_lz4->setArg(narg++, *(bufheadVec[i]));
        packer_kernel_lz4->setArg(narg++, *(bufCompSizeVec[i]));
        packer_kernel_lz4->setArg(narg++, *(bufblockSizeVec[i]));
        packer_kernel_lz4->setArg(narg++, *(buflz4OutSizeVec[i]));
        packer_kernel_lz4->setArg(narg++, *(bufInputVec[i]));
        packer_kernel_lz4->setArg(narg++, headerSizeVec[i]);
        packer_kernel_lz4->setArg(narg++, offset);
        packer_kernel_lz4->setArg(narg++, m_BlockSizeInKb);
        packer_kernel_lz4->setArg(narg++, no_blocks_calc);
        packer_kernel_lz4->setArg(narg++, tail_bytes);
        packerKernelVec.push_back(packer_kernel_lz4);
        
    }
    m_q->finish();
    
    auto total_start = std::chrono::high_resolution_clock::now();
    for (uint32_t i = 0; i < fd_p2p_in_vec.size(); i++) {
        /* Read Data from ssd */
        auto ssd_start = std::chrono::high_resolution_clock::now();
        if (enable_p2p == true)
        {
            ret = read(fd_p2p_in_vec[i], bufp2pInVec[i], inSizeVec[i]);
        }
        else
        {
            ret = read(fd_p2p_in_vec[i], originalDataInHostVec[i], inSizeVec[i]);
        }

        if (ret == -1)
        {
            std::cout << "read() failed with error: " << ret << ", line: " << __LINE__ << std::endl;
            std::cout << "Error Param :: " << fd_p2p_in_vec[i] << ", " << originalDataInHostVec[i] << ", " << inSizeVec[i] << std::endl;
            printf ("%d: %s\n",errno,strerror(errno));

            exit(1);
        }
        auto ssd_end = std::chrono::high_resolution_clock::now();
        auto ssd_time_ns = std::chrono::duration<double, std::nano>(ssd_end - ssd_start);
        total_ssd_read_time_ns += ssd_time_ns;
    }

    auto comp_start = std::chrono::high_resolution_clock::now();
    for (uint32_t i = 0; i < fd_p2p_in_vec.size(); i++) {
        /* Transfer data from host to device
        * In p2p case, no need to transfer buffer input to device from host.
        */
        std::vector<cl::Event> compWait;
        std::vector<cl::Event> packWait;
        std::vector<cl::Event> writeWait;

        cl::Event comp_event, pack_event;
        cl::Event write_event;

        // Migrate memory - Map host to device buffers
        if (enable_p2p == false)
        {
            m_q->enqueueMigrateMemObjects({*(bufInputVec[i]), *(bufblockSizeVec[i]), *(bufheadVec[i])}, 0 /* 0 means from host*/, NULL, &write_event);
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
        
        m_q->enqueueMigrateMemObjects({*(buflz4OutSizeVec[i])}, CL_MIGRATE_MEM_OBJECT_HOST, &packWait,
                                      opFinishEvent[i]);
    }

    for (uint32_t i = 0; i < fd_p2p_in_vec.size(); i++) {
        opFinishEvent[i]->wait();

        uint32_t compressed_size = *(h_lz4OutSizeVec[i]);
        if (enable_p2p == false) {
            m_q->enqueueReadBuffer(*(buflz4OutVec[i]), 0, 0, compressed_size, compressDataInHostVec[i]);
            m_q->finish();
        }
    }
    auto comp_end = std::chrono::high_resolution_clock::now();
    total_comp_time_ns = std::chrono::duration<double, std::nano>(comp_end - comp_start);

    uint8_t empty_buffer[4096] = {0};
    uint64_t total_file_size = 0;
    uint64_t comp_file_size = 0;
    for (uint32_t i = 0; i < fd_p2p_in_vec.size(); i++) {
        uint32_t compressed_size = *(h_lz4OutSizeVec[i]);
        uint32_t align_4k = compressed_size / RESIDUE_4K;
        uint32_t outIdx_align = RESIDUE_4K * align_4k;
        uint32_t residue_size = compressed_size - outIdx_align;
        // Counter which helps in tracking
        // Output buffer index
        
        if (enable_p2p) {
            uint8_t* temp;

            temp = (uint8_t*)bufp2pOutVec[i];

            /* Make last packer output block divisible by 4K by appending 0's */
            temp = temp + compressed_size;
            memcpy(temp, empty_buffer, RESIDUE_4K - residue_size);
        }
        compressed_size = outIdx_align + RESIDUE_4K;
        compressSizeVec.push_back(compressed_size);

        comp_file_size += compressed_size;

        auto ssd_start = std::chrono::high_resolution_clock::now();
        if (enable_p2p) {
            ret = write(fd_p2p_out_vec[i], bufp2pOutVec[i], compressed_size);
            if (ret == -1)
            {
                std::cout << i << " :: " << compressed_size << std::endl;
                std::cout << "P2P: write() failed with error: " << ret << ", line: " << __LINE__ << std::endl;
            }

            close(fd_p2p_out_vec[i]);
        } else {
            ret = write(fd_p2p_out_vec[i], compressDataInHostVec[i], compressed_size);
            if (ret == -1)
            {
                std::cout << i << " :: " << compressed_size << std::endl;
                std::cout << "P2P: write() failed with error: " << ret << ", line: " << __LINE__ << std::endl;
            }
            close(fd_p2p_out_vec[i]);
        }
        auto ssd_end = std::chrono::high_resolution_clock::now();
        auto ssd_time_ns = std::chrono::duration<double, std::nano>(ssd_end - ssd_start);
        total_ssd_write_time_ns += ssd_time_ns;
        total_file_size += inSizeVec[i];

        
        if (!enable_p2p) {
            delete originalDataInHostVec[i];
            delete compressDataInHostVec[i];
        }
    }
    
    // Post Processing and cleanup
    auto total_end = std::chrono::high_resolution_clock::now();

    std::cout << "########################### Test Result ############################################" << std::endl;
    float ssd_throughput_in_mbps_read = (float)total_file_size * 1000 / total_ssd_read_time_ns.count();
    std::cout << "\nSSD Read Throughput: " << std::fixed << std::setprecision(2) << ssd_throughput_in_mbps_read;
    std::cout << " MB/s (" << total_ssd_read_time_ns.count() << " ns)";

    float ssd_throughput_in_mbps_write = (float)comp_file_size * 1000 / total_ssd_write_time_ns.count();
    std::cout << "\nSSD Write Throughput: " << std::fixed << std::setprecision(2) << ssd_throughput_in_mbps_write;
    std::cout << " MB/s (" << total_ssd_write_time_ns.count() << " ns)";

    float throughput_comp_only = (float)total_file_size * 1000 / total_comp_time_ns.count();
    std::cout << "\nComp bandwidth: " << std::fixed << std::setprecision(2)
              << throughput_comp_only;
    std::cout << " MB/s (" << total_comp_time_ns.count() << " ns)";

    auto time_ns = std::chrono::duration<double, std::nano>(total_end - total_start);
    float throughput_in_mbps_1 = (float)total_file_size * 1000 / time_ns.count();

    std::cout << "\nOverall Throughput [Including SSD Operation]: " << std::fixed << std::setprecision(2)
              << throughput_in_mbps_1;
    std::cout << " MB/s (" << time_ns.count() << " ns)";
    std::cout << "\n####################################################################################" << std::endl;
    for (uint32_t i = 0; i < fd_p2p_in_vec.size(); i++) {
        delete (bufInputVec[i]);
        delete (bufOutputVec[i]);
        delete (buflz4OutVec[i]);
        delete (bufCompSizeVec[i]);
        delete (bufblockSizeVec[i]);
        delete (buflz4OutSizeVec[i]);
        delete (compressKernelVec[i]);
        delete (packerKernelVec[i]);

        close(fd_p2p_in_vec[i]);
    }
}
#endif