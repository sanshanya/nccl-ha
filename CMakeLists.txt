# CMakeLists.txt for the nccl-ha plugin

# 1. 设置 CMake 最低版本和项目信息
cmake_minimum_required(VERSION 3.10)
project(nccl-ha C)

# 2. 设置 C 语言标准和编译器选项
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
# 添加编译标志，-fPIC 是构建共享库所必需的
add_compile_options(-O2 -Wall -Werror -fPIC)

# 3. 添加插件的目标
# 定义插件名称
set(PLUGIN_NAME ha)
# 从源文件构建一个名为 "ha" 的共享库 (.so)
add_library(${PLUGIN_NAME} SHARED src/ha_net.c)
# 设置最终输出的 .so 文件名格式为 libnccl-net-ha.so
set_target_properties(${PLUGIN_NAME} PROPERTIES OUTPUT_NAME "nccl-net-${PLUGIN_NAME}")

# 4. 指定头文件搜索路径
# 告诉 CMake 在项目的 'include' 目录下寻找头文件
target_include_directories(${PLUGIN_NAME} PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# 5. 查找并链接依赖库 (例如 libibverbs)
# 这是一个更现代、更健壮的方法，用于未来的 IB 实现
# find_package(Libverbs REQUIRED)
# target_link_libraries(${PLUGIN_NAME} PRIVATE Verbs::verbs)

# 6. (可选) 添加安装规则，对应 "make install"
# 将编译好的 .so 文件安装到 /usr/local/lib
install(TARGETS ${PLUGIN_NAME} DESTINATION lib)

# --- 结束 ---
# CMake Tools 插件会自动读取此文件，并配置好一切！