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
    std::cout << "\x1B[32m[FPGA Operation]\033[0m Compression Time : " << std::fixed << std::setprecision(2) << m_compression_time.count() << " ns" << std::endl;
    for (uint32_t i = 0; i < inputFDVec.size(); i++) {
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
        outputFileVec.push_back(out_file);

        std::ifstream oriFile(token.c_str(), std::ifstream::binary);
        if (!oriFile) {
            std::cout << "Unable to open file";
            exit(1);
        }
        uint32_t input_size = SmartSSD::get_file_size(oriFile);
        oriFile.close();

        uint64_t input_size_4k_multiple = ((input_size - 1) / (4096) + 1) * 4096;
        oriFileSizeVec.push_back(input_size_4k_multiple);
        printf("%d\n", input_size_4k_multiple);
    }

    outputFileSizeVec = oriFileSizeVec;
}

void Decompress::preProcess()
{
    std::cout << "preprocess\n";

    cl_mem_ext_ptr_t hostBoExt = {0};
    cl_mem_ext_ptr_t hostOutBoExt = {0};
    for (uint32_t fid = 0; fid < inputFDVec.size(); fid++) {
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
        unpacker_kernel_lz4->setArg(narg++, *(bufInputVec[fid]));
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
        decompress_kernel_lz4->setArg(narg++, *(bufInputVec[fid]));
        decompress_kernel_lz4->setArg(narg++, *(bufOutputVec[fid]));
        decompress_kernel_lz4->setArg(narg++, *(bufBlockInfoVec[fid]));
        decompress_kernel_lz4->setArg(narg++, *(bufChunkInfoVec[fid]));
        decompress_kernel_lz4->setArg(narg++, m_BlockSizeInKb);
        decompress_kernel_lz4->setArg(narg++, tmp);
        decompress_kernel_lz4->setArg(narg++, total_no_cu);
        decompress_kernel_lz4->setArg(narg++, num_blocks);
        decompressKernelVec.push_back(decompress_kernel_lz4);

        printf("PARAMETER : %d\n", num_blocks);
        cl::Event* event = new cl::Event();
        opFinishEvent.push_back(event);
    }
    m_q->finish();
}
void Decompress::run()
{
    std::cout << "run\n";

    auto total_start = std::chrono::high_resolution_clock::now();
    for (uint32_t fid = 0; fid < inputFDVec.size(); fid++) {
        cl::Event write_event;
        if (!m_p2p_enable) 
        {
            m_q->enqueueMigrateMemObjects({*(bufInputVec[fid])}, 0 /* 0 means from host*/, NULL, &write_event);
        }
        // m_q->enqueueMigrateMemObjects({*(bufChunkInfoVec[fid])}, 0 /* 0 means from host*/, NULL, NULL);

        write_event.wait();
    std::cout << "run0\n";
        std::vector<cl::Event> e_upWait;
        std::vector<cl::Event> e_decWait;
        cl::Event e_up;
        cl::Event e_dec;

        m_q->enqueueTask(*unpackerKernelVec[fid], NULL, &e_up);
        e_upWait.push_back(e_up);
        e_upWait[fid].wait();
    std::cout << "run1\n";
        m_q->enqueueTask(*decompressKernelVec[fid], &e_upWait, opFinishEvent[fid]);
        opFinishEvent[fid]->wait();
    std::cout << "run2\n";
        //m_q->enqueueMigrateMemObjects({*(bufOutputVec[fid])}, CL_MIGRATE_MEM_OBJECT_HOST, &e_decWait, opFinishEvent[fid]);
    std::cout << "run3\n";
    }

    std::cout << "run\n";
    for (uint32_t i = 0; i < inputFDVec.size(); i++) {
        opFinishEvent[i]->wait();
        if (m_p2p_enable == false) {
            //m_q->enqueueReadBuffer(*(bufOutputVec[i]), 0, 0, compressed_size, resultDataInHostVec[i]);
        }
    }
    std::cout << "3!" << std::endl;
}

     void Decompress::postProcess()
     {
        std::cout << "postProcess\n";
     }
std::vector<unsigned char> readBinary(const std::string& fileName) {
    ifstream file(fileName, ios::binary | ios::ate);
    if (file) {
        file.seekg(0, ios::end);
        streamsize size = file.tellg();
        file.seekg(0, ios::beg);
        std::vector<unsigned char> buffer(size);
        file.read((char*)buffer.data(), size);
        return buffer;
    } else {
        return std::vector<unsigned char>(0);
    }
}

