#include "SmartSSD.hpp"

SmartSSD::SmartSSD(const std::string& binaryFileName, uint8_t device_id, bool p2p_enable)
{
    std::cout << "\x1B[32m[OpenCL Setup]\033[0m OpenCL/Host/Device Buffer Setup Started ..." << std::endl;
    // Index calculation
    // The get_xil_devices will return vector of Xilinx Devices
    std::vector<cl::Device> devices = xcl::get_xil_devices();
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
    m_p2p_enable = p2p_enable;
    std::cout << "\x1B[32m[OpenCL Setup]\033[0m OpenCL/Host/Device Buffer Setup Done ..." << std::endl;

    m_input_file_open_time = std::chrono::milliseconds::zero();
    m_output_file_open_time = std::chrono::milliseconds::zero();
    m_ssd_read_time = std::chrono::milliseconds::zero();
    m_ssd_write_time = std::chrono::milliseconds::zero();

    m_input_file_size = 0;
    m_output_file_size = 0;
}


SmartSSD::~SmartSSD() 
{
    delete (m_program);
    delete (m_q);
    delete (m_context);
    
    auto total_end = std::chrono::high_resolution_clock::now();

    std::cout << "########################### Test Result ############################################" << std::endl;
    std::cout << "\x1B[31m[Disk Operation]\033[0m File(input) open Time : " << m_input_file_open_time.count() << " ns" << std::endl;
    std::cout << "\x1B[31m[Disk Operation]\033[0m Total File size : " << m_input_file_size << " B" << std::endl;

    float ssd_throughput_in_mbps_read = (float)m_input_file_size * 1000 / m_ssd_read_time.count();
    std::cout << "\x1B[31m[Disk Operation]\033[0m SSD Read Throughput: " << std::fixed << std::setprecision(2) << ssd_throughput_in_mbps_read;
    std::cout << " MB/s (" << m_ssd_read_time.count() << " ns)" << std::endl;;

    std::cout << "\x1B[31m[Disk Operation]\033[0m File(output) open Time : " << m_output_file_open_time.count() << " ns" << std::endl;
    float ssd_throughput_in_mbps_write = (float)m_output_file_size * 1000 / m_ssd_write_time.count();
    std::cout << "\x1B[31m[Disk Operation]\033[0m SSD Write Throughput: " << std::fixed << std::setprecision(2) << ssd_throughput_in_mbps_write;
    std::cout << " MB/s (" << m_ssd_write_time.count() << " ns)";
    std::cout << "\n####################################################################################" << std::endl;
}


void SmartSSD::SetInputFileList (const std::vector<std::string>& inputFile)
{
    inputFileVec = inputFile;
}

void SmartSSD::SetOutputFileList (const std::vector<std::string>& outputFile)
{
    outputFileVec = outputFile;
}

void SmartSSD::OpenInputFiles()
{
    uint32_t total_file_size = 0;

    std::cout << "\nNum Input Files:" << inputFileVec.size() << std::endl;
    std::cout << "\x1B[31m[Disk Operation]\033[0m Reading Input Files Started ..." << std::endl;
    for (uint32_t fid = 0; fid < inputFileVec.size(); fid++) {
        std::string inFile_name = inputFileVec[fid];
        std::ifstream inFile(inFile_name.c_str(), std::ifstream::binary);
        if (!inFile) {
            std::cout << "Unable to open file";
            exit(1);
        }
        uint32_t input_size = SmartSSD::get_file_size(inFile);
        inFile.close();

        uint64_t input_size_4k_multiple = ((input_size - 1) / (4096) + 1) * 4096;
        inputFileSizeVec.push_back(input_size_4k_multiple);
        m_input_file_size += input_size;

        auto file_open_time_start = std::chrono::high_resolution_clock::now();
        int fd_p2p_c_in = open(inFile_name.c_str(), O_RDONLY | O_DIRECT);
        if (fd_p2p_c_in <= 0) {
            std::cout << "P2P: Unable to open input file, fd: " << fd_p2p_c_in << std::endl;
            exit(1);
        }
        auto file_open_time_end = std::chrono::high_resolution_clock::now();
        m_input_file_open_time = m_input_file_open_time + std::chrono::duration<double, std::nano>(file_open_time_end - file_open_time_start);
        inputFDVec.push_back(fd_p2p_c_in);
    }
    std::cout << "\x1B[31m[Disk Operation]\033[0m Reading Input Files Done ..." << std::endl;
}

