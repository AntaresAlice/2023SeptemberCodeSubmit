//
// Copyright (c) 2021, NVIDIA CORPORATION. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//  * Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//  * Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//  * Neither the name of NVIDIA CORPORATION nor the names of its
//    contributors may be used to endorse or promote products derived
//    from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
// OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//

#include <optix.h>
#include <optix_function_table_definition.h>
#include <optix_stack_size.h>
#include <optix_stubs.h>

#include <cuda_runtime.h>

#include <sampleConfig.h>

#include <sutil/Exception.h>
#include <sutil/sutil.h>

#include "state.h"
#include "timer.h"

#include <array>
#include <bitset>
#include <iomanip>
#include <iostream>
#include <string>
#include <unistd.h>
#include <map>

#include <sutil/Camera.h>

#include <thrust/device_vector.h>
#include <thrust/sort.h>
#include <thrust/sequence.h>
#include <thrust/gather.h>

#define THREAD_NUM (sysconf(_SC_NPROCESSORS_ONLN) - 2)   // (20 - 2) = 18

template <typename T>
struct SbtRecord
{
    __align__(OPTIX_SBT_RECORD_ALIGNMENT) char header[OPTIX_SBT_RECORD_HEADER_SIZE];
    T data;
};

typedef SbtRecord<RayGenData> RayGenSbtRecord;
typedef SbtRecord<MissData> MissSbtRecord;
typedef SbtRecord<HitGroupData> HitGroupSbtRecord;

typedef uint32_t CODE;
typedef uint32_t BITS;

//
//  variable
//
Timer                   timer_;
ScanState               state;

extern "C" void kGenAABB(double3 *points, double radius, unsigned int numPrims, OptixAabb *d_aabb);

static void context_log_cb(unsigned int level, const char* tag, const char* message, void* /*cbdata */) {
    std::cerr << "[" << std::setw(2) << level << "][" << std::setw(12) << tag << "]: "
              << message << "\n";
}

void uint32_to_double3(double3* vertices, CODE **data, int length, int column_num) {
    for (int i = 0; i < length; i++) {
        vertices[i] = {
            static_cast<double>(data[0][i]),
            0,
            0,
        };
    }
}

void vertices_to_triangles(ScanState &state, double3 *vertices, float3 *triangle_vertices) {
    for (int i = 0; i < state.length; i++) {
        triangle_vertices[3 * i] = {
            (float) vertices[i].x, 
            (float) vertices[i].y, 
            (float) (vertices[i].z + state.params.aabb_width + state.params.epsilon)
        };
        triangle_vertices[3 * i + 1] = {
            (float) (vertices[i].x + state.params.aabb_width + state.params.epsilon), 
            (float) vertices[i].y, 
            (float) vertices[i].z
        };
        triangle_vertices[3 * i + 2] = {
            (float) vertices[i].x, 
            (float) (vertices[i].y + state.params.aabb_width + state.params.epsilon), 
            (float) (vertices[i].z - state.params.aabb_width - state.params.epsilon)
        };
    }
    // for (int i = 0; i < 30; i++) {
    //     printf("triangle_vertices[%d] = {%f, %f, %f}\n", i, triangle_vertices[i].x, triangle_vertices[i].y, triangle_vertices[i].z);
    // }
}

inline void set_result_cpu(unsigned int *result, int pos, bool inverse) {
    assert(pos >= 0 && pos < 32);
    if (inverse) {
        *result &= ~(1 << (31 - pos));
    } else {
        *result |= (1 << (31 - pos));
    }
}

void scan_with_cpu(const double3 *vertices, int data_num,
                   unsigned int *result, double *predicate, bool inverse, int column_num) {
    for (int i = 0; i < data_num; i++) {
        if (vertices[i].x > predicate[0] && vertices[i].x < predicate[1]) {
            set_result_cpu(result + i / 32, i % 32, inverse);
        }
    }
}

int find_different_pos(unsigned int result_cpu, unsigned int result_gpu) {
    int pos = 0;
    for (int i = 0; i < 32; i++) {
        unsigned int val = 1 << (31 - i);
        if ((result_cpu & val) != (result_gpu & val)) {
            pos = i;
            break;
        }
    }
    return pos;
}

void compare_result(unsigned int* result_cpu, unsigned int* result_gpu, int data_num, double *p, bool inverse, int column_num) {
    int len = (data_num - 1) / 32 + 1;
    for (int i = 0; i < len; i++) {
        if (result_cpu[i] != result_gpu[i]) {
            int pos = find_different_pos(result_cpu[i], result_gpu[i]);
            int val_pos = i * 32 + pos;
            fprintf(stdout,
                "compare result error: result_cpu[%d] = 0x%08x, result_gpu[%d] = 0x%08x, pos = %d\n"
                "data[%d] = {x: %.0f, y: %.0f, z: %.0f}\n"
                "predicate = {x1: %.0f, x2: %.0f, y1: %.0f, y2: %.0f, z1: %.0f, z2: %.0f}\n"
                "inverse = %s\n",
                i, result_cpu[i],
                i, result_gpu[i],
                pos,
                val_pos, state.vertices[val_pos].x, state.vertices[val_pos].y, state.vertices[val_pos].z,
                p[0], p[1], p[2], p[3], p[4], p[5],
                inverse ? "true" : "false");
            exit(1);
        }
    }
    printf("\033[1;32m##### result_cpu == result_gpu #####\033[0m\n");
}

