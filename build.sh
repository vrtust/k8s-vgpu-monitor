mkdir -p build
cd build
cmake ..
make -j

echo "2" > gpu_allocation.txt