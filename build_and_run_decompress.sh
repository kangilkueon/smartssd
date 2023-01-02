#!/bin/bash

source /opt/xilinx/xrt/setup.sh
source /tools/Xilinx/Vitis/2021.2/settings64.sh 
pushd ./client
make
echo "=== P2P Enable ==="
./compression-client --xclbin=../compression.xclbin --compress=false --enable_p2p=true --input=/mnt/smartssd/test.txt.lz4
./compression-client --xclbin=../compression.xclbin --compress=false --enable_p2p=true --input=/mnt/smartssd/test.txt.lz4 /mnt/smartssd/test2.txt.lz4
./compression-client --xclbin=../compression.xclbin --compress=false --enable_p2p=true --input=/mnt/smartssd/test.txt.lz4 /mnt/smartssd/test2.txt.lz4 /mnt/smartssd/test3.txt.lz4 /mnt/smartssd/test4.txt.lz4 
./compression-client --xclbin=../compression.xclbin --compress=false --enable_p2p=true --input=/mnt/smartssd/test.txt.lz4 /mnt/smartssd/test2.txt.lz4 /mnt/smartssd/test3.txt.lz4 /mnt/smartssd/test4.txt.lz4  /mnt/smartssd/test5.txt.lz4 /mnt/smartssd/test6.txt.lz4  /mnt/smartssd/test7.txt.lz4 /mnt/smartssd/test8.txt.lz4 

echo "=== P2P Disable ==="
./compression-client --xclbin=../compression.xclbin --compress=false --enable_p2p=false --input=/mnt/smartssd/test.txt.lz4
./compression-client --xclbin=../compression.xclbin --compress=false --enable_p2p=false --input=/mnt/smartssd/test.txt.lz4 /mnt/smartssd/test2.txt.lz4
./compression-client --xclbin=../compression.xclbin --compress=false --enable_p2p=false --input=/mnt/smartssd/test.txt.lz4 /mnt/smartssd/test2.txt.lz4 /mnt/smartssd/test3.txt.lz4 /mnt/smartssd/test4.txt.lz4 
./compression-client --xclbin=../compression.xclbin --compress=false --enable_p2p=false --input=/mnt/smartssd/test.txt.lz4 /mnt/smartssd/test2.txt.lz4 /mnt/smartssd/test3.txt.lz4 /mnt/smartssd/test4.txt.lz4  /mnt/smartssd/test5.txt.lz4 /mnt/smartssd/test6.txt.lz4  /mnt/smartssd/test7.txt.lz4 /mnt/smartssd/test8.txt.lz4 
popd
 #/opt/xilinx/xrt/bin/xbutil reset --device 0000:3e:00.1