void initialize_optix(ScanState &state) {
    // Initialize CUDA
    CUDA_CHECK(cudaFree(0));

    // Initialize the OptiX API, loading all API entry points
    OPTIX_CHECK(optixInit());

    // Specify context options
    OptixDeviceContextOptions options = {};
    options.logCallbackFunction = &context_log_cb;
    options.logCallbackLevel = 4;

    // Associate a CUDA context (and therefore a specific GPU) with this
    // device context
    CUcontext cuCtx = 0; // zero means take the current context
    OPTIX_CHECK(optixDeviceContextCreate(cuCtx, &options, &state.context));
}

void make_gas(ScanState &state) {
    // Use default options for simplicity.  In a real use case we would want to
    // enable compaction, etc
    OptixAccelBuildOptions accel_options = {};
    accel_options.buildFlags = OPTIX_BUILD_FLAG_ALLOW_COMPACTION;
    accel_options.operation = OPTIX_BUILD_OPERATION_BUILD;

    OptixBuildInput vertex_input = {};

#if PRIMITIVE_TYPE == 0
    printf("[OptiX] PRIMITIVE_TYPE: Triangle\n");
    const size_t triangle_vertices_size = 3 * state.length * sizeof(float3);
    state.triangle_vertices = (float3 *) malloc(triangle_vertices_size);
    vertices_to_triangles(state, state.vertices, state.triangle_vertices);
    
    float3* d_triangles;
    CUDA_CHECK(cudaMalloc(reinterpret_cast<void**>(&d_triangles), triangle_vertices_size));
    CUDA_CHECK(cudaMemcpy(
        reinterpret_cast<void*>(d_triangles),
        state.triangle_vertices,
        triangle_vertices_size,
        cudaMemcpyHostToDevice));
    free(state.triangle_vertices);
    CUdeviceptr d_triangle_ptr = reinterpret_cast<CUdeviceptr>(d_triangles);
    state.params.d_triangles   = d_triangles;

    // Our build input is a simple list of non-indexed triangle vertices
    const uint32_t triangle_input_flags[1]      = {OPTIX_GEOMETRY_FLAG_REQUIRE_SINGLE_ANYHIT_CALL};
    vertex_input.type                           = OPTIX_BUILD_INPUT_TYPE_TRIANGLES;
    vertex_input.triangleArray.vertexFormat     = OPTIX_VERTEX_FORMAT_FLOAT3;
    vertex_input.triangleArray.numVertices      = 3 * state.length;
    vertex_input.triangleArray.vertexBuffers    = &d_triangle_ptr;
    vertex_input.triangleArray.vertexStrideInBytes = sizeof(float3);
    vertex_input.triangleArray.flags            = triangle_input_flags;
    vertex_input.triangleArray.numSbtRecords    = 1;

#else
    printf("[OptiX] PRIMITIVE_TYPE: Cube\n");
    OptixAabb* d_aabb;
    unsigned int numPrims = state.length;
    CUDA_CHECK(cudaMalloc(reinterpret_cast<void**>(&d_aabb), numPrims * sizeof(OptixAabb)));
    kGenAABB(state.params.points, state.params.aabb_width / 2, numPrims, d_aabb);
    CUdeviceptr d_aabb_ptr = reinterpret_cast<CUdeviceptr>(d_aabb);
    state.params.d_aabb    = d_aabb;

    // Our build input is a simple list of non-indexed triangle vertices
    const uint32_t vertex_input_flags[1] = {OPTIX_GEOMETRY_FLAG_NONE};
    vertex_input.type = OPTIX_BUILD_INPUT_TYPE_CUSTOM_PRIMITIVES;
    vertex_input.customPrimitiveArray.aabbBuffers = &d_aabb_ptr;
    vertex_input.customPrimitiveArray.flags = vertex_input_flags;
    vertex_input.customPrimitiveArray.numSbtRecords = 1;
    vertex_input.customPrimitiveArray.numPrimitives = numPrims;
    // it's important to pass 0 to sbtIndexOffsetBuffer
    vertex_input.customPrimitiveArray.sbtIndexOffsetBuffer = 0;
    vertex_input.customPrimitiveArray.sbtIndexOffsetSizeInBytes = sizeof(uint32_t);
    vertex_input.customPrimitiveArray.primitiveIndexOffset = 0;
#endif

    OptixAccelBufferSizes gas_buffer_sizes;
    OPTIX_CHECK(optixAccelComputeMemoryUsage(
        state.context,
        &accel_options,
        &vertex_input,
        1, // Number of build inputs
        &gas_buffer_sizes));
    CUdeviceptr d_temp_buffer_gas;
    CUDA_CHECK(cudaMalloc(
        reinterpret_cast<void **>(&d_temp_buffer_gas),
        gas_buffer_sizes.tempSizeInBytes));

    // non-compacted output and size of compacted GAS.
    CUdeviceptr d_buffer_temp_output_gas_and_compacted_size;
    size_t compactedSizeOffset = roundUp<size_t>(gas_buffer_sizes.outputSizeInBytes, 8ull);
    CUDA_CHECK(cudaMalloc(
        reinterpret_cast<void **>(&d_buffer_temp_output_gas_and_compacted_size),
        compactedSizeOffset + 8));

    OptixAccelEmitDesc emitProperty = {};
    emitProperty.type = OPTIX_PROPERTY_TYPE_COMPACTED_SIZE;
    emitProperty.result = (CUdeviceptr)((char *)d_buffer_temp_output_gas_and_compacted_size + compactedSizeOffset);

    OPTIX_CHECK(optixAccelBuild(
        state.context,
        0, // CUDA stream
        &accel_options,
        &vertex_input,
        1, // num build inputs
        d_temp_buffer_gas,
        gas_buffer_sizes.tempSizeInBytes,
        d_buffer_temp_output_gas_and_compacted_size,
        gas_buffer_sizes.outputSizeInBytes,
        &state.gas_handle,
        &emitProperty, // emitted property list
        1              // num emitted properties
        ));

    // We can now free the scratch space buffer used during build and the vertex
    // inputs, since they are not needed by our trivial shading method
    CUDA_CHECK(cudaFree(reinterpret_cast<void *>(d_temp_buffer_gas)));
    // CUDA_CHECK(cudaFree(reinterpret_cast<void *>(d_vertices)));  // 放到了最后来释放 params.points 的空间

    size_t compacted_gas_size;
    CUDA_CHECK(cudaMemcpyAsync(&compacted_gas_size, (void *)emitProperty.result, sizeof(size_t), cudaMemcpyDeviceToHost, 0));
    if (compacted_gas_size < gas_buffer_sizes.outputSizeInBytes) {
        // compacted size is smaller, so store the compacted GAS in new device memory and free the original GAS memory/
        CUDA_CHECK(cudaMalloc(reinterpret_cast<void**>(&state.d_gas_output_buffer), compacted_gas_size));

        // use handle as input and output
        OPTIX_CHECK(optixAccelCompact(state.context, 0, state.gas_handle, state.d_gas_output_buffer, compacted_gas_size, &state.gas_handle));

        CUDA_CHECK(cudaFree((void*)d_buffer_temp_output_gas_and_compacted_size));
    } else {
        // original size is smaller, so point d_gas_output_buffer directly to the original device GAS memory.
        state.d_gas_output_buffer = d_buffer_temp_output_gas_and_compacted_size;
    }
    fprintf(stdout, "Final GAS size: %f MB\n", (double)compacted_gas_size / (1024 * 1024));
}