void SmartSSD::OpenOutputFiles()
{
    std::cout << "\nNum Output Files:" << outputFileVec.size() << std::endl;
    std::cout << "\x1B[31m[Disk Operation]\033[0m Reading Output Files Started ..." << std::endl;
    for (uint32_t fid = 0; fid < outputFileVec.size(); fid++) {
        std::string outFile_name = outputFileVec[fid];
        auto file_open_time_start = std::chrono::high_resolution_clock::now();
        int fd_p2p_c_out = open(outFile_name.c_str(), O_CREAT | O_WRONLY | O_DIRECT, 0777);
        if (fd_p2p_c_out <= 0) {
            std::cout << "P2P: Unable to open input file, fd: " << fd_p2p_c_out << std::endl;
            exit(1);
        }
        auto file_open_time_end = std::chrono::high_resolution_clock::now();
        m_output_file_open_time = m_output_file_open_time + std::chrono::duration<double, std::nano>(file_open_time_end - file_open_time_start);
        outputFDVec.push_back(fd_p2p_c_out);
    }
    std::cout << "\x1B[31m[Disk Operation]\033[0m Reading Output Files Done ..." << std::endl;

    outputFileSizeVec = inputFileSizeVec;
}

void SmartSSD::CloseInputFiles()
{
    std::chrono::duration<double, std::nano> file_open_time_ns(0);

    for (uint32_t fid = 0; fid < inputFDVec.size(); fid++) {
        auto file_open_time_start = std::chrono::high_resolution_clock::now();
        close(inputFDVec[fid]);
        auto file_open_time_end = std::chrono::high_resolution_clock::now();
        file_open_time_ns = file_open_time_ns + std::chrono::duration<double, std::nano>(file_open_time_end - file_open_time_start);
    }
    std::cout << "\x1B[31m[Disk Operation]\033[0m Close output Files Done ..." << std::endl;
}

void SmartSSD::CloseOutputFiles()
{
    std::chrono::duration<double, std::nano> file_open_time_ns(0);

    for (uint32_t fid = 0; fid < outputFDVec.size(); fid++) {
        auto file_open_time_start = std::chrono::high_resolution_clock::now();
        close(outputFDVec[fid]);
        auto file_open_time_end = std::chrono::high_resolution_clock::now();
        file_open_time_ns = file_open_time_ns + std::chrono::duration<double, std::nano>(file_open_time_end - file_open_time_start);
    }
    std::cout << "\x1B[31m[Disk Operation]\033[0m Close output Files Done ..." << std::endl;
}
        
