#ifndef RES_HAL_HOST_H
#define RES_HAL_HOST_H

/* 主机端：从 build_ext_resources.py 生成的 bin 镜像加载资源区与 manifest。 */
int res_hal_host_init(const char* bin_path);
void res_hal_host_shutdown(void);

#endif