void make_module(ScanState &state) {
    char log[2048];

    OptixModuleCompileOptions module_compile_options = {};
#if !defined(NDEBUG)
    module_compile_options.optLevel = OPTIX_COMPILE_OPTIMIZATION_LEVEL_0;
    module_compile_options.debugLevel = OPTIX_COMPILE_DEBUG_LEVEL_FULL;
#endif

    state.pipeline_compile_options.usesMotionBlur = false;
    state.pipeline_compile_options.traversableGraphFlags = OPTIX_TRAVERSABLE_GRAPH_FLAG_ALLOW_SINGLE_GAS;
    state.pipeline_compile_options.numPayloadValues = 3;
    state.pipeline_compile_options.numAttributeValues = 0;
#ifdef DEBUG // Enables debug exceptions during optix launches. This may incur significant performance cost and should only be done during development.
    pipeline_compile_options.exceptionFlags = OPTIX_EXCEPTION_FLAG_DEBUG | OPTIX_EXCEPTION_FLAG_TRACE_DEPTH | OPTIX_EXCEPTION_FLAG_STACK_OVERFLOW;
#else
    state.pipeline_compile_options.exceptionFlags = OPTIX_EXCEPTION_FLAG_NONE;
#endif
    state.pipeline_compile_options.pipelineLaunchParamsVariableName = "params";
    // By default (usesPrimitiveTypeFlags == 0) it supports custom and triangle primitives
#if PRIMITIVE_TYPE == 0
    state.pipeline_compile_options.usesPrimitiveTypeFlags = OPTIX_PRIMITIVE_TYPE_FLAGS_TRIANGLE;
#else
    state.pipeline_compile_options.usesPrimitiveTypeFlags = OPTIX_PRIMITIVE_TYPE_FLAGS_CUSTOM;
#endif

    size_t inputSize = 0;
    const char *input = sutil::getInputData(OPTIX_SAMPLE_NAME, OPTIX_SAMPLE_DIR, "optixScan.cu", inputSize);
    size_t sizeof_log = sizeof(log);

    OPTIX_CHECK_LOG(optixModuleCreateFromPTX(
        state.context,
        &module_compile_options,
        &state.pipeline_compile_options,
        input,
        inputSize,
        log,
        &sizeof_log,
        &state.module));
}

