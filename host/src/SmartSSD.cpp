#include "SmartSSD.hpp"

SmartSSD::SmartSSD(const std::string& binaryFileName, uint8_t device_id, bool p2p_enable)
{
#if (_DEBUG == 1)
    std::cout << "\x1B[32m[OpenCL Setup]\033[0m OpenCL/Host/Device Buffer Setup Started ..." << std::endl;
#endif
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
#if (_DEBUG == 1)
    std::cout << "Found Device=" << device_name.c_str() << ", device id = " << unsigned(device_id) << std::endl;
#endif

    // import_binary() command will find the OpenCL binary file created using the
    // v++ compiler load into OpenCL Binary and return as Binaries
    // OpenCL and it can contain many functions which can be executed on the
    // device.

    auto fileBuf = xcl::read_binary_file(binaryFileName.c_str());
    cl::Program::Binaries bins{{fileBuf.data(), fileBuf.size()}};
    devices.resize(1);

    m_program = new cl::Program(*m_context, devices, bins);
    m_p2pEnable = p2p_enable;
#if (_DEBUG == 1)
    std::cout << "\x1B[32m[OpenCL Setup]\033[0m OpenCL/Host/Device Buffer Setup Done ..." << std::endl;
#endif

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

    if (m_p2pEnable == false)
    {
        for (uint32_t i = 0; i < m_InputFileDescVec.size(); i++) 
        {
            delete (m_InputHostMappedBufVec[i]);
            delete (m_OutputHostMappedBufVec[i]);
        }
    }

    std::cout << "########################### Disk Operation ###########################################" << std::endl;
    std::cout << "\x1B[31m[Disk Operation]\033[0m File(input) open Time : " << m_input_file_open_time.count() << " ns" << std::endl;
    std::cout << "\x1B[31m[Disk Operation]\033[0m Total File(input) size : " << m_input_file_size << " B" << std::endl;
    std::cout << "\x1B[31m[Disk Operation]\033[0m Num Input Files:" << m_InputFileNameVec.size() << std::endl;

    float ssd_throughput_in_mbps_read = (float)m_input_file_size * 1000 / m_ssd_read_time.count();
    std::cout << "\x1B[31m[Disk Operation]\033[0m SSD Read Throughput: " << std::fixed << std::setprecision(2) << ssd_throughput_in_mbps_read;
    std::cout << " MB/s (" << m_ssd_read_time.count() << " ns)" << std::endl;;

    std::cout << "\x1B[31m[Disk Operation]\033[0m File(output) open Time : " << m_output_file_open_time.count() << " ns" << std::endl;
    std::cout << "\x1B[31m[Disk Operation]\033[0m Total File(output) size : " << m_output_file_size << " B" << std::endl;
    std::cout << "\x1B[31m[Disk Operation]\033[0m Output Files:" << m_OutputFileNameVec.size() << std::endl;
    float ssd_throughput_in_mbps_write = (float)m_output_file_size * 1000 / m_ssd_write_time.count();
    std::cout << "\x1B[31m[Disk Operation]\033[0m SSD Write Throughput: " << std::fixed << std::setprecision(2) << ssd_throughput_in_mbps_write;
    std::cout << " MB/s (" << m_ssd_write_time.count() << " ns)" << std::endl;;
}


void SmartSSD::SetInputFileList (const std::vector<std::string>& inputFile)
{
    m_InputFileNameVec = inputFile;
}

void SmartSSD::SetOutputFileList (const std::vector<std::string>& outputFile)
{
    m_OutputFileNameVec = outputFile;
}