// Constructor
xfLz4::xfLz4(const std::string& binaryFile) {
    int err;
    cl_int error;
    m_device = 0;
    cl_platform_id platform;
    cl_uint num_platforms;
    err = clGetPlatformIDs(0, NULL, &num_platforms);
    cl_platform_id* platform_ids = (cl_platform_id*)malloc(sizeof(cl_platform_id) * num_platforms);
    err = clGetPlatformIDs(num_platforms, platform_ids, NULL);
    size_t i;

    for (i = 0; i < num_platforms; i++) {
        size_t platform_name_size;
        err = clGetPlatformInfo(platform_ids[i], CL_PLATFORM_NAME, 0, NULL, &platform_name_size);
        if (err != CL_SUCCESS) {
            printf("Error: Could not determine platform name!\n");
            exit(EXIT_FAILURE);
        }

        char* platform_name = (char*)malloc(sizeof(char) * platform_name_size);
        if (platform_name == NULL) {
            printf("Error: out of memory!\n");
            exit(EXIT_FAILURE);
        }

        err = clGetPlatformInfo(platform_ids[i], CL_PLATFORM_NAME, platform_name_size, platform_name, NULL);
        if (err != CL_SUCCESS) {
            printf("Error: could not determine platform name!\n");
            exit(EXIT_FAILURE);
        }

        if (!strcmp(platform_name, "Xilinx")) {
            free(platform_name);
            platform = platform_ids[i];
            break;
        }

        free(platform_name);
    }

    err = clGetDeviceIDs(platform, CL_DEVICE_TYPE_ALL, 1, &m_device, NULL);
    cl_context_properties properties[3] = {CL_CONTEXT_PLATFORM, (cl_context_properties)platform, 0};

    m_context = clCreateContext(properties, 1, &m_device, NULL, NULL, &err);
    if (err != CL_SUCCESS) std::cout << "clCreateContext call: Failed to create a compute context" << err << std::endl;

    std::vector<unsigned char> binary = readBinary(binaryFile);
    size_t binary_size = binary.size();
    const unsigned char* binary_data = binary.data();

    m_program = clCreateProgramWithBinary(m_context, 1, &m_device, &binary_size, &binary_data, NULL, &err);

    ooo_q = clCreateCommandQueue(m_context, m_device,
                                 CL_QUEUE_OUT_OF_ORDER_EXEC_MODE_ENABLE | CL_QUEUE_PROFILING_ENABLE, &error);
}

// Destructor
xfLz4::~xfLz4() {
    clReleaseCommandQueue(ooo_q);
    clReleaseContext(m_context);
    clReleaseProgram(m_program);
}

