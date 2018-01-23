#pragma once

#ifdef WIN32
#include "../../HydraAPI/clew/clew.h"
#else
#include <CL/cl.h>
#endif

struct ScanCLArgs
{
  cl_kernel        scanBlockK;
  cl_kernel        propagateK;
  cl_command_queue cmdQueue;
};

bool scan_alloc_internal(size_t a_size, cl_context ctx);
void scan_free_internal();

void scan1f_gpu(cl_mem a_data, size_t a_size, ScanCLArgs args);


struct ReduceCLArgs
{
  cl_kernel        reductionK;
  cl_command_queue cmdQueue;
};

void reduce_average4f_gpu(cl_mem a_data, size_t a_size, float result[4], ReduceCLArgs args);