void make_program_groups(ScanState &state) {
    char log[2048];

    OptixProgramGroupOptions program_group_options = {}; // Initialize to zeros

    OptixProgramGroupDesc raygen_prog_group_desc = {};
    raygen_prog_group_desc.kind = OPTIX_PROGRAM_GROUP_KIND_RAYGEN;
    raygen_prog_group_desc.raygen.module = state.module;
    raygen_prog_group_desc.raygen.entryFunctionName = "__raygen__rg";
    size_t sizeof_log = sizeof(log);
    OPTIX_CHECK_LOG(optixProgramGroupCreate(
        state.context,
        &raygen_prog_group_desc,
        1, // num program groups
        &program_group_options,
        log,
        &sizeof_log,
        &state.raygen_prog_group));

    OptixProgramGroupDesc miss_prog_group_desc = {};
    miss_prog_group_desc.kind = OPTIX_PROGRAM_GROUP_KIND_MISS;
    miss_prog_group_desc.miss.module = state.module;
    miss_prog_group_desc.miss.entryFunctionName = "__miss__ms";
    sizeof_log = sizeof(log);
    OPTIX_CHECK_LOG(optixProgramGroupCreate(
        state.context,
        &miss_prog_group_desc,
        1, // num program groups
        &program_group_options,
        log,
        &sizeof_log,
        &state.miss_prog_group));

    OptixProgramGroupDesc hitgroup_prog_group_desc = {};
    hitgroup_prog_group_desc.kind = OPTIX_PROGRAM_GROUP_KIND_HITGROUP;
#if PRIMITIVE_TYPE == 0
    hitgroup_prog_group_desc.hitgroup.moduleAH = state.module;
    hitgroup_prog_group_desc.hitgroup.entryFunctionNameAH = "__anyhit__get_prim_id";    
#else
    hitgroup_prog_group_desc.hitgroup.moduleIS = state.module;
    hitgroup_prog_group_desc.hitgroup.entryFunctionNameIS = "__intersection__cube";
#endif
                               
      
    sizeof_log = sizeof(log);
    OPTIX_CHECK_LOG(optixProgramGroupCreate(
        state.context,
        &hitgroup_prog_group_desc,
        1, // num program groups
        &program_group_options,
        log,
        &sizeof_log,
        &state.hitgroup_prog_group));
}

void make_pipeline(ScanState &state) {
    char log[2048];
    const uint32_t max_trace_depth = 1;
    std::vector<OptixProgramGroup> program_groups{state.raygen_prog_group, state.miss_prog_group, state.hitgroup_prog_group};

    OptixPipelineLinkOptions pipeline_link_options = {};
    pipeline_link_options.maxTraceDepth = max_trace_depth;
    pipeline_link_options.debugLevel = OPTIX_COMPILE_DEBUG_LEVEL_FULL;
    size_t sizeof_log = sizeof(log);
    OPTIX_CHECK_LOG(optixPipelineCreate(
        state.context,
        &state.pipeline_compile_options,
        &pipeline_link_options,
        program_groups.data(),
        program_groups.size(),
        log,
        &sizeof_log,
        &state.pipeline));

    OptixStackSizes stack_sizes = {};
    for (auto &prog_group : program_groups) {
        OPTIX_CHECK(optixUtilAccumulateStackSizes(prog_group, &stack_sizes));
    }

    uint32_t direct_callable_stack_size_from_traversal;
    uint32_t direct_callable_stack_size_from_state;
    uint32_t continuation_stack_size;
    OPTIX_CHECK(optixUtilComputeStackSizes(&stack_sizes, max_trace_depth,
                                           0, // maxCCDepth
                                           0, // maxDCDEpth
                                           &direct_callable_stack_size_from_traversal,
                                           &direct_callable_stack_size_from_state, &continuation_stack_size));
    OPTIX_CHECK(optixPipelineSetStackSize(state.pipeline, direct_callable_stack_size_from_traversal,
                                          direct_callable_stack_size_from_state, continuation_stack_size,
                                          1 // maxTraversableDepth
                                          ));
}

void make_sbt(ScanState &state) {
    CUdeviceptr raygen_record;
    const size_t raygen_record_size = sizeof(RayGenSbtRecord);
    CUDA_CHECK(cudaMalloc(reinterpret_cast<void **>(&raygen_record), raygen_record_size));
    RayGenSbtRecord rg_sbt;
    OPTIX_CHECK(optixSbtRecordPackHeader(state.raygen_prog_group, &rg_sbt));
    CUDA_CHECK(cudaMemcpy(
        reinterpret_cast<void *>(raygen_record),
        &rg_sbt,
        raygen_record_size,
        cudaMemcpyHostToDevice));

    CUdeviceptr miss_record;
    size_t miss_record_size = sizeof(MissSbtRecord);
    CUDA_CHECK(cudaMalloc(reinterpret_cast<void **>(&miss_record), miss_record_size));
    MissSbtRecord ms_sbt;
    OPTIX_CHECK(optixSbtRecordPackHeader(state.miss_prog_group, &ms_sbt));
    CUDA_CHECK(cudaMemcpy(
        reinterpret_cast<void *>(miss_record),
        &ms_sbt,
        miss_record_size,
        cudaMemcpyHostToDevice));

    CUdeviceptr hitgroup_record;
    size_t hitgroup_record_size = sizeof(HitGroupSbtRecord);
    CUDA_CHECK(cudaMalloc(reinterpret_cast<void **>(&hitgroup_record), hitgroup_record_size));
    HitGroupSbtRecord hg_sbt;
    // hg_sbt.data = predicate; // copy data
    OPTIX_CHECK(optixSbtRecordPackHeader(state.hitgroup_prog_group, &hg_sbt));
    CUDA_CHECK(cudaMemcpy(
        reinterpret_cast<void *>(hitgroup_record),
        &hg_sbt,
        hitgroup_record_size,
        cudaMemcpyHostToDevice));

    state.sbt.raygenRecord = raygen_record;
    state.sbt.missRecordBase = miss_record;
    state.sbt.missRecordStrideInBytes = sizeof(MissSbtRecord);
    state.sbt.missRecordCount = 1;
    state.sbt.hitgroupRecordBase = hitgroup_record;
    state.sbt.hitgroupRecordStrideInBytes = sizeof(HitGroupSbtRecord);
    state.sbt.hitgroupRecordCount = 1;
}