void xfLz4::decompress_in_line_multiple_files(const std::vector<std::string>& inFileVec,
                                              std::vector<int>& fd_p2p_vec,
                                              std::vector<char*>& outVec,
                                              std::vector<uint64_t>& orgSizeVec,
                                              std::vector<uint64_t>& inSizeVec,
                                              bool enable_p2p) {
    std::vector<cl_kernel> unpackerKernelVec;
    std::vector<cl_kernel> decompressKernelVec;
    std::vector<cl_mem> bufInputVec;
    std::vector<cl_mem> bufOutVec;
    std::vector<cl_mem> bufBlockInfoVec;
    std::vector<cl_mem> bufChunkInfoVec;
    std::vector<uint64_t> inSizeVec4k;
    std::vector<char*> p2pPtrVec;
    std::vector<std::vector<uint8_t, aligned_allocator<uint8_t> > > inVec;

    uint64_t total_size = 0;
    uint64_t total_in_size = 0;
    std::chrono::duration<double, std::nano> total_ssd_time_ns(0);

    int ret = 0;
    cl_int error;

    cl_mem buffer_input, buffer_chunk_info;
        std::cout << "1!" << std::endl;
    for (uint32_t fid = 0; fid < inFileVec.size(); fid++) {
        uint64_t original_size = 0;
        uint32_t block_size_in_bytes = BLOCK_SIZE_IN_KB * 1024;
        uint32_t m_BlockSizeInKb = BLOCK_SIZE_IN_KB;
        original_size = orgSizeVec[fid];
        total_size += original_size;

        uint32_t num_blocks = (original_size - 1) / block_size_in_bytes + 1;
        uint8_t total_no_cu = 1;
        uint8_t first_chunk = 1;
        std::string up_kname = unpacker_kernel_names[0];
        std::string dec_kname = decompress_kernel_names[0];

        cl_mem_ext_ptr_t p2pBoExt = {0};
        cl_mem_ext_ptr_t hostBoExt = {0};
        cl_mem_ext_ptr_t hostOutBoExt = {0};

        if (enable_p2p) p2pBoExt = {XCL_MEM_EXT_P2P_BUFFER, NULL, 0};

        if ((fid % 2) == 0) {
            up_kname += ":{xilLz4Unpacker_1}";
            dec_kname += ":{xilLz4P2PDecompress_1}";
        } else {
            up_kname += ":{xilLz4Unpacker_2}";
            dec_kname += ":{xilLz4P2PDecompress_2}";
        }
        uint64_t input_size = inSizeVec[fid];
        uint64_t input_size_4k_multiple = ((input_size - 1) / (4096) + 1) * 4096;
        inSizeVec4k.push_back(input_size_4k_multiple);
        total_in_size += input_size;

        if (!enable_p2p) {
            std::vector<uint8_t, aligned_allocator<uint8_t> > in(input_size_4k_multiple);
            inVec.push_back(in);
            read(fd_p2p_vec[fid], inVec[fid].data(), inSizeVec4k[fid]);
        }
        // Allocate BOs.
        if (enable_p2p) {
            buffer_input = clCreateBuffer(m_context, CL_MEM_WRITE_ONLY | CL_MEM_EXT_PTR_XILINX, input_size_4k_multiple,
                                          &p2pBoExt, &error);
        } else {
            buffer_input = clCreateBuffer(m_context, CL_MEM_USE_HOST_PTR | CL_MEM_READ_ONLY, input_size_4k_multiple,
                                          inVec[fid].data(), &error);
        }
        if (error)
            std::cout << "P2P: buffer_input creation failed, error: " << error << ", line: " << __LINE__ << std::endl;

        bufInputVec.push_back(buffer_input);
        if (enable_p2p) {
            char* p2pPtr = (char*)clEnqueueMapBuffer(ooo_q, buffer_input, CL_TRUE, CL_MAP_READ, 0,
                                                     input_size_4k_multiple, 0, NULL, NULL, NULL);
            p2pPtrVec.push_back(p2pPtr);
        }

        assert(sizeof(dt_chunkInfo) == (GMEM_DATAWIDTH / 8));
        buffer_chunk_info = clCreateBuffer(m_context, CL_MEM_EXT_PTR_XILINX | CL_MEM_WRITE_ONLY, sizeof(dt_chunkInfo),
                                           &hostBoExt, &error);

        assert(sizeof(dt_blockInfo) == (GMEM_DATAWIDTH / 8));
        cl_mem buffer_block_info = clCreateBuffer(m_context, CL_MEM_EXT_PTR_XILINX | CL_MEM_WRITE_ONLY,
                                                  sizeof(dt_blockInfo) * num_blocks, &hostBoExt, &error);

        hostOutBoExt.obj = outVec[fid];
        cl_mem buffer_output =
            clCreateBuffer(m_context, CL_MEM_EXT_PTR_XILINX | CL_MEM_WRITE_ONLY | CL_MEM_USE_HOST_PTR, original_size,
                           &hostOutBoExt, &error);

        bufOutVec.push_back(buffer_output);

        bufChunkInfoVec.push_back(buffer_chunk_info);
        bufBlockInfoVec.push_back(buffer_block_info);

        cl_kernel unpacker_kernel_lz4 = clCreateKernel(m_program, up_kname.c_str(), &error);
        uint32_t narg = 0;
        clSetKernelArg(unpacker_kernel_lz4, narg++, sizeof(cl_mem), &bufInputVec[fid]);
        clSetKernelArg(unpacker_kernel_lz4, narg++, sizeof(cl_mem), &bufBlockInfoVec[fid]);
        clSetKernelArg(unpacker_kernel_lz4, narg++, sizeof(cl_mem), &bufChunkInfoVec[fid]);
        clSetKernelArg(unpacker_kernel_lz4, narg++, sizeof(uint32_t), &m_BlockSizeInKb);
        clSetKernelArg(unpacker_kernel_lz4, narg++, sizeof(uint8_t), &first_chunk);
        clSetKernelArg(unpacker_kernel_lz4, narg++, sizeof(uint8_t), &total_no_cu);
        clSetKernelArg(unpacker_kernel_lz4, narg++, sizeof(uint32_t), &num_blocks);
        unpackerKernelVec.push_back(unpacker_kernel_lz4);

        narg = 0;
        uint32_t tmp = 0;

        cl_kernel decompress_kernel_lz4 = clCreateKernel(m_program, dec_kname.c_str(), &error);
        clSetKernelArg(decompress_kernel_lz4, narg++, sizeof(cl_mem), &bufInputVec[fid]);
        clSetKernelArg(decompress_kernel_lz4, narg++, sizeof(cl_mem), &bufOutVec[fid]);
        clSetKernelArg(decompress_kernel_lz4, narg++, sizeof(cl_mem), &bufBlockInfoVec[fid]);
        clSetKernelArg(decompress_kernel_lz4, narg++, sizeof(cl_mem), &bufChunkInfoVec[fid]);
        clSetKernelArg(decompress_kernel_lz4, narg++, sizeof(uint32_t), &m_BlockSizeInKb);
        clSetKernelArg(decompress_kernel_lz4, narg++, sizeof(uint32_t), &tmp);
        clSetKernelArg(decompress_kernel_lz4, narg++, sizeof(uint8_t), &total_no_cu);
        clSetKernelArg(decompress_kernel_lz4, narg++, sizeof(uint32_t), &num_blocks);
        decompressKernelVec.push_back(decompress_kernel_lz4);
    }
    error = clFinish(ooo_q);

    auto total_start = std::chrono::high_resolution_clock::now();
    for (uint32_t fid = 0; fid < inFileVec.size(); fid++) {
        if (!enable_p2p) clEnqueueMigrateMemObjects(ooo_q, 1, &buffer_input, 0, 0, nullptr, nullptr);
        clEnqueueMigrateMemObjects(ooo_q, 1, &buffer_chunk_info, 0, 0, nullptr, nullptr);

        auto ssd_start = std::chrono::high_resolution_clock::now();
        if (enable_p2p) {
            ret = read(fd_p2p_vec[fid], p2pPtrVec[fid], inSizeVec4k[fid]);
            if (ret == -1)
                std::cout << "P2P: compress(): read() failed, err: " << ret << ", line: " << __LINE__ << std::endl;
        }
        auto ssd_end = std::chrono::high_resolution_clock::now();
        auto ssd_time_ns = std::chrono::duration<double, std::nano>(ssd_end - ssd_start);
        total_ssd_time_ns += ssd_time_ns;

        cl_event e_up, e_dec;
        error = clEnqueueTask(ooo_q, unpackerKernelVec[fid], 0, NULL, &e_up);
        error = clEnqueueTask(ooo_q, decompressKernelVec[fid], 1, &e_up, &e_dec);
        error = clEnqueueMigrateMemObjects(ooo_q, 1, &bufOutVec[fid], CL_MIGRATE_MEM_OBJECT_HOST, 1, &e_dec, NULL);
        clReleaseEvent(e_up);
        clReleaseEvent(e_dec);
    }

        std::cout << "3!" << std::endl;
    error = clFinish(ooo_q);
    auto total_end = std::chrono::high_resolution_clock::now();

    for (uint32_t fid = 0; fid < inFileVec.size(); fid++) {
        if (enable_p2p) clEnqueueUnmapMemObject(ooo_q, bufInputVec[fid], p2pPtrVec[fid], 0, NULL, NULL);
        clReleaseKernel(unpackerKernelVec[fid]);
        clReleaseKernel(decompressKernelVec[fid]);
        clReleaseMemObject(bufChunkInfoVec[fid]);
        clReleaseMemObject(bufBlockInfoVec[fid]);
        clReleaseMemObject(bufOutVec[fid]);
    }
    auto total_time_ns = std::chrono::duration<double, std::nano>(total_end - total_start);
    float throughput_in_mbps_1 = (float)total_size * 1000 / total_time_ns.count();
    float ssd_throughput_in_mbps_1 = (float)total_in_size * 1000 / total_ssd_time_ns.count();
    std::cout << std::fixed << std::setprecision(2) << "Throughput\t\t:" << throughput_in_mbps_1 << std::endl;
    if (enable_p2p) std::cout << "SSD Throughput\t\t:" << ssd_throughput_in_mbps_1 << std::endl;
    std::cout << "InputSize(inMB)\t\t:" << ((float)total_in_size / 1000000) << std::endl
              << "outputSize(inMB)\t:" << (float)total_size / 1000000 << std::endl
              << "CR\t\t\t:" << ((float)total_size / total_in_size) << std::endl;
}