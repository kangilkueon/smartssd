#!/bin/bash

source /opt/xilinx/xrt/setup.sh
source /tools/Xilinx/Vitis/2021.2/settings64.sh 
pushd ./client
make
echo "=== P2P Enable ==="
./compression-client --xclbin=../compression.xclbin --enable_p2p=true --input=/mnt/smartssd/test.txt >> result.txt
./compression-client --xclbin=../compression.xclbin --enable_p2p=true --input=/mnt/smartssd/test.txt /mnt/smartssd/test2.txt >> result.txt
./compression-client --xclbin=../compression.xclbin --enable_p2p=true --input=/mnt/smartssd/test.txt /mnt/smartssd/test2.txt /mnt/smartssd/test3.txt /mnt/smartssd/test4.txt  >> result.txt
./compression-client --xclbin=../compression.xclbin --enable_p2p=true --input=/mnt/smartssd/test.txt /mnt/smartssd/test2.txt /mnt/smartssd/test3.txt /mnt/smartssd/test4.txt  /mnt/smartssd/test5.txt /mnt/smartssd/test6.txt  /mnt/smartssd/test7.txt /mnt/smartssd/test8.txt  >> result.txt

echo "=== P2P Disable ==="
./compression-client --xclbin=../compression.xclbin --enable_p2p=false --input=/mnt/smartssd/test.txt >> result.txt
./compression-client --xclbin=../compression.xclbin --enable_p2p=false --input=/mnt/smartssd/test.txt /mnt/smartssd/test2.txt >> result.txt
./compression-client --xclbin=../compression.xclbin --enable_p2p=false --input=/mnt/smartssd/test.txt /mnt/smartssd/test2.txt /mnt/smartssd/test3.txt /mnt/smartssd/test4.txt  >> result.txt
./compression-client --xclbin=../compression.xclbin --enable_p2p=false --input=/mnt/smartssd/test.txt /mnt/smartssd/test2.txt /mnt/smartssd/test3.txt /mnt/smartssd/test4.txt  /mnt/smartssd/test5.txt /mnt/smartssd/test6.txt  /mnt/smartssd/test7.txt /mnt/smartssd/test8.txt  >> result.txt
popd