void initialize_params(ScanState &state) {
    CUDA_CHECK(cudaMalloc(reinterpret_cast<void **>(&state.params.result), state.result_byte_num));
    CUDA_CHECK(cudaMemset(state.params.result, 0, state.result_byte_num));

    // `ray_primitive_hits` is used to DEBUG
    CUDA_CHECK(cudaMalloc(reinterpret_cast<void **>(&state.params.ray_primitive_hits), state.width * state.height * sizeof(unsigned int)));
    CUDA_CHECK(cudaMemset(state.params.ray_primitive_hits, 0, state.width * state.height * sizeof(unsigned int)));

    // result bit vector
    CUDA_CHECK(cudaMallocHost(reinterpret_cast<void **>(&state.h_result), state.result_byte_num)); // TODO: pinned memory

    // intersection_test_num
    CUDA_CHECK(cudaMalloc(reinterpret_cast<void **>(&state.params.intersection_test_num), sizeof(unsigned int)));
    CUDA_CHECK(cudaMemset(state.params.intersection_test_num, 0, sizeof(unsigned int)));
}

void display_results(ScanState &state, BITS *result_cpu, double *p, unsigned init_hit_num) {
    unsigned int *intersection_test_num;
    CUDA_CHECK(cudaMallocHost(reinterpret_cast<void **>(&intersection_test_num), sizeof(unsigned int)));
    CUDA_CHECK(cudaMemcpy(
        reinterpret_cast<void *>(intersection_test_num),
        state.params.intersection_test_num,
        sizeof(unsigned int),
        cudaMemcpyDeviceToHost));
    fprintf(stdout, "[OptiX] intersection_test_num: %u\n", *intersection_test_num);
    unsigned int *hit_num;
    CUDA_CHECK(cudaMallocHost(reinterpret_cast<void **>(&hit_num), sizeof(unsigned int)));
    CUDA_CHECK(cudaMemcpy(
        reinterpret_cast<void *>(hit_num),
        state.params.hit_num,
        sizeof(unsigned int),
        cudaMemcpyDeviceToHost));
    fprintf(stdout, "[OptiX] hit_num: %u\n", *hit_num);
    
    CUDA_CHECK(cudaMallocHost(reinterpret_cast<void **>(&state.h_result), state.result_byte_num));
    CUDA_CHECK(cudaMemcpy(
        reinterpret_cast<void *>(state.h_result),
        state.params.result,
        state.result_byte_num,
        cudaMemcpyDeviceToHost));

    // Obtain the number of hit points from the result bit vector calculated by #CPU#
    unsigned int point_num_cpu = 0;
    for (int i = 0; i < state.length; i++) {
        int pos_in_unsigned_val = 1 << (31 - i);
        if (result_cpu[i / 32] & pos_in_unsigned_val) {
            point_num_cpu++;
        }
    }
    fprintf(stdout, "[OptiX] result_cpu_point_num: %u\n", point_num_cpu);
    
    // Obtain the number of hit points from the result bit vector calculated by #GPU#
    unsigned int point_num = 0;
    for (int i = 0; i < state.length; i++) {
        int pos_in_unsigned_val = 1 << (31 - i);
        if (state.h_result[i / 32] & pos_in_unsigned_val) {
            point_num++;
        }
    }
    fprintf(stdout, "[OptiX] result_gpu_point_num: %u\n", point_num);
    fprintf(stdout, "[OptiX] actually refine num: %u\n", point_num > init_hit_num ? point_num - init_hit_num : init_hit_num - point_num);
    
    compare_result(result_cpu, state.h_result, state.length, p, state.params.inverse, state.column_num);
    CUDA_CHECK(cudaFreeHost(state.h_result));
    CUDA_CHECK(cudaFreeHost(intersection_test_num));
    CUDA_CHECK(cudaFreeHost(hit_num));
    state.h_result = nullptr;
}

