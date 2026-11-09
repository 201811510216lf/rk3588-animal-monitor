# SPDX-License-Identifier: GPL-2.0
# 状态：implemented（初稿）/ hardware-blocked / UNVERIFIED（未在目标内核编译验证）
obj-$(CONFIG_MLX90640) += mlx90640.o

# UAPI 头随模块一起安装（用户态经 #include "mlx90640/uapi/mlx90640.h" 引用）
ccflags-y += -I$(src)
