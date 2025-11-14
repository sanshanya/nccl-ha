## 原生无插件
mpirun --bind-to core -np 8 --allow-run-as-root \
       -hostfile ip.txt \
       --mca routed direct \
       --mca btl_tcp_if_include bond0 \
       --mca oob_tcp_if_include bond0,eth0 \
       --mca plm_rsh_num_concurrent 512 \
       -x NCCL_NET_PLUGIN=none \
       -x NCCL_DEBUG=INFO \
       -x NCCL_IB_HCA=mlx5_0:0,mlx5_1:1,mlx5_2:1,mlx5_3:1,mlx5_4:1,mlx5_5:1,mlx5_6:1,mlx5_7:1 \
       -x NCCL_SOCKET_IFNAME=bond0 \
       -x UCX_NET_DEVICES=bond0 \
       -x NCCL_MIN_NCHANNELS=32 \
       -x NCCL_IB_QPS_PER_CONNECTION=8 \
       -x NCCL_PXN_DISABLE=1 \
       -x LD_LIBRARY_PATH=/root/sans/nccl/build/lib:$LD_LIBRARY_PATH \
       /root/sanstest/nccl-tests/build/all_reduce_perf \
       -b 8G -e 9G -i 2 -g 1 -c 0 -n 1 -m 50
## 带插件测试
mpirun --bind-to core -np 8 --allow-run-as-root \
       -hostfile ip.txt \
       --mca routed direct \
       --mca btl_tcp_if_include bond0 \
       --mca oob_tcp_if_include bond0,eth0 \
       --mca plm_rsh_num_concurrent 512 \
       -x NCCL_NET_PLUGIN=ha \
       -x NCCL_DEBUG=INFO \
       -x NCCL_IB_HCA=mlx5_0:0,mlx5_1:1,mlx5_2:1,mlx5_3:1,mlx5_4:1,mlx5_5:1,mlx5_6:1,mlx5_7:1 \
       -x NCCL_SOCKET_IFNAME=bond0 \
       -x UCX_NET_DEVICES=bond0 \
       -x NCCL_MIN_NCHANNELS=32 \
       -x NCCL_IB_QPS_PER_CONNECTION=8 \
       -x NCCL_PXN_DISABLE=1 \
       -x LD_LIBRARY_PATH=/root/sans/nccl-ha/build:/root/sans/nccl/build/lib:$LD_LIBRARY_PATH \
       /root/sanstest/nccl-tests/build/all_reduce_perf \
       -b 8G -e 9G -i 2 -g 1 -c 0 -n 1 -m 50