void display_ray_hits(ScanState &state) {
    int ray_num = state.launch_width * state.launch_height * state.depth;
    unsigned int *ray_hits = (unsigned int *) malloc(sizeof(unsigned int) * ray_num);
    CUDA_CHECK(cudaMemcpy(ray_hits, state.params.ray_primitive_hits, sizeof(unsigned int) * ray_num, cudaMemcpyDeviceToHost));
    std::map<unsigned int, int> hitNum_rayNum;
    int sum = 0;
    for (int i = 0; i < ray_num; i++) {
        sum += ray_hits[i];
        if (hitNum_rayNum.count(ray_hits[i])) {
            hitNum_rayNum[ray_hits[i]]++;
        } else {
            hitNum_rayNum[ray_hits[i]] = 1;
        }       
    }

    int min, max, median = -1;
    double avg;
    int tmp_sum = 0;
    min = hitNum_rayNum.begin()->first;
    max = (--hitNum_rayNum.end())->first;
    avg = 1.0 * sum / ray_num;
    printf("光线穿过 cube 数: 对应光线数\n");
    for (auto &item: hitNum_rayNum) {
        fprintf(stdout, "%d: %d\n", item.first, item.second);
        tmp_sum += item.second;
        if (median == -1 && tmp_sum >= ray_num / 2) {
            median = item.first;
        }
    }
    printf("min: %d, max: %d, average: %lf, median: %d\n", min, max, avg, median);
    free(ray_hits);

    // first hit, next hit
    int first_hit, next_hit;
    if (min == 0) {
        first_hit = ray_num - hitNum_rayNum.begin()->second;
    } else {
        first_hit = ray_num;
    }
    next_hit = sum - first_hit;
    printf("first_hit: %d, next_hit: %d\n", first_hit, next_hit);
    printf("active_ray: %d, total intersection: %d\n", first_hit, sum);
    printf("job per ray: %lf\n", 1.0 * sum / first_hit);
    int real_next_hit = 0;
    for (auto it = ++(++hitNum_rayNum.begin()); it != hitNum_rayNum.end(); it++) {
        real_next_hit += it->second;
    }
    printf("real_next_hit: %d\n", real_next_hit);
}

void cleanup(ScanState &state) {
    // free host memory

    // free device memory
    CUDA_CHECK(cudaFree(reinterpret_cast<void *>(state.sbt.raygenRecord)));
    CUDA_CHECK(cudaFree(reinterpret_cast<void *>(state.sbt.missRecordBase)));
    CUDA_CHECK(cudaFree(reinterpret_cast<void *>(state.sbt.hitgroupRecordBase)));
    CUDA_CHECK(cudaFree(reinterpret_cast<void *>(state.d_gas_output_buffer)));

    OPTIX_CHECK(optixPipelineDestroy(state.pipeline));
    OPTIX_CHECK(optixProgramGroupDestroy(state.hitgroup_prog_group));
    OPTIX_CHECK(optixProgramGroupDestroy(state.miss_prog_group));
    OPTIX_CHECK(optixProgramGroupDestroy(state.raygen_prog_group));
    OPTIX_CHECK(optixModuleDestroy(state.module));

    OPTIX_CHECK(optixDeviceContextDestroy(state.context));
}

void log_common_info(ScanState &state) {
    printf("max ray num:                %dx%dx%d\n", state.width, state.height, state.depth);
    printf("data num:                   %d\n", state.length);
    printf("aabb_width                  %f\n", state.params.aabb_width);
    printf("[OptiX] ray_interval: %lf\n", state.params.ray_interval);
    fprintf(stdout, "[OptiX] range: ");
    for (int i = 0; i < 6; i++) {
        fprintf(stdout, "%u ", state.range[i]);
    }
    fprintf(stdout, "\n");
    printf("epsilon: %d\n", state.params.epsilon);
}

void checkoptixScanOnline() {
    std::cout << "[OptiX]optix Scan online now." << std::endl;
}

void get_epsilon(int &epsilon, CODE data_max) {
    epsilon = (data_max >> 24) << 3; // 2^24: precision of float representing int
    if (epsilon == 0) {
        return;
    }
    CODE and_val = 0;
    for (int i = 0; i < 32; i++) {
        and_val = 1 << (31 - i);
        if (epsilon & and_val) {
            break;
        }
    }
    epsilon = and_val;
}

