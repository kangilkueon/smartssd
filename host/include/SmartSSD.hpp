
#include <defns.h>
#define _DEBUG  (0)
class SmartSSD {
    public:
        SmartSSD(const std::string& binaryFile, uint8_t device_id, bool p2p_enable);
        ~SmartSSD();

        void SetInputFileList (const std::vector<std::string>& inputFile);
        void SetOutputFileList (const std::vector<std::string>& outputFile);
        void OpenInputFiles();
        void OpenOutputFiles();
        void CloseInputFiles();
        void CloseOutputFiles();

        void initBuffer();
        void readFile(size_t size = 0);
        void writeFile(size_t size = 0);
        
        virtual void preProcess();
        virtual void run();
        virtual void postProcess();
    protected:
        uint32_t get_file_size(std::string filename) {
            std::ifstream file(filename.c_str(), std::ifstream::binary);
            if (!file) {
                std::cout << "Unable to open file";
                exit(1);
            }
            file.seekg(0, file.end);
            uint32_t file_size = file.tellg();
            file.seekg(0, file.beg);
            file.close();
            return file_size;
        }

        cl::Program* m_program;
        cl::Context* m_context;
        cl::CommandQueue* m_q;

        bool m_p2pEnable;
        
        std::vector<std::string> m_InputFileNameVec;
        std::vector<int> m_InputFileDescVec;
        std::vector<uint32_t> m_InputFileSizeVec;
        
        std::vector<std::string> m_OutputFileNameVec;
        std::vector<int> m_OutputFileDescVec;
        std::vector<uint32_t> outputFileSizeVec;
        
        std::vector<cl::Buffer*> m_InputCLBufVec;
        std::vector<cl::Buffer*> m_OutputCLBufVec;
        
        std::vector<uint8_t*> m_InputHostMappedBufVec;
        std::vector<uint8_t*> m_OutputHostMappedBufVec;
        
    private:
        std::chrono::duration<double, std::nano> m_input_file_open_time;
        std::chrono::duration<double, std::nano> m_output_file_open_time;
        std::chrono::duration<double, std::nano> m_ssd_read_time;
        std::chrono::duration<double, std::nano> m_ssd_write_time;
        
        uint32_t m_input_file_size;
        uint32_t m_output_file_size;
};