void SmartSSD::OpenInputFiles()
{
#if (_DEBUG == 1)
    std::cout << "\x1B[31m[Disk Operation]\033[0m Reading Input Files Started ..." << std::endl;
    std::cout << "\x1B[31m[Disk Operation]\033[0m Num Input Files:" << m_InputFileNameVec.size() << std::endl;
#endif
    for (uint32_t fid = 0; fid < m_InputFileNameVec.size(); fid++) {
        std::string inFile_name = m_InputFileNameVec[fid];
        uint32_t input_size = get_file_size(inFile_name);
        uint64_t input_size_4k_multiple = ((input_size - 1) / (4096) + 1) * 4096;
        m_InputFileSizeVec.push_back(input_size_4k_multiple);
        m_input_file_size += input_size;

        auto file_open_time_start = std::chrono::high_resolution_clock::now();
        int fd_p2p_c_in = open(inFile_name.c_str(), O_RDONLY | O_DIRECT);
        if (fd_p2p_c_in <= 0) {
            std::cout << "P2P: Unable to open input file, fd: " << fd_p2p_c_in << std::endl;
            exit(1);
        }
        auto file_open_time_end = std::chrono::high_resolution_clock::now();
        m_input_file_open_time = m_input_file_open_time + std::chrono::duration<double, std::nano>(file_open_time_end - file_open_time_start);
        m_InputFileDescVec.push_back(fd_p2p_c_in);
    }
#if (_DEBUG == 1)
    std::cout << "\x1B[31m[Disk Operation]\033[0m Reading Input Files Done ..." << std::endl;
#endif
}

void SmartSSD::OpenOutputFiles()
{
#if (_DEBUG == 1)
    std::cout << "\x1B[31m[Disk Operation]\033[0m Reading Output Files Started ..." << std::endl;
    std::cout << "\x1B[31m[Disk Operation]\033[0m Output Files:" << m_OutputFileNameVec.size() << std::endl;
#endif
    for (uint32_t fid = 0; fid < m_OutputFileNameVec.size(); fid++) {
        std::string outFile_name = m_OutputFileNameVec[fid];
        auto file_open_time_start = std::chrono::high_resolution_clock::now();
        int fd_p2p_c_out = open(outFile_name.c_str(), O_CREAT | O_WRONLY | O_DIRECT, 0777);
        if (fd_p2p_c_out <= 0) {
            std::cout << "P2P: Unable to open input file, fd: " << fd_p2p_c_out << std::endl;
            exit(1);
        }
        auto file_open_time_end = std::chrono::high_resolution_clock::now();
        m_output_file_open_time = m_output_file_open_time + std::chrono::duration<double, std::nano>(file_open_time_end - file_open_time_start);
        m_OutputFileDescVec.push_back(fd_p2p_c_out);
    }
#if (_DEBUG == 1)
    std::cout << "\x1B[31m[Disk Operation]\033[0m Reading Output Files Done ..." << std::endl;
#endif
}

void SmartSSD::CloseInputFiles()
{
    std::chrono::duration<double, std::nano> file_open_time_ns(0);

    for (uint32_t fid = 0; fid < m_InputFileDescVec.size(); fid++) {
        auto file_open_time_start = std::chrono::high_resolution_clock::now();
        close(m_InputFileDescVec[fid]);
        auto file_open_time_end = std::chrono::high_resolution_clock::now();
        file_open_time_ns = file_open_time_ns + std::chrono::duration<double, std::nano>(file_open_time_end - file_open_time_start);
    }
#if (_DEBUG == 1)
    std::cout << "\x1B[31m[Disk Operation]\033[0m Close input Files Done ..." << std::endl;
#endif
}

void SmartSSD::CloseOutputFiles()
{
    std::chrono::duration<double, std::nano> file_open_time_ns(0);

    for (uint32_t fid = 0; fid < m_OutputFileDescVec.size(); fid++) {
        auto file_open_time_start = std::chrono::high_resolution_clock::now();
        close(m_OutputFileDescVec[fid]);
        auto file_open_time_end = std::chrono::high_resolution_clock::now();
        file_open_time_ns = file_open_time_ns + std::chrono::duration<double, std::nano>(file_open_time_end - file_open_time_start);
    }
#if (_DEBUG == 1)
    std::cout << "\x1B[31m[Disk Operation]\033[0m Close output Files Done ..." << std::endl;
#endif
}
        