void SmartSSD::initBuffer()
{
    for (uint32_t i = 0; i < inputFDVec.size(); i++) {
        // Device buffer allocation
        // K1 Input:- This buffer contains input chunk data
        if (m_p2p_enable == true)
        {
            // DDR buffer extensions
            cl_mem_ext_ptr_t lz4Ext;
            lz4Ext.flags = XCL_MEM_DDR_BANK0 | XCL_MEM_EXT_P2P_BUFFER;
            lz4Ext.param = NULL;
            lz4Ext.obj = nullptr;
            cl::Buffer* buffer_input =new cl::Buffer(*m_context, CL_MEM_EXT_PTR_XILINX | CL_MEM_READ_WRITE, inputFileSizeVec[i], &(lz4Ext));
            bufInputVec.push_back(buffer_input);

            uint8_t* h_buf_in_p2p = (uint8_t*)m_q->enqueueMapBuffer(*(buffer_input), CL_TRUE, CL_MAP_READ, 0, inputFileSizeVec[i]);
            bufp2pInVec.push_back(h_buf_in_p2p);
        }
        else
        {
            uint8_t* originalData = (uint8_t*) aligned_alloc(4096, inputFileSizeVec[i]); //new uint8_t[inSizeVec[i]];
            originalDataInHostVec.push_back(originalData);

            cl::Buffer* buffer_input =new cl::Buffer(*m_context, CL_MEM_USE_HOST_PTR | CL_MEM_READ_WRITE, inputFileSizeVec[i], originalDataInHostVec[i]);
            bufInputVec.push_back(buffer_input);
        }
        
        //K2 Output:- This buffer contains compressed data written by device
        if (m_p2p_enable) 
        {
            // DDR buffer extensions
            cl_mem_ext_ptr_t lz4Ext;
            lz4Ext.flags = XCL_MEM_DDR_BANK0 | XCL_MEM_EXT_P2P_BUFFER;
            lz4Ext.param = NULL;
            lz4Ext.obj = nullptr;
            cl::Buffer* buffer_output = new cl::Buffer(*m_context, CL_MEM_WRITE_ONLY | CL_MEM_EXT_PTR_XILINX, outputFileSizeVec[i], &(lz4Ext));
            bufOutputVec.push_back(buffer_output);
            uint8_t* h_buf_out_p2p = (uint8_t*)m_q->enqueueMapBuffer(*(buffer_output), CL_TRUE, CL_MAP_READ, 0, outputFileSizeVec[i]);
            bufp2pOutVec.push_back(h_buf_out_p2p);
        }
        else
        {
            // Creating Host memory to read the compressed data back to host for non-p2p flow case
            uint8_t* resultData = (uint8_t*)  aligned_alloc(4096, outputFileSizeVec[i]);// new uint8_t[outputSize];
            resultDataInHostVec.push_back(resultData);
            cl::Buffer* buffer_output = new cl::Buffer(*m_context, CL_MEM_USE_HOST_PTR | CL_MEM_WRITE_ONLY, outputFileSizeVec[i], resultData);
            bufOutputVec.push_back(buffer_output);
        }
    }
}

void SmartSSD::readFile(size_t size)
{
    uint32_t ret = 0;
    auto ssd_start = std::chrono::high_resolution_clock::now();
    for (uint32_t i = 0; i < inputFDVec.size(); i++) {
        if (size == 0)
        {
            size = inputFileSizeVec[i];
        }
        /* Read Data from ssd */
        if (m_p2p_enable == true)
        {
            ret = read(inputFDVec[i], bufp2pInVec[i], size);
        }
        else
        {
            ret = read(inputFDVec[i], originalDataInHostVec[i], size);
        }

        if (ret == -1)
        {
            std::cout << "read() failed with error: " << ret << ", line: " << __LINE__ << std::endl;
            printf ("%d: %s\n",errno,strerror(errno));

            exit(1);
        }
    }
    auto ssd_end = std::chrono::high_resolution_clock::now();
    m_ssd_read_time = std::chrono::duration<double, std::nano>(ssd_end - ssd_start);
}

void SmartSSD::writeFile(size_t size)
{
    uint32_t ret = 0;

    auto ssd_start = std::chrono::high_resolution_clock::now();
    for (uint32_t i = 0; i < outputFDVec.size(); i++) {
        size_t write_size = 0;
        if (size == 0)
        {
            write_size = outputFileSizeVec[i];
        }
        else
        {
            write_size = size;
        }
        m_output_file_size += write_size;

        if (m_p2p_enable) {
            ret = write(outputFDVec[i], bufp2pOutVec[i], write_size);
            if (ret == -1)
            {
                std::cout << i << " :: " << write_size << std::endl;
                std::cout << "P2P: write() failed with error: " << ret << ", line: " << __LINE__ << std::endl;
            }
        } else {
            ret = write(outputFDVec[i], resultDataInHostVec[i], write_size);
            if (ret == -1)
            {
                std::cout << i << " :: " << write_size << std::endl;
                std::cout << "P2P: write() failed with error: " << ret << ", line: " << __LINE__ << std::endl;
            }
        }
    }
    auto ssd_end = std::chrono::high_resolution_clock::now();
    m_ssd_write_time = std::chrono::duration<double, std::nano>(ssd_end - ssd_start);
}

void SmartSSD::preProcess()
{
    
}

void SmartSSD::run()
{
    
}

void SmartSSD::postProcess()

{
    
}