void initializeOptixRTc1(CODE **raw_data, int length, int density_width, int density_height, int column_num, CODE *range, int cube_width_factor, 
                         int ray_interval, int prim_size) {
    fprintf(stdout, "[OptiX]initializeOptix begin...\n");
    state.column_num = column_num;
    state.length = length;
    state.width = density_width;
    state.height = density_height;
    state.vertices = (double3 *) malloc(length * sizeof(double3));
    state.result_byte_num = ((state.length - 1) / 32 + 1) * 4;
    uint32_to_double3(state.vertices, raw_data, length, column_num);
    state.range = range;
    CODE data_min, data_max;
    data_min = state.range[0];
    data_max = state.range[1];
    for (int i = 1; i < column_num; i++) {
        if (state.range[2 * i] < data_min) {
            data_min = state.range[2 * i];
        }
        if (state.range[2 * i + 1] > data_max) {
            data_max = state.range[2 * i + 1];
        }
    }

    if (ray_interval == -1) {
        if (cube_width_factor == -1) {
            state.params.aabb_width = (data_max - data_min) / state.width + 1.0f;
        } else {
            state.params.aabb_width = (data_max - data_min) / cube_width_factor + 1.0f;
        }
        state.params.ray_interval = (data_max - data_min) / state.width + 1.0f;
    } else {
        if (prim_size == -1) {
            state.params.aabb_width = ray_interval;
        } else {
            state.params.aabb_width = prim_size;
        }
        state.params.ray_interval = ray_interval;
    }

    get_epsilon(state.params.epsilon, data_max);
    
    // intersection_test_num, hit_num, predicate
    CUDA_CHECK(cudaMalloc(&state.params.intersection_test_num, sizeof(unsigned int)));
    CUDA_CHECK(cudaMalloc(&state.params.hit_num, sizeof(unsigned int)));
    CUDA_CHECK(cudaMalloc(&state.params.predicate, 6 * sizeof(double)));
    
    CUdeviceptr d_vertices = 0;
    CUDA_CHECK(cudaMalloc(reinterpret_cast<void**>(&d_vertices), state.length * sizeof(double3)));
    CUDA_CHECK(cudaMemcpy(
        reinterpret_cast<void*>(d_vertices),
        state.vertices,
        state.length * sizeof(double3),
        cudaMemcpyHostToDevice));
    state.params.points = reinterpret_cast<double3*>(d_vertices);

    log_common_info(state);

    timer_.commonGetStartTime(0); // record gas time
    initialize_optix(state);
    make_gas(state);
    make_module(state);
    make_program_groups(state);
    make_pipeline(state); // Link pipeline
    make_sbt(state);
    timer_.commonGetEndTime(0);
    timer_.showTime(0, "initializeOptix");
    fprintf(stdout, "[OptiX]initializeOptix end\n");
}