void SmartSSD::initBuffer()
{
    for (uint32_t i = 0; i < m_InputFileDescVec.size(); i++) {
        // Device buffer allocation
        // K1 Input:- This buffer contains input chunk data
        if (m_p2pEnable == true)
        {
            // DDR buffer extensions
            cl_mem_ext_ptr_t lz4Ext;
            lz4Ext.flags = XCL_MEM_DDR_BANK0 | XCL_MEM_EXT_P2P_BUFFER;
            lz4Ext.param = NULL;
            lz4Ext.obj = nullptr;
            cl::Buffer* buffer_input =new cl::Buffer(*m_context, CL_MEM_EXT_PTR_XILINX | CL_MEM_READ_WRITE, m_InputFileSizeVec[i], &(lz4Ext));
            m_InputCLBufVec.push_back(buffer_input);

            uint8_t* h_buf_in_p2p = (uint8_t*)m_q->enqueueMapBuffer(*(buffer_input), CL_TRUE, CL_MAP_READ, 0, m_InputFileSizeVec[i]);
            m_InputHostMappedBufVec.push_back(h_buf_in_p2p);
        }
        else
        {
            uint8_t* hostBuf = (uint8_t*) aligned_alloc(4096, m_InputFileSizeVec[i]); //new uint8_t[inSizeVec[i]];
            m_InputHostMappedBufVec.push_back(hostBuf);

            cl::Buffer* buffer_input =new cl::Buffer(*m_context, CL_MEM_USE_HOST_PTR | CL_MEM_READ_WRITE, m_InputFileSizeVec[i], hostBuf);
            m_InputCLBufVec.push_back(buffer_input);
        }
        
        //K2 Output:- This buffer contains compressed data written by device
        if (m_p2pEnable) 
        {
            // DDR buffer extensions
            cl_mem_ext_ptr_t lz4Ext;
            lz4Ext.flags = XCL_MEM_DDR_BANK0 | XCL_MEM_EXT_P2P_BUFFER;
            lz4Ext.param = NULL;
            lz4Ext.obj = nullptr;
            cl::Buffer* buffer_output = new cl::Buffer(*m_context, CL_MEM_WRITE_ONLY | CL_MEM_EXT_PTR_XILINX, outputFileSizeVec[i], &(lz4Ext));
            m_OutputCLBufVec.push_back(buffer_output);
            uint8_t* h_buf_out_p2p = (uint8_t*)m_q->enqueueMapBuffer(*(buffer_output), CL_TRUE, CL_MAP_READ, 0, outputFileSizeVec[i]);
            m_OutputHostMappedBufVec.push_back(h_buf_out_p2p);
        }
        else
        {
            // Creating Host memory to read the compressed data back to host for non-p2p flow case
            uint8_t* resultData = (uint8_t*)  aligned_alloc(4096, outputFileSizeVec[i]);// new uint8_t[outputSize];
            m_OutputHostMappedBufVec.push_back(resultData);
            cl::Buffer* buffer_output = new cl::Buffer(*m_context, CL_MEM_USE_HOST_PTR | CL_MEM_WRITE_ONLY, outputFileSizeVec[i], resultData);
            m_OutputCLBufVec.push_back(buffer_output);
        }
    }
}

void SmartSSD::readFile(size_t size)
{
    int ret = 0;
    auto ssd_start = std::chrono::high_resolution_clock::now();
    for (uint32_t i = 0; i < m_InputFileDescVec.size(); i++) {
        if (size == 0)
        {
            size = m_InputFileSizeVec[i];
        }
        /* Read Data from ssd */
        ret = read(m_InputFileDescVec[i], m_InputHostMappedBufVec[i], size);
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
    int ret = 0;

    auto ssd_start = std::chrono::high_resolution_clock::now();
    for (uint32_t i = 0; i < m_OutputFileDescVec.size(); i++) {
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

        ret = write(m_OutputFileDescVec[i], m_OutputHostMappedBufVec[i], write_size);
        if (ret == -1)
        {
            std::cout << i << " :: " << write_size << std::endl;
            std::cout << "Write() failed with error: " << ret << ", line: " << __LINE__ << std::endl;
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