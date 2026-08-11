# Latch


## Prepare

```
sudo apt-get install build-essential
sudo apt-get install libglib2.0-dev
sudo apt-get install libgtest-dev
python3 -m pip install jinja2
python3 -m pip install pybind11
python3 -m pip install pandas
python3 -m pip install black==23.7.0
python3 -m pip install openpyxl
sudo apt install python3-dev
sudo apt install verilator
sudo apt install clang-format
mkdir build
cd build
cmake ../
make -j
```

## Maybe require

install spdlog
```
apt install libspdlog-dev
```

install gflags
```
git clone https://github.com/gflags/gflags.git
cd gflags
mkdir build
cd build
cmake ..
make -j
sudo make install
```

## Usage
Run ISA Unittests

```shell
cd build
cmake .. && make -j4
./test/isa/ihex_parser <hex文件>
./test/isa/elf_parser <ELF文件>
```

```shell
cmake -S . -B build  # cmake config
cmake --build build  # build
cd build/test && ctest  # run all test
ctest --output-on-failure -R '^{{test_name}}$'  # Run a single test based on its name, or filter on a regular expression
```

## CI

```shell
sudo vim /etc/security/limits.conf
 49 *                soft    nofile          1000000
 50 *                hard    nofile          1000000
```

## 版本

Release给软件的编译环境

1. ubuntu 16.04
2. 默认是GLIBC 2.23
3. 安装gcc 9.4.0
4. 安装python3.10，以及相关的python库