// called by bindex
// direction = (x = 0, y = 1, z = 2)
// ray_mode = 0 for continuous ray, 1 for ray with space, 2 for ray as point
void refineWithOptixRTc1(BITS *dev_result_bitmap, double *predicate, int column_num, 
                         int ray_length, int ray_segment_num, bool inverse, int direction, int ray_mode) {
#if DEBUG_INFO == 1
    timer_.commonGetStartTime(1);
#endif

    for (int i = 0; i < 3; i++) {
        if (predicate[i * 2] >= predicate[i * 2 + 1]) {
            return;
        }
    }
    double prange[3] = {
        predicate[1] - predicate[0],
        predicate[3] - predicate[2],
        predicate[5] - predicate[4]
    };

#if DEBUG_ISHIT_CMP_RAY == 1
    unsigned int point_num_cpu;
    BITS* result_cpu;
    int uint_num = (state.length + 32 - 1) / 32;
    BITS* host_ptr = (BITS*)malloc(sizeof(BITS) * uint_num);
    cudaMemcpy(host_ptr, dev_result_bitmap, 4 * uint_num, cudaMemcpyDeviceToHost);
    point_num_cpu = 0;
    for (int i = 0; i < state.length; i++) {
        int pos_in_unsigned_val = 1 << (31 - i);
        if (host_ptr[i / 32] & pos_in_unsigned_val) {
            point_num_cpu++;
        }
    }
    fprintf(stdout, "[OptiX] initial point num: %u\n", point_num_cpu);
    free(host_ptr);

    fprintf(stdout, "[OptiX] scan with cpu...\n");
    result_cpu = (BITS*)malloc(sizeof(BITS) * uint_num);
    CUDA_CHECK(cudaMemcpy(result_cpu, dev_result_bitmap, sizeof(BITS) * uint_num, cudaMemcpyDeviceToHost));
    scan_with_cpu(state.vertices, state.length, result_cpu, predicate, inverse, column_num);
    
    CUDA_CHECK(cudaMemset(state.params.intersection_test_num, 0, sizeof(unsigned)));
    CUDA_CHECK(cudaMemset(state.params.hit_num, 0, sizeof(unsigned)));
#endif

    double predicate_range;
    if (direction == 0) {
        predicate_range = prange[0];
        state.launch_width  = (int) (prange[1] / state.params.ray_interval) + 1;
        state.launch_height = (int) (prange[2] / state.params.ray_interval) + 1;
    } else if (direction == 1) {
        predicate_range = prange[1];
        state.launch_width  = (int) (prange[0] / state.params.ray_interval) + 2; // +2 is for without sieve
        state.launch_height = (int) (prange[2] / state.params.ray_interval) + 1;
    } else {
        predicate_range = prange[2];
        state.launch_width  = (int) (prange[0] / state.params.ray_interval) + 1;
        state.launch_height = (int) (prange[1] / state.params.ray_interval) + 1;
    }

    if (ray_length == -1) { // Launch rays based on ray_segment_num
        if (ray_mode == 0) {
            state.params.ray_stride = predicate_range / ray_segment_num;
            state.params.ray_space  = 0.0;
            state.params.ray_length = state.params.ray_stride - state.params.ray_space;
            state.depth             = ray_segment_num;
            state.params.ray_last_length = predicate_range - (state.depth - 1) * state.params.ray_stride;
        } else if (ray_mode == 1) {
            state.params.ray_stride = predicate_range / ray_segment_num;
            state.depth             = ray_segment_num;
            if (state.params.ray_stride <= state.params.aabb_width) {
                state.params.ray_length = 1e-5;
                state.params.ray_space  = predicate_range / state.depth;
            } else {
                state.params.ray_length = state.params.ray_stride - state.params.aabb_width;
                state.params.ray_space  = state.params.aabb_width;
            }
        } else if (ray_mode == 2) {
            // Recalculate ray segment with fixed ray stride
            state.params.ray_length = 1e-5;
            state.params.ray_space  = state.params.aabb_width;
            state.params.ray_stride = state.params.ray_length + state.params.ray_space;
            state.depth             = (int) (predicate_range / state.params.ray_stride) + 1;
        } else {
            printf("ray_mode(%d) is not valid!\n", ray_mode);
            fflush(stdout);
            exit(1);
        }
    } else { // Launch rays based on ray_length
        if (ray_mode == 0) { // Continuous rays
            state.params.ray_space  = 0.0;
        } else if (ray_mode == 1) { // Rays with space
            state.params.ray_space  = state.params.aabb_width;
        } else {
            printf("ray_mode(%d) is not valid for launching rays based on 'ray_length'\n", ray_mode);
            fflush(stdout);
            exit(1);
        }
        state.params.ray_length = ray_length;
        state.params.ray_stride = state.params.ray_length + state.params.ray_space;
        state.depth             = (int) (predicate_range / state.params.ray_stride);
        if (state.depth * state.params.ray_stride < predicate_range) {
            double last_stride = predicate_range - state.depth * state.params.ray_stride;
            state.params.ray_last_length = max(last_stride - state.params.ray_space, 0.0);
            state.depth++;
        } else {
            state.params.ray_last_length = state.params.ray_length;
        }
    }

    state.params.result = dev_result_bitmap;
    state.params.direction = direction;
    state.params.handle = state.gas_handle;
    state.params.inverse = inverse;
#if DEBUG_INFO == 1
    printf("[OptiX] ray_mode: %d\n", ray_mode);
    printf("[OptiX] predicate_range: %lf\n", predicate_range);
    printf("[OptiX] ray_stride: %lf\n", state.params.ray_stride);
    printf("[OptiX] ray_length: %lf\n", state.params.ray_length);
    printf("[OptiX] ray_last_length: %lf\n", state.params.ray_last_length);
    printf("[OptiX] ray_space: %lf\n", state.params.ray_space);
    printf("[OptiX] ray_segment_num: %d\n", ray_segment_num);
    printf("[OptiX] ray_interval: %lf\n", state.params.ray_interval);
    printf("[OptiX] inverse: %s\n", inverse ? "true" : "false");
    printf("[OptiX] direction: %d\n", direction);
    printf("[OptiX] launch_width = %d, launch_height = %d, depth = %d, total ray num = %d\n", state.launch_width, state.launch_height, state.depth, state.launch_width * state.launch_height * state.depth);
#endif

#ifdef DEBUG_RAY
    CUDA_CHECK(cudaMalloc(reinterpret_cast<void **>(&state.params.ray_primitive_hits), sizeof(unsigned) * state.launch_width * state.launch_height * state.depth));
#endif
    CUDA_CHECK(cudaMemcpy(state.params.predicate, predicate, 6 * sizeof(double), cudaMemcpyHostToDevice));
    
    //************
    //* Memcpy Params
    //************
    CUDA_CHECK(cudaMalloc(reinterpret_cast<void **>(&state.d_params), sizeof(Params))); // Cannot malloc in initializeOptix phase
    CUDA_CHECK(cudaMemcpy(
        reinterpret_cast<void *>(state.d_params),
        &state.params,
        sizeof(Params),
        cudaMemcpyHostToDevice));

#if DEBUG_INFO == 1
    timer_.commonGetStartTime(2);
#endif
    OPTIX_CHECK(optixLaunch(state.pipeline, 0, state.d_params, sizeof(Params), &state.sbt, state.launch_width, state.launch_height, state.depth));
    CUDA_SYNC_CHECK();
#if DEBUG_INFO == 1
    timer_.commonGetEndTime(2);
    timer_.commonGetEndTime(1);
#endif

#if DEBUG_INFO == 1
    timer_.showTime(1, "refineWithOptix");
    timer_.showTime(2, "optixLaunch");
    timer_.clear();
    fprintf(stdout, "[OptiX] refineWithOptix end\n");
#endif
    
#if DEBUG_ISHIT_CMP_RAY == 1
    display_results(state, result_cpu, predicate, point_num_cpu);
#endif
#ifdef DEBUG_RAY
    display_ray_hits(state);
#endif
    
    // cleanup
    CUDA_CHECK(cudaFree((void *)state.d_params));
#ifdef DEBUG_RAY
    CUDA_CHECK(cudaFree(state.params.ray_primitive_hits));
#endif
}